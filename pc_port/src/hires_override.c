#include "hires_override.h"
#include "dds_load.h"
#include "texpack_lazy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"

/* PNG overrides carry a true 8-bit alpha channel (TIM transparency is
 * 1-bit: raw value 0 = transparent). stb_image is vendored, PNG-only,
 * memory-only; its default allocator is malloc so the shared free() below
 * is valid for both decode paths.
 *
 * The IDAT inflate is hooked out to miniz (already vendored for .zip packs):
 * on the 200-file pack sample it cuts a whole stbi_load_from_memory from
 * 2.519 to 1.973 ms/file (-21.7%), no new dependency. Output matched stb's
 * on all 400 real pack PNGs; the hook also deliberately tolerates a bad
 * trailing adler32 (see Png_Inflate) so it cannot reject an image stb would
 * have accepted. Only the inflate is swapped — stb still does filters,
 * palette expansion, tRNS and 16-bit reduction, which is what makes its
 * output the reference. */
static char* Png_Inflate(const char* buf, int len, int initialSize, int* outLen, int parseHeader);
#define STBI_PNG_ZLIB_DECODE Png_Inflate

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

/* stb's raw_len guess is the exact decoded size for a non-interlaced PNG, so
 * the common case decodes straight into a right-sized buffer and never grows.
 * Interlaced PNGs (whose Adam7 filter bytes overrun that guess) fall back to
 * the growing heap decode. stb frees the result with STBI_FREE == free, which
 * is what miniz's MZ_MALLOC/MZ_REALLOC use here.
 *
 * ADLER32: stb's own inflate never checks the zlib trailer, so it happily
 * decodes a stream whose adler32 is wrong or truncated. miniz's mem_to_mem /
 * mem_to_heap wrappers DO check it and fail the whole call, which would make
 * this drop-in stricter than what it replaces and silently vanish a texture
 * that used to load. Drive tinfl_decompress directly instead and accept
 * ADLER32_MISMATCH alongside DONE. The heap fallback below still goes through
 * miniz's wrapper, so a bad adler32 can only bite an INTERLACED PNG (the one
 * shape that overruns stb's size guess) — no such file exists in any known
 * pack, and the non-interlaced path above covers everything else. */
