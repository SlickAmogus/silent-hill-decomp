/*
 * xbox_respool.c - resident chunk textures on Xbox (PC's resident_textures).
 *
 * THE PROBLEM IT SOLVES. The game's chunk-texture pool is 8 full VRAM pages plus
 * 2 half pages. When more distinct chunk materials are visible than the pool
 * holds, Texture_Get steals a page: the TIM that owned it is overwritten in
 * place, and every prim still pointing at that page samples whatever moved in.
 * That is the flat/rainbow/wrong-texture class. PSX shipped with it because its
 * draw distance kept the working set inside 10 pages; this port draws far more
 * of the map, so the pool overflows constantly.
 *
 * PC's fix (resident_textures) appends VIRTUAL slots to the pool. A virtual slot
 * never uploads to VRAM -- Fs_QueuePostLoadTim skips both LoadImage calls and
 * hands the TIM here instead -- and the prims address it through a synthetic
 * clut word (encoding in pc_port/include/hires_override.h). Nothing can steal a
 * virtual slot's pixels because it does not share VRAM with anything. ALL of
 * that machinery is shared decomp code compiled under SH_PC_PORT, so the Xbox
 * already has it; what was missing is the backing store, and it was pinned off
 * (xbox_compat_globals.c) because PC's backing store is not affordable here.
 *
 * WHY THIS IS NOT PC'S IMPLEMENTATION. PC keeps a decoded RGBA8 texture per
 * CLUT row -- 256KB per row, and chunk TIMs ship up to 12 rows. With 256 slots
 * that is tens of megabytes. In-game free RAM here is 3-4MB.
 *
 * What Xbox stores instead is the TIM's PSX PIXEL BLOCK VERBATIM: the exact
 * 16-bit word rectangle that would have been LoadImage'd into a VRAM page. A
 * full page is 64 words x 256 rows = 32KB, and it cannot be larger, because a
 * TIM that did not fit a real page could not have been pool-assigned in the
 * first place. That is 8x smaller than one decoded RGBA row, and it plugs
 * straight into the existing paletted cache: PageDecodeIndices already walks
 * exactly this layout, it just reads s_vram today. So a virtual slot costs
 * 32KB of system RAM and NOTHING on the GPU -- the index pages and palettes it
 * decodes into are the same LRU-managed pool every real page already shares.
 *
 * The slabs are reserved UP FRONT and the offered slot count is derived from
 * how many were actually reserved (Xbox_ResidentFullSlots). A slot must never be
 * offered that cannot be backed: Fs_QueuePostLoadTim has already skipped the
 * VRAM upload by the time registration runs, so a failure there renders that
 * material broken rather than merely un-improved. Reserving first makes the
 * offer a promise the store can keep, and if the reservation fails outright the
 * pool stays vanilla.
 */
#include <stdlib.h>
#include <string.h>
#include "xbox_respool.h"
#include "sh_log.h"
#include "pc_config.h"   /* texture_paletted gates the decode path we depend on */

/* One slab = one PSX full tpage of 16-bit words (64 words x 256 rows). Half-page
 * materials use half of one and waste the rest; there are far fewer of them. */
#define RES_SLAB_WORDS   (64 * 256)
#define RES_SLAB_BYTES   (RES_SLAB_WORDS * 2)

/* CLUT rows a slot can hold. HIRES_POOL_MAX_ROWS in the shared encoding -- a
 * prim selects its row with a baked +64*row clut delta, so rows beyond this
 * alias into the next slot's key space and are not addressable anyway. */
#define RES_MAX_ROWS     16
#define RES_CLUT_WORDS   256                  /* 8bpp worst case; 4bpp uses 16 */

/* Reservation ceiling, and the free-RAM floor it stops at. Slabs are taken one
 * at a time while the console still has RES_FREE_FLOOR_KB left, so a map that is
 * already tight simply gets fewer resident slots instead of an OOM -- the pool
 * degrades toward vanilla rather than failing. The reserve happens at map init,
 * BEFORE the adaptive ARGB texture cache grows into what remains, so that cache
 * sizes itself around this rather than fighting it. 48 would put 8 vanilla + 40
 * full pages in play against vanilla's 8. */
#define RES_SLABS_MAX      48
#define RES_FREE_FLOOR_KB  6144

