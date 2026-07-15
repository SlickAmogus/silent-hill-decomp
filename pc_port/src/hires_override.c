#include "hires_override.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PNG overrides carry a true 8-bit alpha channel (TIM transparency is
 * 1-bit: raw value 0 = transparent). stb_image is vendored, PNG-only,
 * memory-only; its default allocator is malloc so the shared free() below
 * is valid for both decode paths. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include "sh_log.h"

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
} HiresEntry;

static HiresEntry g_entries[MAX_HIRES_OVERRIDES];
static int        g_numEntries = 0;
static int        g_initialized = 0;

static int upload_rgba(GLuint* tex, const unsigned char* rgba, int w, int h, int nearest);

/* ---- Texture-pack GL byte budget ------------------------------------------
 * A DuckStation pack composes + uploads a pack-resolution RGBA texture (with
 * mipmaps) for EVERY CLUT row of EVERY claimed page. The whole-town render
 * mode claims most of a street map's pages, which multiplied out to gigabytes
 * and hard-hung the machine (2026-07-12 Levin-house load). Every pack-composed
 * upload is charged here (mip chain ~= 4/3x) and credited when its texture is
 * replaced or deleted; fsqueue_3.c stops composing further rows once the
 * budget is spent, so those rows keep the native disc art. Claims are made
 * nearest-first (Ipd_ChunkMaterialsApply), so the budget favors what is close.
 * Normal streamed play churns slots in place and never approaches the cap. */
#define SH_TEXPACK_BUDGET_BYTES ((long long)768 << 20)
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

int HiresOverride_PackBudgetExceeded(void)
{
    return g_packBytesLive >= SH_TEXPACK_BUDGET_BYTES;
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
} PoolSlotEntry;

static PoolSlotEntry g_poolSlots[HIRES_POOL_SLOT_MAX];

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

    snprintf(tag, sizeof(tag), "pool slot %d", slotId);

    /* One texture per CLUT row: prims select palette rows with baked +64*row
     * clut deltas (see hires_override.h), so every row a TIM ships must be
     * decodable at draw. PNG replacements have one palette: row 0 only. */
    PoolSlotEntry* s = &g_poolSlots[slotId];
    int rows = 1, r, hiW = 0, hiH = 0;

    for (r = 0; r < rows; r++)
    {
        unsigned char* rgba = NULL;
        int w = 0, h = 0, srcBpp = 0, timRows = 1;
        if (decode_to_rgba(tag, data, size, &rgba, &w, &h, &srcBpp, r, &timRows) != 0)
        {
            return (r == 0) ? -1 : 0;
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
                return (r == 0) ? -1 : 0;
            }
            free(rgba);
            rowSlot->rowW[rowIdx] = (unsigned short)w;
            rowSlot->rowH[rowIdx] = (unsigned short)h;
            /* This row now holds base/loose content — release any pack charge a
             * previous occupant of the slot left on it. */
            pack_credit(&rowSlot->rowPackBytes[rowIdx]);
        }
    }

    /* Slot reuse with fewer rows: drop stale row textures past the new count. */
    for (r = rows; r < HIRES_POOL_MAX_ROWS; r++)
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
    return 0;
}

/* Upload straight RGBA into a slot/entry texture, creating it on demand.
 * Upscaled replacements (non-nearest) get mipmaps so they don't shimmer at
 * distance the way a raw LINEAR-sampled 4x texture does; native-res decodes
 * stay NEAREST with no mips (PSX-exact). Returns 0 on success. */
static int upload_rgba(GLuint* tex, const unsigned char* rgba, int w, int h, int nearest)
{
    if (rgba == NULL || w <= 0 || h <= 0) return -1;
    if (*tex == 0)
    {
        glGenTextures(1, tex);
        if (*tex == 0) return -1;
    }
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
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

int HiresOverride_PoolSlotRegisterRGBA(int slotId, int row,
                                       const unsigned char* rgba, int w, int h,
                                       int nativePixelW, int nativePixelH)
{
    if (!g_initialized) HiresOverride_Init();
    if (slotId < 0 || slotId >= HIRES_POOL_SLOT_MAX ||
        row < 0 || row >= HIRES_POOL_MAX_ROWS)
    {
        SH_DBG("[POOLTEX] slot %d row %d out of range (RGBA)", slotId, row);
        return -1;
    }

    PoolSlotEntry* s = &g_poolSlots[slotId];
    if (upload_rgba(&s->glTexture[row], rgba, w, h,
                    (w == nativePixelW && h == nativePixelH)) != 0)
    {
        return -1;
    }
    pack_charge(&s->rowPackBytes[row], w, h);
    s->rowW[row] = (unsigned short)w;
    s->rowH[row] = (unsigned short)h;
    s->nativeW = nativePixelW;
    s->nativeH = nativePixelH;
    return 0;
}

int HiresOverride_RegisterRGBA(const char* label,
                               const unsigned char* rgba, int w, int h,
                               int targetVramX, int targetVramY,
                               int targetVramW, int targetVramH,
                               int targetClutX, int targetClutY,
                               int originalBitDepth)
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
    if (e == NULL)
    {
        if (g_numEntries >= MAX_HIRES_OVERRIDES)
        {
            SH_DBG("[HIRES] table full, ignoring %s", label);
            return -1;
        }
        e = &g_entries[g_numEntries];
        e->glTexture = 0;
    }

    if (upload_rgba(&e->glTexture, rgba, w, h, 0) != 0)
    {
        return -1;
    }
    if (e == &g_entries[g_numEntries])
    {
        e->packBytes = 0;
        g_numEntries++;
    }
    pack_charge(&e->packBytes, w, h);

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

void HiresOverride_PoolSlotsReset(void)
{
    int i, r, live = 0;
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
                    if (outNativePixelW) *outNativePixelW = s->nativeW;
                    if (outNativePixelH) *outNativePixelH = s->nativeH;
                    if (outOffsetX)      *outOffsetX      = 0;
                    if (outOffsetY)      *outOffsetY      = 0;
                    /* Pool-slot content (world map chunks, characters, decals) is
                     * CONTINUOUS imagery, not a glyph atlas: the per-native-texel
                     * footprint clamp (u_hiresHalf) would drop a seam at every
                     * native-texel boundary. Report 0 so the shader keeps the
                     * smooth free-linear sampling these textures had before. The
                     * clamp still applies to the VRAM-entry overrides below (menu
                     * fonts / cursor) -- those ARE atlases and are where the
                     * edge-bleed artifact actually was. (rowW/rowH stay tracked in
                     * case a future per-slot atlas case needs them.) */
                    if (outHiresW)       *outHiresW       = 0;
                    if (outHiresH)       *outHiresH       = 0;
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