static char* Png_Inflate(const char* buf, int len, int initialSize, int* outLen, int parseHeader)
{
    int flags = parseHeader ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0;

    if (initialSize > 0)
    {
        char* out = (char*)malloc((size_t)initialSize);
        if (out != NULL)
        {
            /* ~11KB; malloc'd rather than stacked so this stays safe on the
             * queue/post-load call depth. */
            tinfl_decompressor* decomp =
                (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
            if (decomp != NULL)
            {
                size_t       inN  = (size_t)len;
                size_t       outN = (size_t)initialSize;
                tinfl_status st;

                tinfl_init(decomp);
                st = tinfl_decompress(decomp, (const mz_uint8*)buf, &inN,
                                      (mz_uint8*)out, (mz_uint8*)out, &outN,
                                      flags | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
                free(decomp);

                if (st == TINFL_STATUS_DONE || st == TINFL_STATUS_ADLER32_MISMATCH)
                {
                    *outLen = (int)outN;
                    return out;
                }
            }
            free(out);
        }
    }

    {
        size_t outN = 0;
        void*  p    = tinfl_decompress_mem_to_heap(buf, (size_t)len, &outN, flags);
        if (p == NULL)
        {
            /* stb's contract: a failing zlib decode must leave an error string,
             * or stbi_failure_reason() reports whatever failed last instead. */
            stbi__err("PNG inflate failed", "Corrupt PNG");
            return NULL;
        }
        *outLen = (int)outN;
        return (char*)p;
    }
}

#include "sh_log.h"
#include "pc_config.h"

#include <PsyX/common/glad.h>

#define MAX_HIRES_OVERRIDES 256

typedef struct {
    int      vramX, vramY;       /* 16-bit cells in PSX VRAM (match key) */
    int      vramW, vramH;
    int      clutX, clutY;       /* CLUT cell coords, -1 if no CLUT (16bpp) */
    int      originalBitDepth;   /* 4, 8, 16 */
    GLuint   glTexture;
    int      hiresW, hiresH;     /* actual pixel dimensions of decoded RGBA */
    int      sourceBitDepth;     /* TIM mode of the loose file itself */
    unsigned packBytes;          /* GL bytes charged to the pack budget (0 = uncounted) */
    unsigned tick;               /* pump tick this entry was last SAMPLED (LRU key) */
    unsigned long long contentHash; /* TexPack_LastComposeHash of the resident texture; 0 = unknown/always re-upload */
} HiresEntry;

static HiresEntry g_entries[MAX_HIRES_OVERRIDES];
static int        g_numEntries = 0;
static int        g_initialized = 0;

static int upload_rgba(GLuint* tex, const unsigned char* rgba, int w, int h, int nearest);
static void bgr555_to_rgba(unsigned short cx, unsigned char* out);

/* Force GL_NEAREST (point) sampling on the next upload(s), set by the font TIM
 * loader. HD font packs paint gutterless glyph cells edge-to-edge, so bilinear
 * MAG/mipmap sampling bleeds a neighbour glyph's ink across the cell boundary
 * ("ghost text" beside A/O). Point-sampling the font atlas removes the bleed
 * while every other override keeps bilinear. Persistent (not one-shot): a font
 * registers one upload per CLUT row, so the loader sets it around the whole
 * PostLoadTim registration and clears it after. */
static int s_forceNearestUpload = 0;
void HiresOverride_SetForceNearestUpload(int on) { s_forceNearestUpload = on ? 1 : 0; }

/* ---- Texture-pack GL byte budget ------------------------------------------
 * A DuckStation pack composes + uploads a pack-resolution RGBA texture (with
 * mipmaps) for EVERY CLUT row of EVERY claimed page. The whole-town render
 * mode claims most of a street map's pages, which multiplied out to gigabytes
 * and hard-hung the machine (2026-07-12 Levin-house load). Every pack-composed
 * upload is charged here (mip chain ~= 4/3x) and credited when its texture is
 * replaced or deleted; fsqueue_3.c stops composing further rows once the
 * budget is spent, so those rows keep the native disc art. Claims are made
 * nearest-first (Ipd_ChunkMaterialsApply), so the budget favors what is close.
 *
 * The cap is a safety valve against a single multi-GB whole-map load, NOT a
 * memory diet: on a 64-bit build with real VRAM it is meant to be large, so it
 * is config-driven (texpack_budget_mb, default 6 GB, 0 = unlimited). */
static long long g_packBytesLive = 0;

static unsigned pack_bytes_for(int w, int h)
{
    return (unsigned)(((long long)w * h * 4 * 4) / 3);
}

static void pack_credit(unsigned* slot)
{
    g_packBytesLive -= (long long)*slot;
    if (g_packBytesLive < 0) g_packBytesLive = 0;
    *slot = 0;
}

static void pack_charge(unsigned* slot, int w, int h)
{
    pack_credit(slot);
    *slot = pack_bytes_for(w, h);
    g_packBytesLive += (long long)*slot;
}

/* Size the pack budget to what this GPU can actually hold.
 *
 * The shipped `texpack_budget_mb` is a CONSTANT (6144 MB). While this routine
 * only ever LOWERED it, that constant was the effective budget on every card
 * bigger than ~13.6 GB: a 16 GB card computes a 7372 MB ceiling, 6144 is
 * already under it, nothing changes, and the run pins at exactly 6 GB with 8 GB
 * of VRAM still free — the reported "stops loading textures after some point".
 * A constant cannot be right for both a 4 GB laptop and a 24 GB desktop, so
 * when the user has not named a value the budget is DERIVED from the GPU
 * instead and may be raised as well as lowered.
 *
 * An explicit config value still only ever gets lowered, so a user who wrote a
 * small number is respected and 0 ("unlimited") stays unlimited by their own
 * choice. texpackBudgetCeilingMb caps the derivation on shared-memory APUs,
 * where main_pc.c has already limited pack memory against physical RAM — the
 * GPU may report plenty of "VRAM" that is really the same RAM the game runs in.
 *
 * Needs a live GL context, so it runs after PsyX_Initialise. 45% leaves room
 * for the native rows, the PsyCross framebuffers and the driver's working set;
 * with LRU eviction in the pump the budget is now a steady-state working-set
 * target rather than a one-way ratchet, so overshooting it is self-correcting. */
void HiresOverride_ClampBudgetToVram(void)
{
    int dedicatedMb = 0;

    while (glGetError() != GL_NO_ERROR) { }

    {
        /* GL_NVX_gpu_memory_info: total dedicated video memory, in KB. */
        GLint kb = 0;
        glGetIntegerv(0x9047 /* GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX */, &kb);
        if (glGetError() == GL_NO_ERROR && kb > 0)
        {
            dedicatedMb = kb / 1024;
        }
    }

    if (dedicatedMb <= 0)
    {
        /* GL_ATI_meminfo: [0] = total free texture memory, in KB. Free rather
         * than total, so it reads low — acceptable for a clamp that only
         * lowers, and it is the only figure this extension offers. */
        GLint info[4] = { 0, 0, 0, 0 };
        glGetIntegerv(0x87FC /* TEXTURE_FREE_MEMORY_ATI */, info);
        if (glGetError() == GL_NO_ERROR && info[0] > 0)
        {
            dedicatedMb = info[0] / 1024;
        }
    }

    while (glGetError() != GL_NO_ERROR) { }

    if (dedicatedMb <= 0)
    {
        SH_DBG("[TEXPACK] GPU VRAM not reported by this driver — budget left at %d MB",
               g_PcConfig.texpackBudgetMb);
        return;
    }

    {
        int cap = dedicatedMb * 45 / 100;
        if (cap < 256) cap = 256;

        SH_DBG("[TEXPACK] GPU reports %d MB VRAM; pack budget ceiling %d MB", dedicatedMb, cap);

        if (g_PcConfig.texpackBudgetUserSet)
        {
            if (g_PcConfig.texpackBudgetMb > cap)
            {
                SH_DBG("[TEXPACK] GL texture budget capped %d -> %d MB (45%% of %d MB VRAM)",
                       g_PcConfig.texpackBudgetMb, cap, dedicatedMb);
                g_PcConfig.texpackBudgetMb = cap;
            }
            return;
        }

        /* No user value: the GPU decides, up or down. */
        {
            int want = cap;
            if (g_PcConfig.texpackBudgetCeilingMb > 0 &&
                want > g_PcConfig.texpackBudgetCeilingMb)
            {
                want = g_PcConfig.texpackBudgetCeilingMb;
                SH_DBG("[TEXPACK] VRAM-derived budget held at the %d MB system-RAM ceiling",
                       want);
            }
            if (want != g_PcConfig.texpackBudgetMb)
            {
                SH_DBG("[TEXPACK] GL texture budget %d -> %d MB (45%% of %d MB VRAM, auto)",
                       g_PcConfig.texpackBudgetMb, want, dedicatedMb);
                g_PcConfig.texpackBudgetMb = want;
            }
        }
    }
}

int HiresOverride_PackBudgetExceeded(void)
{
    long long cap = (long long)g_PcConfig.texpackBudgetMb << 20;
    if (cap <= 0)
        return 0; /* 0 = unlimited: use whatever VRAM the system has */
    return g_packBytesLive >= cap;
}

/* Eviction target: 7/8 of the cap, NOT the cap itself.
 *
 * Evicting only until `PackBudgetExceeded` clears settles the run at exactly
 * the cap, which has two costs. It leaves ZERO headroom for the OTHER consumer
 * of this budget — the VRAM-entry path (charas, items, HUD, 2D backgrounds)
 * composes eagerly at PostLoadTim and simply gives up when the budget is spent,
 * so a pool path parked on the line starves it permanently. And it recycles one
 * row per pump forever: free 5 MB, compose 5 MB, exceed, repeat (measured: 69
 * consecutive "evicted 1 cold row(s)" hovering at 510/512 MB). Freeing a slab
 * at a time gives both a working margin. */
int HiresOverride_PackBudgetOverTarget(void)
{
    long long cap = (long long)g_PcConfig.texpackBudgetMb << 20;
    if (cap <= 0)
        return 0;
    return g_packBytesLive > cap - (cap >> 3);
}

long long HiresOverride_PackBytesLive(void) { return g_packBytesLive; }

/* ---- Chunk-pool virtual slots (resident_textures) -------------------------
 * See hires_override.h for the canonical key encoding. Slot texture content
 * is REPLACED in place when the engine reuses a slot for another TIM. */
typedef struct {
    GLuint glTexture[HIRES_POOL_MAX_ROWS]; /* per CLUT row; [0] = base, 0 = empty */
    int    nativeW, nativeH; /* disc TIM pixel dims — texelSize denominator so
                              * prim UVs map 0..1 over any replacement size */
    unsigned rowPackBytes[HIRES_POOL_MAX_ROWS]; /* pack-budget charge per row */
    unsigned short rowW[HIRES_POOL_MAX_ROWS];   /* GL texture pixel dims per row — */
    unsigned short rowH[HIRES_POOL_MAX_ROWS];   /* the shader's footprint clamp */
    unsigned long long rowHash[HIRES_POOL_MAX_ROWS]; /* TexPack_LastComposeHash of each resident row; 0 = unknown */
    unsigned rowTick[HIRES_POOL_MAX_ROWS];      /* pump tick this row was last SAMPLED (LRU key) */
} PoolSlotEntry;

static PoolSlotEntry g_poolSlots[HIRES_POOL_SLOT_MAX];

/* Advanced once per pumped frame. Only the ordering matters, so wrap is
 * harmless: comparisons below are on age (now - tick) in unsigned arithmetic. */
static unsigned g_hiresTick = 0;

void HiresOverride_Tick(void) { g_hiresTick++; }

/* Free the least-recently-SAMPLED pack-composed row and credit its bytes back.
 *
 * Without this the budget is a one-way ratchet: `PackBudgetExceeded` latches on
 * and every later row keeps native art for the rest of the session, however far
 * the player walks from whatever filled it. That is the reported "stops loading
 * textures after some point", and it is why no single budget number can be
 * right — too small and textures stop, too large and the driver thrashes. The
 * CPU-side compose cache has had LRU eviction all along (tp_cache_evict_lru);
 * this is the missing GPU-side half.
 *
 * `minAgeTicks` keeps rows sampled in the last few frames off the table. The
 * pump runs after PsyX_EndScene, so a texture wanted this frame has already
 * been submitted, but the margin costs nothing and removes any argument about
 * GL names baked into prims still being in flight.
 *
 * The caller MUST restore native art for the returned row (see
 * HiresOverride_PoolSlotRestoreNativeRow): a row left at glTexture==0 falls
 * back to row 0's palette in the lookup, which is visibly wrong on the
 * multi-CLUT slots eviction targets most. Returns 0 when nothing is evictable
 * (everything is hot), which is the caller's signal to stop trying. */
int HiresOverride_EvictColdestPackRow(unsigned minAgeTicks,
                                      HiresRestorablePredicate canRestore,
                                      int* outSlot, int* outRow)
{
    int      bestSlot = -1, bestRow = -1;
    unsigned bestAge  = 0;
    int      i, r;

    for (i = 0; i < HIRES_POOL_SLOT_MAX; i++)
    {
        PoolSlotEntry* s = &g_poolSlots[i];
        for (r = 0; r < HIRES_POOL_MAX_ROWS; r++)
        {
            unsigned age;
            /* Only pack-composed rows are worth evicting: a native row costs
             * the budget nothing, so dropping one frees no bytes and would
             * just churn the upload. */
            if (s->rowPackBytes[r] == 0 || s->glTexture[r] == 0) continue;
            age = g_hiresTick - s->rowTick[r];
            if (age < minAgeTicks) continue;
            /* Never free art that cannot be put back. Slots whose retained TIM
             * blocks are gone (or were never kept - the chara pool registers no
             * lazy source) have no way back to their own palette, and the
             * lookup would serve ROW 0's instead: a monster in another
             * monster's colours. Those rows simply stay resident. */
            if (canRestore != NULL && !canRestore(i, r)) continue;
            if (bestSlot < 0 || age > bestAge)
            {
                bestAge  = age;
                bestSlot = i;
                bestRow  = r;
            }
        }
    }

    if (bestSlot >= 0)
    {
        PoolSlotEntry* s = &g_poolSlots[bestSlot];
        glDeleteTextures(1, &s->glTexture[bestRow]);
        s->glTexture[bestRow] = 0;
        s->rowHash[bestRow]   = 0;
        pack_credit(&s->rowPackBytes[bestRow]);

        if (outSlot) *outSlot = bestSlot;
        if (outRow)  *outRow  = bestRow;
        return 1;
    }

    /* LAST RESORT: a cold VRAM-entry override (chara / item / HUD / 2D
     * background pack row).
     *
     * These are charged to the same budget but were never eviction candidates,
     * so they were a permanent floor under it. Measured on a 512 MB budget: 104
     * VRAM-entry pack registrations, 78 of them 1024x1024 (5.6 MB each with
     * mips) — enough that no pool row could be freed to make room, the pump
     * logged "the hot set fills it", and world pack textures stopped for the
     * rest of the map ("they all suddenly unloaded").
     *
     * Ranked BELOW pool rows deliberately, and only reached once no pool row is
     * evictable, because the two recover differently: a pool row is recomposed
     * on demand by the very next TexPackLazy pump that sees a prim want it,
     * whereas the VRAM path only composes inside PostLoadTim, so an evicted
     * VRAM entry keeps native art until its TIM is uploaded again. Dropping one
     * is nonetheless SAFE — with no entry the prim samples real VRAM, which
     * still holds the native art. That is exactly what HiresOverride_-
     * InvalidateVramRect already does when the game rewrites a rect. */
    {
        int      bestEntry = -1;
        unsigned bestEntryAge = 0;

        for (i = 0; i < g_numEntries; i++)
        {
            HiresEntry* e = &g_entries[i];
            unsigned    age;

            if (e->packBytes == 0 || e->glTexture == 0) continue;
            age = g_hiresTick - e->tick;
            if (age < minAgeTicks) continue;
            if (bestEntry < 0 || age > bestEntryAge)
            {
                bestEntryAge = age;
                bestEntry    = i;
            }
        }

        if (bestEntry < 0) return 0;

        {
            HiresEntry* e = &g_entries[bestEntry];
            glDeleteTextures(1, &e->glTexture);
            pack_credit(&e->packBytes);
            g_entries[bestEntry] = g_entries[--g_numEntries];
        }
    }

    /* Slot -1 tells the caller there is no pool row to restore: the entry is
     * gone and native VRAM is already the fallback. */
    if (outSlot) *outSlot = -1;
    if (outRow)  *outRow  = -1;
    return 1;
}

/* Expand one CLUT row of a raw TIM pixel+palette block to RGBA8 at native
 * resolution and install it as this slot row's texture, UNCHARGED against the
 * pack budget (native rows never were). Restores an evicted row to correct art
 * rather than leaving the hole described above. */
int HiresOverride_PoolSlotRestoreNativeRow(int slotId, int row,
                                           const unsigned char* pixels, int w16, int h,
                                           const unsigned short* clutRow, int clutW,
                                           int bpp, int nativePixelW, int nativePixelH)
{
    PoolSlotEntry* s;
    unsigned char* rgba;
    int            pixW, x, y, rowBytes, rc;

    if (!g_initialized) HiresOverride_Init();
    if (slotId < 0 || slotId >= HIRES_POOL_SLOT_MAX) return -1;
    if (row < 0 || row >= HIRES_POOL_MAX_ROWS) return -1;
    if (pixels == NULL || clutRow == NULL || clutW <= 0) return -1;
    if (bpp != 4 && bpp != 8) return -1;
    if (w16 <= 0 || h <= 0) return -1;

    pixW     = (bpp == 4) ? w16 * 4 : w16 * 2;
    rowBytes = w16 * 2;

    rgba = (unsigned char*)malloc((size_t)pixW * (size_t)h * 4);
    if (rgba == NULL) return -1;

    for (y = 0; y < h; y++)
    {
        const unsigned char* src = pixels + (size_t)y * (size_t)rowBytes;
        for (x = 0; x < pixW; x++)
        {
            unsigned int idx;
            if (bpp == 4)
            {
                unsigned char b = src[x >> 1];
                idx = (x & 1) ? (unsigned int)((b >> 4) & 0xF) : (unsigned int)(b & 0xF);
            }
            else
            {
                idx = src[x];
            }
            if ((int)idx >= clutW) idx = 0;
            bgr555_to_rgba(clutRow[idx], &rgba[((size_t)y * (size_t)pixW + (size_t)x) * 4]);
        }
    }

    s  = &g_poolSlots[slotId];
    rc = upload_rgba(&s->glTexture[row], rgba, pixW, h,
                     (pixW == nativePixelW && h == nativePixelH));
    free(rgba);
    if (rc != 0) return -1;

    s->rowW[row]    = (unsigned short)pixW;
    s->rowH[row]    = (unsigned short)h;
    s->rowHash[row] = 0;
    s->rowTick[row] = g_hiresTick;
    pack_credit(&s->rowPackBytes[row]); /* native art is not pack-budgeted */
    if (nativePixelW > 0) s->nativeW = nativePixelW;
    if (nativePixelH > 0) s->nativeH = nativePixelH;
    return 0;
}

void HiresOverride_Init(void)
{
    if (g_initialized) return;
    g_numEntries = 0;
    g_initialized = 1;
    SH_DBG("[HIRES] override system initialized (capacity=%d)",
           MAX_HIRES_OVERRIDES);
}

/* TIM PSX-565 -> RGBA8 expansion (BGR555 with STP bit). */
static void bgr555_to_rgba(unsigned short cx, unsigned char* out)
{
    int r5 = cx & 0x1F;
    int g5 = (cx >> 5) & 0x1F;
    int b5 = (cx >> 10) & 0x1F;
    int stp = (cx >> 15) & 1;
    /* Standard 5-to-8 bit expansion: replicate top 3 bits into low bits. */
    out[0] = (unsigned char)((r5 << 3) | (r5 >> 2));
    out[1] = (unsigned char)((g5 << 3) | (g5 >> 2));
    out[2] = (unsigned char)((b5 << 3) | (b5 >> 2));
    /* PSX semi-transparency: cx==0 means transparent, otherwise opaque
     * (STP affects blending, not whether the pixel is drawn). */
    out[3] = (cx == 0) ? 0 : 255;
    (void)stp;
}

/* Parse a TIM file. On success: sets *outRGBA to a malloc'd RGBA8 buffer,
 * fills *outW, *outH with pixel dimensions, and *outBpp with the source
 * TIM's bit depth (4, 8, 16, or 24). Paletted TIMs decode with CLUT row
 * `clutRow` (clamped; chunk TIMs carry 6-12 palette-variant rows) and
 * report the row count via *outClutRows (1 for 16/24bpp). Caller must
 * free *outRGBA. Returns 0 on success, -1 on parse failure. */
static int parse_tim_to_rgba(const unsigned char* data, unsigned int size,
                             unsigned char** outRGBA, int* outW, int* outH,
                             int* outBpp, int clutRow, int* outClutRows)
{
    if (size < 12) return -1;
    /* TIM header: 4-byte magic (0x00000010) + 4-byte mode flags. */
    if (data[0] != 0x10 || data[1] != 0x00 || data[2] != 0x00 || data[3] != 0x00)
        return -1;

    unsigned int flags = (unsigned int)data[4]
                       | ((unsigned int)data[5] << 8)
                       | ((unsigned int)data[6] << 16)
                       | ((unsigned int)data[7] << 24);
    int bppCode  = (int)(flags & 0x7);   /* 0=4, 1=8, 2=16, 3=24 */
    int hasClut  = (int)((flags >> 3) & 1);
    int srcBpp;
    switch (bppCode) {
        case 0: srcBpp = 4;  break;
        case 1: srcBpp = 8;  break;
        case 2: srcBpp = 16; break;
        case 3: srcBpp = 24; break;
        default: return -1;
    }
    *outBpp = srcBpp;

    const unsigned char* p = data + 8;
    const unsigned char* end = data + size;

    /* Optional CLUT block (always 16bpp BGR555 entries). */
    const unsigned char* clutEntries = NULL;
    int                  clutW = 0, clutH = 0;
    if (hasClut)
    {
        if (p + 12 > end) return -1;
        unsigned int blockLen = (unsigned int)p[0]
                              | ((unsigned int)p[1] << 8)
                              | ((unsigned int)p[2] << 16)
                              | ((unsigned int)p[3] << 24);
        clutW = (int)((unsigned int)p[8] | ((unsigned int)p[9] << 8));
        clutH = (int)((unsigned int)p[10] | ((unsigned int)p[11] << 8));
        clutEntries = p + 12;
        if (p + blockLen > end || blockLen < 12) return -1;
        p += blockLen;
    }
    if (outClutRows) *outClutRows = (clutH > 0) ? clutH : 1;

    /* Pixel block. */
    if (p + 12 > end) return -1;
    /* unsigned int pixBlockLen = (unsigned int)p[0] | ... ; (unused) */
    int pixCellW = (int)((unsigned int)p[8] | ((unsigned int)p[9] << 8));
    int pixH     = (int)((unsigned int)p[10] | ((unsigned int)p[11] << 8));
    const unsigned char* pix = p + 12;

    /* Cell width is in 16-bit units. Convert to pixel width per bit depth. */
    int pixW;
    switch (srcBpp) {
        case 4:  pixW = pixCellW * 4;            break;
        case 8:  pixW = pixCellW * 2;            break;
        case 16: pixW = pixCellW;                break;
        case 24: pixW = (pixCellW * 2) / 3;      break;
        default: return -1;
    }
    if (pixW <= 0 || pixH <= 0 || pixW > 8192 || pixH > 8192) return -1;

    *outW = pixW;
    *outH = pixH;

    unsigned char* rgba = (unsigned char*)malloc((size_t)pixW * (size_t)pixH * 4);
    if (!rgba) return -1;

    if (srcBpp == 4 || srcBpp == 8)
    {
        if (!clutEntries) { free(rgba); return -1; }
        int rowCells = pixCellW;
        int rowBytes = rowCells * 2;
        if (clutRow >= clutH) clutRow = 0;
        if (clutRow > 0) clutEntries += (size_t)clutRow * (size_t)clutW * 2;
        int totalIndices = clutW;
        for (int y = 0; y < pixH; y++)
        {
            const unsigned char* row = pix + (size_t)y * (size_t)rowBytes;
            for (int x = 0; x < pixW; x++)
            {
                unsigned int idx;
                if (srcBpp == 4)
                {
                    int byteIdx = x >> 1;
                    if (byteIdx >= rowBytes) { idx = 0; }
                    else
                    {
                        unsigned char b = row[byteIdx];
                        idx = (x & 1) ? (unsigned int)((b >> 4) & 0xF)
                                       : (unsigned int)(b & 0xF);
                    }
                }
                else /* 8bpp */
                {
                    if (x >= rowBytes) { idx = 0; }
                    else { idx = row[x]; }
                }
                if ((int)idx >= totalIndices) idx = 0;
                unsigned short cx = (unsigned short)clutEntries[idx * 2]
                                  | ((unsigned short)clutEntries[idx * 2 + 1] << 8);
                bgr555_to_rgba(cx, &rgba[(size_t)(y * pixW + x) * 4]);
            }
        }
    }
    else if (srcBpp == 16)
    {
        for (int y = 0; y < pixH; y++)
        {
            const unsigned char* row = pix + (size_t)y * (size_t)pixCellW * 2;
            for (int x = 0; x < pixW; x++)
            {
                unsigned short cx = (unsigned short)row[x * 2]
                                  | ((unsigned short)row[x * 2 + 1] << 8);
                bgr555_to_rgba(cx, &rgba[(size_t)(y * pixW + x) * 4]);
            }
        }
    }
    else /* 24bpp */
    {
        /* 24-bit: 3 bytes/pixel, packed BGR. Width-cell math means
         * pixel data is 3 bytes per pixel, row stride = pixCellW * 2. */
        for (int y = 0; y < pixH; y++)
        {
            const unsigned char* row = pix + (size_t)y * (size_t)pixCellW * 2;
            for (int x = 0; x < pixW; x++)
            {
                unsigned char* o = &rgba[(size_t)(y * pixW + x) * 4];
                /* Most TIM 24bpp tools store as B,G,R; treat as that. */
                o[0] = row[x * 3 + 0];
                o[1] = row[x * 3 + 1];
                o[2] = row[x * 3 + 2];
                o[3] = 255;
            }
        }
    }

    *outRGBA = rgba;
    return 0;
}

/* Decode a texture blob — PNG (real 8-bit alpha) or TIM (1-bit colour-0) —
 * to a malloc'd RGBA8 buffer. `tag` is for log messages only. On success
 * caller owns *outRGBA (stb's allocator is malloc, so free() fits both
 * paths). *outBpp: source depth, 32 for PNG. Returns 0 on success. */
static int decode_to_rgba(const char* tag,
                          const unsigned char* data, unsigned int size,
                          unsigned char** outRGBA, int* outW, int* outH,
                          int* outBpp, int clutRow, int* outClutRows)
{
    if (outClutRows) *outClutRows = 1;
    if (size >= 8 && data[0] == 0x89 && data[1] == 'P' &&
        data[2] == 'N' && data[3] == 'G')
    {
        int comp = 0;
        *outRGBA = stbi_load_from_memory(data, (int)size, outW, outH, &comp, 4);
        if (*outRGBA == NULL)
        {
            SH_DBG("[HIRES] %s: PNG decode failed (size=%u): %s",
                   tag, size, stbi_failure_reason());
            return -1;
        }
        *outBpp = 32;
    }
    else if (parse_tim_to_rgba(data, size, outRGBA, outW, outH, outBpp,
                               clutRow, outClutRows) != 0)
    {
        SH_DBG("[HIRES] %s: TIM parse failed (size=%u)", tag, size);
        return -1;
    }

    if (*outW <= 0 || *outH <= 0 || *outW > 8192 || *outH > 8192)
    {
        SH_DBG("[HIRES] %s: unusable dimensions %dx%d", tag, *outW, *outH);
        free(*outRGBA);
        *outRGBA = NULL;
        return -1;
    }
    return 0;
}

/* PC minimap: decode a raw TIM (or PNG) blob to RGBA8 without touching PSX VRAM
 * or the override tables. Caller frees *outRGBA. Returns 0 on success. */
int HiresOverride_DecodeToRGBA(const unsigned char* data, unsigned int size,
                               unsigned char** outRGBA, int* outW, int* outH)
{
    int bpp = 0;
    return decode_to_rgba("minimap", data, size, outRGBA, outW, outH, &bpp, 0, NULL);
}

int HiresOverride_RegisterFromTim(const char* timPath,
                                   const unsigned char* timData,
                                   unsigned int timSize,
                                   int targetVramX, int targetVramY,
                                   int targetVramW, int targetVramH,
                                   int targetClutX, int targetClutY,
                                   int originalBitDepth)
{
    if (!g_initialized) HiresOverride_Init();
    if (g_numEntries >= MAX_HIRES_OVERRIDES)
    {
        SH_DBG("[HIRES] table full, ignoring %s", timPath);
        return -1;
    }

    unsigned char* rgba = NULL;
    int hiW = 0, hiH = 0, srcBpp = 0;
    if (decode_to_rgba(timPath, timData, timSize, &rgba, &hiW, &hiH, &srcBpp, 0, NULL) != 0)
    {
        return -1;
    }

    GLuint tex = 0;
    if (upload_rgba(&tex, rgba, hiW, hiH, 0) != 0)
    {
        free(rgba);
        return -1;
    }
    free(rgba);

    HiresEntry* e = &g_entries[g_numEntries++];
    e->vramX = targetVramX;
    e->vramY = targetVramY;
    e->vramW = targetVramW;
    e->vramH = targetVramH;
    e->clutX = targetClutX;
    e->clutY = targetClutY;
    e->originalBitDepth = originalBitDepth;
    e->glTexture = tex;
    e->hiresW = hiW;
    e->hiresH = hiH;
    e->sourceBitDepth = srcBpp;
    e->packBytes = 0; /* loose user file, not pack-budgeted (recycled slot may hold a stale charge) */

    SH_DBG("[HIRES] registered %s: vram=(%d,%d %dx%d cells, %dbpp) "
           "clut=(%d,%d) hires=%dx%d (src %dbpp) tex=%u",
           timPath, targetVramX, targetVramY, targetVramW, targetVramH,
           originalBitDepth, targetClutX, targetClutY,
           hiW, hiH, srcBpp, (unsigned)tex);
    return 0;
}


int HiresOverride_PoolSlotRegister(int slotId,
                                   const unsigned char* data, unsigned int size,
                                   int nativePixelW, int nativePixelH)
{
    char tag[24];

    if (!g_initialized) HiresOverride_Init();
    if (slotId < 0 || slotId >= HIRES_POOL_SLOT_MAX)
    {
        SH_DBG("[POOLTEX] slot %d out of range", slotId);
        return -1;
    }

    /* Engine slot reuse: this call rewrites every row of the slot with a
     * different TIM, so the previous occupant's retained pack-compose inputs
     * must not outlive it - a stale source would compose the OLD sheet into the
     * NEW slot's rows. Covers the disc-TIM base registration, the whole-image
     * loose replacement (which bypasses the pack path entirely and so installs
     * no new source) and the minimap slot. The pool path re-installs the new
     * source right after this returns. */
    TexPackLazy_DropSlot(slotId);

    snprintf(tag, sizeof(tag), "pool slot %d", slotId);

    /* BC7 .dds replacement: upload the compressed blocks straight through rather
     * than expanding to RGBA8. Same 4x VRAM saving the format exists for, and it
     * keeps a real 8-bit alpha for the override shader's cutout. A whole-image
     * .dds covers the slot, so it lands on row 0 like any whole-image PNG. */
    {
        s_DdsBptc probe;
        if (Dds_ParseBptc(data, (int)size, &probe))
        {
            PoolSlotEntry* ds = &g_poolSlots[slotId];
            int r2;
            for (r2 = 0; r2 < HIRES_POOL_MAX_ROWS; r2++)
            {
                if (ds->glTexture[r2] != 0)
                {
                    glDeleteTextures(1, &ds->glTexture[r2]);
                    ds->glTexture[r2] = 0;
                }
                pack_credit(&ds->rowPackBytes[r2]);
            }
            if (Dds_UploadBptc(&ds->glTexture[0], data, (int)size,
                               s_forceNearestUpload ||
                               (probe.width == nativePixelW && probe.height == nativePixelH)) != 0)
            {
                SH_DBG("[POOLTEX] slot %d: BC7 upload failed — falling back", slotId);
                return -1;
            }
            /* BC7 is 1 byte/texel vs RGBA8's 4; charge the budget accordingly by
             * quartering the width it accounts for. */
            pack_charge(&ds->rowPackBytes[0], (probe.width + 3) / 4, probe.height);
            ds->rowW[0] = (unsigned short)probe.width;
            ds->rowH[0] = (unsigned short)probe.height;
            ds->nativeW = nativePixelW;
            ds->nativeH = nativePixelH;
            ds->rowHash[0] = 0;
            SH_DBG("[POOLTEX] slot %d <- BC7 %dx%d (%d mip%s, native %dx%d)",
                   slotId, probe.width, probe.height, probe.mipCount,
                   probe.mipCount == 1 ? "" : "s", nativePixelW, nativePixelH);
            return 0;
        }
    }

    /* One texture per CLUT row: prims select palette rows with baked +64*row
     * clut deltas (see hires_override.h), so every row a TIM ships must be
     * decodable at draw. PNG replacements have one palette: row 0 only. */
    PoolSlotEntry* s = &g_poolSlots[slotId];
    int rows = 1, r, hiW = 0, hiH = 0;
    /* A mid-loop failure used to `return` straight out, skipping the stale-row
     * drop below. On a RECYCLED slot that left rows r+1..15 holding the PREVIOUS
     * TIM's textures — so the slot rendered a mix of two sheets depending on
     * which palette row a prim selected — and left the previous occupant's pack
     * charges on the books. Break into the shared epilogue instead, and drop
     * from the number of rows actually written rather than the declared count. */
    int status = 0;
    int doneRows = 0;

    for (r = 0; r < rows; r++)
    {
        unsigned char* rgba = NULL;
        int w = 0, h = 0, srcBpp = 0, timRows = 1;
        if (decode_to_rgba(tag, data, size, &rgba, &w, &h, &srcBpp, r, &timRows) != 0)
        {
            status = (r == 0) ? -1 : 0;
            break;
        }
        if (r == 0)
        {
            rows = (srcBpp == 32) ? 1 : timRows;
            if (rows > HIRES_POOL_MAX_ROWS)
            {
                /* Chara-pool slots spill rows 16+ into slot+64*k — the clut
                 * encoding walks Y groups on +64*row deltas, so a prim
                 * addressing row 20 decodes to (slot+64, row 4) and the
                 * spill registration below matches it. Chunk-pool ids keep
                 * the old cap: their neighbors are other live slots. */
                if (slotId < HIRES_POOL_CHARA_SLOT_BASE)
                {
                    SH_DBG("[POOLTEX] slot %d: %d CLUT rows, capping at %d",
                           slotId, rows, HIRES_POOL_MAX_ROWS);
                    rows = HIRES_POOL_MAX_ROWS;
                }
            }
            hiW = w;
            hiH = h;
        }
        /* Native-res decodes sample NEAREST (PSX-exact texel look); an
         * upscaled loose replacement gets LINEAR+mips like hi-res overrides. */
        {
            PoolSlotEntry* rowSlot = s;
            int            rowIdx  = r;
            if (r >= HIRES_POOL_MAX_ROWS)
            {
                int spillId = slotId + 64 * (r / HIRES_POOL_MAX_ROWS);
                if (spillId >= HIRES_POOL_SLOT_MAX)
                {
                    free(rgba);
                    continue;
                }
                rowSlot = &g_poolSlots[spillId];
                rowIdx  = r % HIRES_POOL_MAX_ROWS;
                rowSlot->nativeW = nativePixelW;
                rowSlot->nativeH = nativePixelH;
            }
            if (upload_rgba(&rowSlot->glTexture[rowIdx], rgba,
                            w, h, (w == nativePixelW && h == nativePixelH)) != 0)
            {
                free(rgba);
                status = (r == 0) ? -1 : 0;
                break;
            }
            free(rgba);
            if (r < HIRES_POOL_MAX_ROWS)
            {
                doneRows = r + 1;
            }
            rowSlot->rowW[rowIdx] = (unsigned short)w;
            rowSlot->rowH[rowIdx] = (unsigned short)h;
            /* This row now holds base/loose content, not a keyed pack composite —
             * invalidate the compose-hash so the redundant-upload skip can never
             * mistake a later pack upload for this row's current content. */
            rowSlot->rowHash[rowIdx] = 0;
            /* This row now holds base/loose content — release any pack charge a
             * previous occupant of the slot left on it. */
            pack_credit(&rowSlot->rowPackBytes[rowIdx]);
        }
    }

    /* Slot reuse with fewer rows, and any tail a mid-loop failure left behind:
     * drop stale row textures past what this TIM actually wrote. On a clean run
     * doneRows == min(rows, HIRES_POOL_MAX_ROWS), so this is the old bound. */
    for (r = doneRows; r < HIRES_POOL_MAX_ROWS; r++)
    {
        if (s->glTexture[r] != 0)
        {
            glDeleteTextures(1, &s->glTexture[r]);
            s->glTexture[r] = 0;
        }
        pack_credit(&s->rowPackBytes[r]);
    }

    s->nativeW = nativePixelW;
    s->nativeH = nativePixelH;

    static int s_regLog = 0;
    if (s_regLog < 256)
    {
        SH_DBG("[POOLTEX] slot %d <- %dx%d x%d rows (native %dx%d) tex=%u",
               slotId, hiW, hiH, rows, nativePixelW, nativePixelH,
               (unsigned)s->glTexture[0]);
        s_regLog++;
    }
    return status;
}

/* Upload straight RGBA into a slot/entry texture, creating it on demand.
 * Upscaled replacements (non-nearest) get mipmaps so they don't shimmer at
 * distance the way a raw LINEAR-sampled 4x texture does; native-res decodes
 * stay NEAREST with no mips (PSX-exact). Returns 0 on success. */
static int upload_rgba(GLuint* tex, const unsigned char* rgba, int w, int h, int nearest)
{
    if (s_forceNearestUpload) nearest = 1; /* font atlas: point-sample, kill gutterless-cell bleed */
    if (rgba == NULL || w <= 0 || h <= 0) return -1;
    if (*tex == 0)
    {
        glGenTextures(1, tex);
        if (*tex == 0) return -1;
    }
    glBindTexture(GL_TEXTURE_2D, *tex);
    while (glGetError() != GL_NO_ERROR) { } /* drain stale errors */

    /* When this texture object ALREADY has these exact dimensions and format,
     * no storage is (re)allocated, so the AMD undefined-storage case the
     * prefill below exists for cannot arise: glTexSubImage2D alone is correct,
     * ~3x faster, and skips the per-upload VRAM realloc churn that fragments
     * the driver heap over a long session. The prefill path stays verbatim for
     * genuine (re)allocations — the only case that workaround was ever about.
     * The driver is asked rather than a side table kept: GL names are deleted
     * and recycled throughout this file, and a slot row that last took a BC7
     * upload reports a COMPRESSED internal format here (a sub-image into it
     * would fail), so only GL's own answer is trustworthy. */
    int reuseStorage;
    {
        GLint curW = 0, curH = 0, curFmt = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &curW);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &curH);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &curFmt);
        reuseStorage = (curW == w && curH == h &&
                        (curFmt == GL_RGBA || curFmt == GL_RGBA8));
    }

    if (reuseStorage)
    {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                        GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    else
    {
        /* Define the storage as fully TRANSPARENT first, then overlay the real
         * pixels via glTexSubImage2D. On some AMD drivers, under the large
         * resident_textures working set, glTexImage2D can leave the texture's
         * storage silently undefined (NO GL error raised — so the error check
         * below can't catch it): the override shader then samples garbage that
         * shows as blue/black spike/wedge fills on AMD while NVIDIA reads benign
         * zeros. This is the residual the school-rainbow fix anticipated
         * ("silent undefined pool tex needs pre-zero"), now confirmed by the
         * reporter's resident_textures=0 bisect making it vanish. A zeroed base
         * means any silent/partial upload failure leaves TRANSPARENT texels
         * (discarded by the override shader), matching the discrete-GPU result.
         * On a conformant driver the SubImage overwrite makes the final texels
         * byte-identical, so working setups are unaffected. Reuses one
         * grown-as-needed zeroed staging buffer. */
        static unsigned char* s_zeroFill = NULL;
        static int            s_zeroBytes = 0;
        int                   need = w * h * 4;
        if (need > s_zeroBytes)
        {
            unsigned char* nz = (unsigned char*)realloc(s_zeroFill, (size_t)need);
            if (nz) { s_zeroFill = nz; memset(s_zeroFill, 0, (size_t)need); s_zeroBytes = need; }
        }
        if (s_zeroFill != NULL && s_zeroBytes >= need)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, s_zeroFill);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                            GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }
        else
        {
            /* Zero buffer alloc failed — fall back to the direct upload. */
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }
    }
    /* ANY upload error must degrade to a clean miss, never a live-but-undefined
     * texture. Only GL_OUT_OF_MEMORY was handled; but on the Intel HD 4600
     * driver a large resident-texture working set can make glTexImage2D fail
     * with other errors while leaving the GL name live over undefined storage.
     * HiresOverride_LookupByTpageClut then returns that name as a HIT, and the
     * 32-bit override shader's `discard alpha<0.5` renders the undefined texels:
     * stale VRAM = vertical rainbow on Intel (modern drivers zero the storage =
     * transparent = invisible). Deleting the texture on any error makes the
     * lookup miss cleanly so the prim samples the zeroed VRAM texture instead —
     * invisible, matching the discrete-GPU result. On a conformant driver a
     * valid upload raises no error, so working setups are unaffected. */
    {
        GLenum uploadErr = glGetError();
        if (uploadErr != GL_NO_ERROR)
        {
            static int s_oomLog = 0;
            if (s_oomLog < 8) { SH_DBG("[POOLTEX] GL error 0x%X on %dx%d upload — keeping native art", (unsigned)uploadErr, w, h); s_oomLog++; }
            glBindTexture(GL_TEXTURE_2D, 0);
            glDeleteTextures(1, tex);
            *tex = 0;
            return -1;
        }
    }
    if (!nearest && glGenerateMipmap != NULL)
    {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    }
    else
    {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return 0;
}