typedef struct {
    unsigned short* words;      /* pixel block, PSX VRAM word layout */
    unsigned short* clut;       /* RES_MAX_ROWS * RES_CLUT_WORDS, or 0 */
    int             pitchWords; /* words per row (= prect.w) */
    int             w, h;       /* pixel dimensions */
    int             bpp;        /* 4 or 8; 16-bit TIMs are not pool-assigned */
    int             rows;       /* CLUT rows present */
    int             slot;       /* owning slot id, or -1 when free */
} ResSlab;

static ResSlab s_slabs[RES_SLABS_MAX];
static int     s_slabCount;                 /* slabs actually reserved */
static int     s_inited;

/* slot id -> slab index, or -1. Covers the chunk range only (0..255); the chara
 * range and the minimap's own slots go through the RGBA table in
 * xbox_pcfeature_stubs.c instead. */
#define RES_SLOT_MAX 256
static short   s_slotSlab[RES_SLOT_MAX];

static void ResInit(void)
{
    int i;

    if (s_inited)
        return;
    s_inited = 1;

    for (i = 0; i < RES_SLOT_MAX; i++)
        s_slotSlab[i] = -1;

    /* The paletted cache IS the decode path for resident slots (psx_vram.c), so
     * with it disabled a resident slot would resolve to nothing and its prims
     * would vanish. Offer none in that case and let the pool stay vanilla. */
    if (!g_PcConfig.xboxPalettedTex) {
        SH_DBG("[RESPOOL] texture_paletted=0 — resident chunk textures unavailable");
        return;
    }

    for (i = 0; i < RES_SLABS_MAX; i++) {
        unsigned short* px;
        unsigned short* cl;

        if (Xbox_MemFreeKB() < RES_FREE_FLOOR_KB)
            break;                             /* leave the caches their headroom */

        px = (unsigned short*)malloc(RES_SLAB_BYTES);
        if (!px)
            break;
        cl = (unsigned short*)malloc(RES_MAX_ROWS * RES_CLUT_WORDS * 2);
        if (!cl) { free(px); break; }

        s_slabs[i].words = px;
        s_slabs[i].clut  = cl;
        s_slabs[i].slot  = -1;
        s_slabCount++;
    }

    SH_DBG("[RESPOOL] reserved %d/%d slabs (%dKB pixels + %dKB cluts), free=%uKB",
           s_slabCount, RES_SLABS_MAX,
           (s_slabCount * RES_SLAB_BYTES) / 1024,
           (s_slabCount * RES_MAX_ROWS * RES_CLUT_WORDS * 2) / 1024,
           Xbox_MemFreeKB());
}

/* How many virtual FULL-page slots the chunk pool may offer. The half-page
 * extras come out of the same slabs, so the split below keeps a few back for
 * them rather than letting full pages claim everything. */
int Xbox_ResidentFullSlots(void)
{
    ResInit();
    if (s_slabCount <= 8)
        return 0;                              /* not worth the bookkeeping */
    return s_slabCount - (s_slabCount / 6);
}

int Xbox_ResidentHalfSlots(void)
{
    ResInit();
    if (s_slabCount <= 8)
        return 0;
    return s_slabCount / 6;
}

/* Map init. The chunk range is per-map, so every slab returns to the free list;
 * the caller (HiresOverride_PoolSlotsReset) also covers the PC chara range,
 * which never lands here. */
void Xbox_ResidentPoolReset(void)
{
    int i;

    if (!s_inited)
        return;
    for (i = 0; i < s_slabCount; i++)
        s_slabs[i].slot = -1;
    for (i = 0; i < RES_SLOT_MAX; i++)
        s_slotSlab[i] = -1;
}

/* Claim (or re-claim) the slab for `slot`. Engine slot reuse re-registers the
 * same id with new content, so an existing binding is overwritten in place. */
static ResSlab* ResSlabFor(int slot)
{
    int i;

    if (slot < 0 || slot >= RES_SLOT_MAX)
        return 0;
    if (s_slotSlab[slot] >= 0)
        return &s_slabs[s_slotSlab[slot]];

    for (i = 0; i < s_slabCount; i++) {
        if (s_slabs[i].slot < 0) {
            s_slabs[i].slot  = slot;
            s_slotSlab[slot] = (short)i;
            return &s_slabs[i];
        }
    }
    return 0;
}