unsigned int HiresOverride_CreateTextureRGBA(const unsigned char* rgba, int w, int h)
{
    GLuint texture = 0;

    if (!g_initialized) HiresOverride_Init();
    if (upload_rgba(&texture, rgba, w, h, 0) != 0)
        return 0;
    return (unsigned int)texture;
}

int HiresOverride_PoolSlotRegisterRGBAKeyed(int slotId, int row,
                                            const unsigned char* rgba, int w, int h,
                                            int nativePixelW, int nativePixelH,
                                            unsigned long long contentHash)
{
    if (!g_initialized) HiresOverride_Init();
    if (slotId < 0 || slotId >= HIRES_POOL_SLOT_MAX ||
        row < 0 || row >= HIRES_POOL_MAX_ROWS)
    {
        SH_DBG("[POOLTEX] slot %d row %d out of range (RGBA)", slotId, row);
        return -1;
    }

    PoolSlotEntry* s = &g_poolSlots[slotId];

    /* Redundant re-upload skip: the engine re-uploads the same TIM to VRAM
     * constantly (room churn, animated CLUTs resolving to the same palette),
     * and glTexImage2D reallocates VRAM each time — thousands of needless
     * re-uploads per session stutter and fragment VRAM (the ~30-min crash). A
     * matching content hash + same dims + a live texture means this row's GL
     * texture is ALREADY correct; keep it and its budget charge untouched. The
     * glTexture!=0 test makes a stale hash after a slot reset (glTexture zeroed)
     * force a genuine re-upload. */
    if (contentHash != 0 && s->rowHash[row] == contentHash && s->glTexture[row] != 0 &&
        s->rowW[row] == (unsigned short)w && s->rowH[row] == (unsigned short)h)
    {
        s->nativeW = nativePixelW;
        s->nativeH = nativePixelH;
        return 0;
    }

    if (upload_rgba(&s->glTexture[row], rgba, w, h,
                    (w == nativePixelW && h == nativePixelH)) != 0)
    {
        s->rowHash[row] = 0;
        /* The uploader zeroes the name when it drops a half-made texture; that
         * row's charge is then dead weight on the budget, which is consulted
         * most often precisely when uploads are failing. Credit it back. (An
         * argument-validation reject leaves the name intact, so it stays
         * charged - correctly, the texture is still live.) */
        if (s->glTexture[row] == 0)
        {
            pack_credit(&s->rowPackBytes[row]);
        }
        return -1;
    }
    pack_charge(&s->rowPackBytes[row], w, h);
    s->rowHash[row] = contentHash;
    s->rowW[row] = (unsigned short)w;
    s->rowH[row] = (unsigned short)h;
    s->nativeW = nativePixelW;
    s->nativeH = nativePixelH;
    /* Stamp the row live NOW, or it is born cold and the next eviction pass
     * frees it before a prim ever samples it — a compose/evict loop, because
     * the want that triggered this compose fires again immediately. The
     * lookup's sample stamp lands on `useRow`, which is row 0 while this row is
     * still empty, so it never reaches the row actually being built. */
    s->rowTick[row] = g_hiresTick;
    return 0;
}