/* Decode a disc TIM into `slot`. Layout: u32 magic 0x10, u32 flags (bpp in bits
 * 0-1, CLUT present in bit 3), optional CLUT block, then the pixel block; both
 * blocks are u32 byteSize + u16 x,y,w,h with w counted in 16-bit WORDS. The
 * pixel words are copied verbatim -- no expansion, no palette application --
 * because the paletted cache consumes exactly this form. */
int Xbox_ResidentPoolRegisterTim(int slot, const unsigned char* tim, unsigned size,
                                 int nativeW, int nativeH)
{
    const unsigned char* p = tim;
    ResSlab*  sl;
    unsigned  flags, bppCode;
    const unsigned short* clutSrc = 0;
    int       clutRows = 0, clutRowWords = 0;
    int       pw, ph, y;

    ResInit();
    if (!tim || size < 20 || *(const unsigned*)p != 0x00000010u)
        return -1;

    flags   = *(const unsigned*)(p + 4);
    bppCode = flags & 3;                       /* 0 = 4bpp, 1 = 8bpp, 2 = 16bpp */
    if (bppCode > 1)
        return -1;                             /* 16/24bpp are never pool-assigned */
    p += 8;

    if (flags & 8) {                           /* CLUT block */
        unsigned csz = *(const unsigned*)p;
        if (csz < 12 || (unsigned)(p - tim) + csz > size)
            return -1;
        clutRowWords = *(const unsigned short*)(p + 8);
        clutRows     = *(const unsigned short*)(p + 10);
        clutSrc      = (const unsigned short*)(p + 12);
        if (clutRows > RES_MAX_ROWS) clutRows = RES_MAX_ROWS;
        if (clutRowWords > RES_CLUT_WORDS) clutRowWords = RES_CLUT_WORDS;
        p += csz;
    }
    if ((unsigned)(p - tim) + 12 > size)
        return -1;

    pw = *(const unsigned short*)(p + 8);      /* width in 16-bit words */
    ph = *(const unsigned short*)(p + 10);
    p += 12;

    if (pw <= 0 || ph <= 0 || (unsigned)(p - tim) + (unsigned)(pw * ph * 2) > size)
        return -1;
    /* A TIM too large for one page could not have been assigned a pool slot, so
     * this is a corrupt read rather than a size we should grow for. */
    if (pw > 64 || ph > 256) {
        SH_DBG("[RESPOOL] slot %d: %dx%d words exceeds one page — not resident", slot, pw, ph);
        return -1;
    }

    sl = ResSlabFor(slot);
    if (!sl) {
        SH_DBG("[RESPOOL] slot %d: no free slab (%d in use)", slot, s_slabCount);
        return -1;
    }

    for (y = 0; y < ph; y++)
        memcpy(sl->words + (size_t)y * pw, (const unsigned short*)p + (size_t)y * pw,
               (size_t)pw * 2);

    if (clutSrc && clutRows > 0) {
        int r;
        memset(sl->clut, 0, RES_MAX_ROWS * RES_CLUT_WORDS * 2);
        for (r = 0; r < clutRows; r++)
            memcpy(sl->clut + (size_t)r * RES_CLUT_WORDS,
                   clutSrc + (size_t)r * clutRowWords,
                   (size_t)clutRowWords * 2);
    }

    sl->pitchWords = pw;
    sl->h          = ph;
    sl->bpp        = bppCode ? 8 : 4;
    sl->w          = bppCode ? (pw * 2) : (pw * 4);
    sl->rows       = clutRows;

    (void)nativeW; (void)nativeH;              /* == sl->w/h for a disc TIM */
    return 0;
}

/* Read side, for psx_vram.c's paletted decode. Returns 0 when the slot is not
 * resident, which the caller must treat as "fall through to the RGBA table". */
const unsigned short* Xbox_ResidentPoolGet(int slot, s_XbResidentDesc* out)
{
    ResSlab* sl;

    if (!s_inited || slot < 0 || slot >= RES_SLOT_MAX)
        return 0;
    if (s_slotSlab[slot] < 0)
        return 0;
    sl = &s_slabs[s_slotSlab[slot]];
    if (!sl->words || sl->pitchWords <= 0)
        return 0;

    if (out) {
        out->pitchWords = sl->pitchWords;
        out->w          = sl->w;
        out->h          = sl->h;
        out->bpp        = sl->bpp;
        out->rows       = sl->rows;
        out->clut       = sl->clut;
    }
    return sl->words;
}