int HiresOverride_PoolSlotRegisterRGBA(int slotId, int row,
                                       const unsigned char* rgba, int w, int h,
                                       int nativePixelW, int nativePixelH)
{
    return HiresOverride_PoolSlotRegisterRGBAKeyed(slotId, row, rgba, w, h,
                                                   nativePixelW, nativePixelH, 0);
}

/* Pack .dds twin of PoolSlotRegisterRGBAKeyed: upload the BC7 blocks straight to
 * this slot row (no RGBA expansion, 4x cheaper VRAM). Same redundant-upload skip
 * and budget bookkeeping; the budget is charged at BC7's 1 byte/texel (quarter
 * width) like the loose-.dds path in PoolSlotRegister. */
int HiresOverride_PoolSlotRegisterDdsKeyed(int slotId, int row,
                                           const unsigned char* ddsBytes, size_t ddsSize,
                                           int nativePixelW, int nativePixelH,
                                           unsigned long long contentHash)
{
    s_DdsBptc      probe;
    int            w, h;
    PoolSlotEntry* s;

    if (!g_initialized) HiresOverride_Init();
    if (slotId < 0 || slotId >= HIRES_POOL_SLOT_MAX ||
        row < 0 || row >= HIRES_POOL_MAX_ROWS)
    {
        SH_DBG("[POOLTEX] slot %d row %d out of range (DDS)", slotId, row);
        return -1;
    }
    if (!Dds_ParseBptc(ddsBytes, (int)ddsSize, &probe)) return -1;
    w = probe.width;
    h = probe.height;

    s = &g_poolSlots[slotId];
    if (contentHash != 0 && s->rowHash[row] == contentHash && s->glTexture[row] != 0 &&
        s->rowW[row] == (unsigned short)w && s->rowH[row] == (unsigned short)h)
    {
        s->nativeW = nativePixelW;
        s->nativeH = nativePixelH;
        return 0;
    }

    /* Sampling must not depend on the pack's FORMAT: upload_rgba derives
     * `nearest` from native-vs-upscaled and additionally honours the font
     * atlas's force-nearest, so the .dds twin has to do both or the same
     * texture renders differently once converted (a font TIM shipped as .dds
     * came back bilinear+mipmapped, re-opening the gutterless-cell ghost text
     * a3e7973cf fixed for .png). */
    if (Dds_UploadBptc(&s->glTexture[row], ddsBytes, (int)ddsSize,
                       s_forceNearestUpload ||
                       (w == nativePixelW && h == nativePixelH)) != 0)
    {
        s->rowHash[row] = 0;
        /* The uploader zeroes the name when it drops a half-made texture; that
         * row's charge is then dead weight on the budget, which is consulted
         * most often precisely when uploads are failing. Credit it back. (An
         * argument-validation reject leaves the name intact, so it stays
         * charged - correctly, the texture is still live.) */
        if (s->glTexture[row] == 0)
        {
            pack_credit(&s->rowPackBytes[row]);
        }
        return -1;
    }
    pack_charge(&s->rowPackBytes[row], (w + 3) / 4, h);
    s->rowHash[row] = contentHash;
    s->rowW[row] = (unsigned short)w;
    s->rowH[row] = (unsigned short)h;
    s->nativeW = nativePixelW;
    s->nativeH = nativePixelH;
    s->rowTick[row] = g_hiresTick; /* see the RGBA twin */
    return 0;
}

int HiresOverride_RegisterRGBAKeyed(const char* label,
                                    const unsigned char* rgba, int w, int h,
                                    int targetVramX, int targetVramY,
                                    int targetVramW, int targetVramH,
                                    int targetClutX, int targetClutY,
                                    int originalBitDepth,
                                    unsigned long long contentHash)
{
    HiresEntry* e = NULL;
    int i;

    if (!g_initialized) HiresOverride_Init();

    /* Replace-in-place on an identical key: the same VRAM rect gets
     * re-uploaded whenever the game reloads that TIM (room transitions),
     * and content-hashed pack composites can change with it. */
    for (i = 0; i < g_numEntries; i++)
    {
        HiresEntry* c = &g_entries[i];
        if (c->vramX == targetVramX && c->vramY == targetVramY &&
            c->vramW == targetVramW && c->vramH == targetVramH &&
            c->clutX == targetClutX && c->clutY == targetClutY &&
            c->originalBitDepth == originalBitDepth)
        {
            e = c;
            break;
        }
    }

    /* Redundant re-upload skip (see the pool-slot registrar): a found entry
     * whose content is byte-identical (same hash + dims + a live texture) is
     * already correct. The same VRAM rect re-uploads every room reload; skip
     * the glTexImage2D churn that stutters and fragments VRAM. */
    if (e != NULL && contentHash != 0 && e->contentHash == contentHash &&
        e->glTexture != 0 && e->hiresW == w && e->hiresH == h)
    {
        return 0;
    }

    if (e == NULL)
    {
        if (g_numEntries >= MAX_HIRES_OVERRIDES)
        {
            SH_DBG("[HIRES] table full, ignoring %s", label);
            return -1;
        }
        e = &g_entries[g_numEntries];
        e->glTexture = 0;
        e->contentHash = 0;
        /* A recycled table index can still carry the previous entry's charge;
         * zero it before any failure path below credits it. */
        e->packBytes = 0;
    }

    if (upload_rgba(&e->glTexture, rgba, w, h, 0) != 0)
    {
        e->contentHash = 0;
        if (e->glTexture == 0)
        {
            pack_credit(&e->packBytes);
        }
        return -1;
    }
    if (e == &g_entries[g_numEntries])
    {
        e->packBytes = 0;
        g_numEntries++;
    }
    pack_charge(&e->packBytes, w, h);
    e->contentHash = contentHash;
    /* Stamp it live NOW. Leaving tick at 0 makes a just-registered entry look
     * maximally cold to the evictor, which would free it before a single prim
     * ever sampled it. */
    e->tick = g_hiresTick;

    e->vramX = targetVramX;
    e->vramY = targetVramY;
    e->vramW = targetVramW;
    e->vramH = targetVramH;
    e->clutX = targetClutX;
    e->clutY = targetClutY;
    e->originalBitDepth = originalBitDepth;
    e->hiresW = w;
    e->hiresH = h;
    e->sourceBitDepth = 32;

    static int s_rgbaLog = 0;
    if (s_rgbaLog < 256)
    {
        SH_DBG("[HIRES] registered %s (RGBA %dx%d): vram=(%d,%d %dx%d cells, %dbpp) clut=(%d,%d) tex=%u",
               label, w, h, targetVramX, targetVramY, targetVramW, targetVramH,
               originalBitDepth, targetClutX, targetClutY, (unsigned)e->glTexture);
        s_rgbaLog++;
    }
    return 0;
}

/* Pack .dds twin of RegisterRGBAKeyed: a whole-upload BC7 pack entry uploaded to
 * a VRAM-rect override without RGBA expansion. Same find-in-place + redundant
 * skip; dims come from the DDS header, budget charged at BC7's 1 byte/texel. */
int HiresOverride_RegisterDdsKeyed(const char* label,
                                   const unsigned char* ddsBytes, size_t ddsSize,
                                   int targetVramX, int targetVramY,
                                   int targetVramW, int targetVramH,
                                   int targetClutX, int targetClutY,
                                   int originalBitDepth,
                                   unsigned long long contentHash)
{
    s_DdsBptc   probe;
    int         w, h, i;
    HiresEntry* e = NULL;

    if (!g_initialized) HiresOverride_Init();
    if (!Dds_ParseBptc(ddsBytes, (int)ddsSize, &probe)) return -1;
    w = probe.width;
    h = probe.height;

    for (i = 0; i < g_numEntries; i++)
    {
        HiresEntry* c = &g_entries[i];
        if (c->vramX == targetVramX && c->vramY == targetVramY &&
            c->vramW == targetVramW && c->vramH == targetVramH &&
            c->clutX == targetClutX && c->clutY == targetClutY &&
            c->originalBitDepth == originalBitDepth)
        {
            e = c;
            break;
        }
    }

    if (e != NULL && contentHash != 0 && e->contentHash == contentHash &&
        e->glTexture != 0 && e->hiresW == w && e->hiresH == h)
    {
        return 0;
    }

    if (e == NULL)
    {
        if (g_numEntries >= MAX_HIRES_OVERRIDES)
        {
            SH_DBG("[HIRES] table full, ignoring %s", label);
            return -1;
        }
        e = &g_entries[g_numEntries];
        e->glTexture = 0;
        e->contentHash = 0;
        /* A recycled table index can still carry the previous entry's charge;
         * zero it before any failure path below credits it. */
        e->packBytes = 0;
    }

    /* RGBA twin passes 0 here and upload_rgba ORs the font force-nearest in
     * itself; replicate that so format never changes sampling. */
    if (Dds_UploadBptc(&e->glTexture, ddsBytes, (int)ddsSize, s_forceNearestUpload) != 0)
    {
        e->contentHash = 0;
        if (e->glTexture == 0)
        {
            pack_credit(&e->packBytes);
        }
        return -1;
    }
    if (e == &g_entries[g_numEntries])
    {
        e->packBytes = 0;
        g_numEntries++;
    }
    pack_charge(&e->packBytes, (w + 3) / 4, h);
    e->contentHash = contentHash;
    e->tick = g_hiresTick; /* see the RGBA twin */

    e->vramX = targetVramX;
    e->vramY = targetVramY;
    e->vramW = targetVramW;
    e->vramH = targetVramH;
    e->clutX = targetClutX;
    e->clutY = targetClutY;
    e->originalBitDepth = originalBitDepth;
    e->hiresW = w;
    e->hiresH = h;
    e->sourceBitDepth = 32; /* the override shader samples BC7 as RGBA */
    return 0;
}

int HiresOverride_RegisterRGBA(const char* label,
                               const unsigned char* rgba, int w, int h,
                               int targetVramX, int targetVramY,
                               int targetVramW, int targetVramH,
                               int targetClutX, int targetClutY,
                               int originalBitDepth)
{
    return HiresOverride_RegisterRGBAKeyed(label, rgba, w, h, targetVramX, targetVramY,
                                           targetVramW, targetVramH, targetClutX, targetClutY,
                                           originalBitDepth, 0);
}

/* A loose per-row PNG/TIM overlays ONE palette row on top of the disc-decoded
 * base slot: decode row 0 of the replacement and hand it to the existing
 * per-row uploader. The replacement carries one palette, so its own row 0 is
 * the image; `row` is which slot row it lands on. */
int HiresOverride_PoolSlotLoosePngRow(int slotId, int row,
                                      const unsigned char* data, unsigned int size,
                                      int nativePixelW, int nativePixelH)
{
    unsigned char* rgba = NULL;
    int w = 0, h = 0, bpp = 0, rc;

    if (decode_to_rgba("loose pool row", data, size, &rgba, &w, &h, &bpp, 0, NULL) != 0)
    {
        return -1;
    }
    rc = HiresOverride_PoolSlotRegisterRGBA(slotId, row, rgba, w, h, nativePixelW, nativePixelH);
    free(rgba);
    return rc;
}

int HiresOverride_RegisterLoosePngRow(const char* label,
                                      const unsigned char* data, unsigned int size,
                                      int targetVramX, int targetVramY,
                                      int targetVramW, int targetVramH,
                                      int targetClutX, int targetClutY,
                                      int originalBitDepth)
{
    unsigned char* rgba = NULL;
    int w = 0, h = 0, bpp = 0, rc;

    if (decode_to_rgba(label, data, size, &rgba, &w, &h, &bpp, 0, NULL) != 0)
    {
        return -1;
    }
    rc = HiresOverride_RegisterRGBA(label, rgba, w, h,
                                    targetVramX, targetVramY, targetVramW, targetVramH,
                                    targetClutX, targetClutY, originalBitDepth);
    free(rgba);
    return rc;
}

int HiresOverride_RegisterLoosePngAllRows(const char* label,
                                          const unsigned char* data, unsigned int size,
                                          int targetVramX, int targetVramY,
                                          int targetVramW, int targetVramH,
                                          int targetClutX, int targetClutY,
                                          int rows, int originalBitDepth)
{
    unsigned char* rgba = NULL;
    int w = 0, h = 0, bpp = 0, r, ok = 0;

    /* BC7 .dds whole-image replacement on the VRAM-rect path (regular/map/HUD
     * textures — the pool/chara path already detects .dds in PoolSlotRegister).
     * Upload the compressed file at each palette row's clut cell, like the RGBA
     * loop below, so any palette a prim selects samples it. */
    {
        s_DdsBptc probe;
        if (Dds_ParseBptc(data, (int)size, &probe))
        {
            if (rows < 1) rows = 1;
            for (r = 0; r < rows; r++)
            {
                if (HiresOverride_RegisterDdsKeyed(label, data, (size_t)size,
                        targetVramX, targetVramY, targetVramW, targetVramH,
                        targetClutX, targetClutY + r, originalBitDepth, 0) == 0)
                    ok++;
                if (targetClutX < 0 || targetClutY < 0) break;
            }
            return (ok > 0) ? 0 : -1;
        }
    }

    if (decode_to_rgba(label, data, size, &rgba, &w, &h, &bpp, 0, NULL) != 0)
    {
        return -1;
    }
    if (rows < 1) rows = 1;
    for (r = 0; r < rows; r++)
    {
        /* Register the same image at each palette row's clut cell so every
         * palette a prim selects maps to it. No CLUT (16bpp / clut < 0) means
         * one palette-less entry — register once. */
        if (HiresOverride_RegisterRGBA(label, rgba, w, h,
                                       targetVramX, targetVramY, targetVramW, targetVramH,
                                       targetClutX, targetClutY + r, originalBitDepth) == 0)
        {
            ok++;
        }
        if (targetClutX < 0 || targetClutY < 0) break;
    }
    free(rgba);
    return (ok > 0) ? 0 : -1;
}

void HiresOverride_PoolSlotsReset(void)
{
    int i, r, live = 0;
    /* Same range as the loop below, and the map-init signal that arms the lazy
     * composer's post-load burst budget: a retained source outliving its slot
     * would compose the OLD map's art into the new occupant. */
    TexPackLazy_MapReset();
    /* Chara-pool slots (>= HIRES_POOL_CHARA_SLOT_BASE, incl. their row-spill
     * aliases) persist across map loads — pc_chara_pool.c loads each chara
     * TIM exactly once and re-registers only on a region file-idx change. */
    for (i = 0; i < HIRES_POOL_CHARA_SLOT_BASE; i++)
    {
        for (r = 0; r < HIRES_POOL_MAX_ROWS; r++)
        {
            if (g_poolSlots[i].glTexture[r] != 0)
            {
                glDeleteTextures(1, &g_poolSlots[i].glTexture[r]);
                g_poolSlots[i].glTexture[r] = 0;
                if (r == 0) live++;
            }
            pack_credit(&g_poolSlots[i].rowPackBytes[r]);
        }
        g_poolSlots[i].nativeW = 0;
        g_poolSlots[i].nativeH = 0;
    }
    if (live > 0)
    {
        SH_DBG("[POOLTEX] reset (%d slots freed)", live);
    }
}

/* Free one chara-pool slot AND its row-spill aliases (slot+64, slot+128, ...)
 * so a region-swapped chara TIM (JPN GreyChild<->Mumbler) can re-register. */
void HiresOverride_CharaPoolSlotReset(int slotId)
{
    int a, r;
    if (slotId < HIRES_POOL_CHARA_SLOT_BASE || slotId >= HIRES_POOL_SLOT_MAX)
    {
        return;
    }
    for (a = slotId; a < HIRES_POOL_SLOT_MAX; a += 64)
    {
        /* Row-spill aliases never carry a source of their own (only base slots
         * do, and the pool pack path caps at HIRES_POOL_MAX_ROWS), but dropping
         * the whole chain keeps this symmetric with the reset above. */
        TexPackLazy_DropSlot(a);
        for (r = 0; r < HIRES_POOL_MAX_ROWS; r++)
        {
            if (g_poolSlots[a].glTexture[r] != 0)
            {
                glDeleteTextures(1, &g_poolSlots[a].glTexture[r]);
                g_poolSlots[a].glTexture[r] = 0;
            }
            pack_credit(&g_poolSlots[a].rowPackBytes[r]);
        }
        g_poolSlots[a].nativeW = 0;
        g_poolSlots[a].nativeH = 0;
    }
}

void HiresOverride_InvalidateVramRect(int x, int y, int w, int h)
{
    int i = 0;
    while (i < g_numEntries)
    {
        HiresEntry* e = &g_entries[i];
        /* Pixel-rect overlap, or the upload stomping the entry's CLUT
         * cells (16 entries at 4bpp, 256 at 8bpp). Either way the entry no
         * longer reflects what a prim keyed to it samples. */
        int clutW = (e->originalBitDepth == 8) ? 256 : 16;
        int hitPixels = e->vramX < x + w && e->vramX + e->vramW > x &&
                        e->vramY < y + h && e->vramY + e->vramH > y;
        int hitClut = e->clutX >= 0 &&
                      e->clutX < x + w && e->clutX + clutW > x &&
                      e->clutY < y + h && e->clutY + 1 > y;
        if (hitPixels || hitClut)
        {
            static int s_invalLog = 0;
            if (s_invalLog < 64)
            {
                SH_DBG("[HIRES] invalidated stale entry vram=(%d,%d %dx%d) clut=(%d,%d) — VRAM rewritten at (%d,%d %dx%d)",
                       e->vramX, e->vramY, e->vramW, e->vramH, e->clutX, e->clutY, x, y, w, h);
                s_invalLog++;
            }
            if (e->glTexture != 0)
            {
                glDeleteTextures(1, &e->glTexture);
            }
            pack_credit(&e->packBytes);
            g_entries[i] = g_entries[--g_numEntries];
            continue;
        }
        i++;
    }
}

unsigned int HiresOverride_LookupByTpageClut(int tpage, int clut,
                                              int* outNativePixelW,
                                              int* outNativePixelH,
                                              int* outOffsetX,
                                              int* outOffsetY,
                                              int* outHiresW,
                                              int* outHiresH)
{
    /* Virtual pool slot: clut bit 15 set; slot id split across the clut X
     * bits + 16-row-spaced Y groups, row = the prim's baked +64*row palette
     * delta (see hires_override.h). Rows the TIM didn't ship fall back to
     * row 0. */
    if (clut & 0x8000)
    {
        /* Virtual pool slots only ever key 4/8bpp CLUT TIMs. A 16bpp (tp>=2)
         * tpage cannot legitimately address one: PSX hardware ignores `clut`
         * in 16bpp mode, so framebuffer-sampling prims (cutscene ghosting
         * overlays) ship uninitialized clut bytes. Honoring such a garbage key
         * hijacked a live pool texture = rainbow bar in cutscenes. */
        if (((tpage >> 7) & 0x3) >= 2)
        {
            static int s_fbClutLogged = 0;
            if (s_fbClutLogged < 8)
            {
                SH_DBG("[HIRES] ignored virtual clut 0x%04X on 16bpp tpage 0x%X (framebuffer prim)",
                       clut, tpage);
                s_fbClutLogged++;
            }
            return 0;
        }

        int q = ((clut >> 6) & 0x3FF) - HIRES_POOL_CLUT_ROW_BASE;
        if (q >= 0)
        {
            int slotId = (q / HIRES_POOL_MAX_ROWS) * 64 + (clut & 0x3F);
            int row    = q % HIRES_POOL_MAX_ROWS;
            if (slotId < HIRES_POOL_SLOT_MAX)
            {
                PoolSlotEntry* s      = &g_poolSlots[slotId];
                int            useRow = (s->glTexture[row] != 0) ? row : 0;
                GLuint         tex    = s->glTexture[useRow];
                /* Demand signal for the on-demand pack composer: this exact
                 * (slot, row) is being drawn, so it is worth the compose the
                 * loader no longer pays up front. Keyed on `row`, NOT `useRow` -
                 * the row a prim ASKED for is what should get the HD art even
                 * while it falls back to row 0, and keying on useRow would pin
                 * such a row to the fallback forever. Placed before the spill
                 * loop below, which rewrites slotId. */
                TexPackLazy_NoteWanted(slotId, row);
                /* Chara-range row spill (base+64k): when the alias slot is
                 * empty — a single-palette loose/PNG replacement registered
                 * only rows 0..15 of a >16-row TIM — fall back to the chara
                 * BASE slot's row 0 so those prims keep the documented row-0
                 * behavior instead of being dropped. */
                while (tex == 0 && slotId >= HIRES_POOL_CHARA_SLOT_BASE + 64)
                {
                    slotId -= 64;
                    s      = &g_poolSlots[slotId];
                    useRow = 0;
                    tex    = s->glTexture[0];
                }
                if (tex != 0)
                {
                    /* LRU key: this row is being SAMPLED right now. Stamped on
                     * the row actually served (useRow), which is the one whose
                     * texture eviction would delete. */
                    s->rowTick[useRow] = g_hiresTick;
                    if (outNativePixelW) *outNativePixelW = s->nativeW;
                    if (outNativePixelH) *outNativePixelH = s->nativeH;
                    if (outOffsetX)      *outOffsetX      = 0;
                    if (outOffsetY)      *outOffsetY      = 0;
                    if (outHiresW)       *outHiresW       = s->rowW[useRow];
                    if (outHiresH)       *outHiresH       = s->rowH[useRow];
                    return (unsigned int)tex;
                }
            }
        }
        return 0;
    }

    if (g_numEntries == 0) return 0;

    int tpx = (tpage & 0xF) * 64;
    int tpy = ((tpage >> 4) & 1) * 256;
    int tpFmt = (tpage >> 7) & 0x3;
    int bd = (tpFmt == 0) ? 4 : (tpFmt == 1) ? 8 : (tpFmt == 2) ? 16 : 0;
    int cx = (clut & 0x3F) * 16;
    int cy = (clut >> 6) & 0x1FF;

    /* A tpage samples pageCellW cells right / 256 rows down from its origin;
     * an entry matches when that window INTERSECTS its rect (origin-inside
     * is not enough: the second half-page pool slot starts 32 cells past its
     * page origin, and non-page-aligned TIMs hang left of the page a prim
     * addresses them through). The clut requirement disambiguates entries
     * sharing a page; offsets may come out negative — prim UVs inside the
     * TIM keep the final coordinate non-negative. */
    int pageCellW = (bd == 4) ? 64 : (bd == 8) ? 128 : 256;

    for (int i = 0; i < g_numEntries; i++)
    {
        HiresEntry* e = &g_entries[i];
        if (e->originalBitDepth != bd) continue;
        if (tpx >= e->vramX + e->vramW || tpx + pageCellW <= e->vramX) continue;
        if (tpy >= e->vramY + e->vramH || tpy + 256 <= e->vramY) continue;
        if (e->clutX >= 0)
        {
            if (cx != e->clutX || cy != e->clutY) continue;
        }
        {
            /* vramX/W are in 16-bit VRAM cells; texels per cell depends on
             * the bit depth. The offset is where this prim's tpage origin
             * sits relative to the replaced TIM, in native texels — prim UVs
             * restart at each tpage, so chunks past the first need it. */
            int pixelsPerCell;
            switch (bd) {
                case 4:  pixelsPerCell = 4; break;
                case 8:  pixelsPerCell = 2; break;
                default: pixelsPerCell = 1; break;
            }
            if (outNativePixelW) *outNativePixelW = e->vramW * pixelsPerCell;
            if (outNativePixelH) *outNativePixelH = e->vramH;
            if (outOffsetX)      *outOffsetX      = (tpx - e->vramX) * pixelsPerCell;
            if (outOffsetY)      *outOffsetY      = tpy - e->vramY;
            if (outHiresW)       *outHiresW       = e->hiresW;
            if (outHiresH)       *outHiresH       = e->hiresH;
        }
        e->tick = g_hiresTick; /* LRU key: sampled now */
        return (unsigned int)e->glTexture;
    }
    return 0;
}

void HiresOverride_LogStats(void)
{
    SH_DBG("[HIRES] %d active overrides", g_numEntries);
    for (int i = 0; i < g_numEntries; i++)
    {
        HiresEntry* e = &g_entries[i];
        SH_DBG("  [%d] vram=(%d,%d %dx%d %dbpp) clut=(%d,%d) hires=%dx%d tex=%u",
               i, e->vramX, e->vramY, e->vramW, e->vramH,
               e->originalBitDepth, e->clutX, e->clutY,
               e->hiresW, e->hiresH, (unsigned)e->glTexture);
    }
}
