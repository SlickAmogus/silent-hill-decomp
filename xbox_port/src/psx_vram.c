/*
 * psx_vram.c - PSX VRAM (1024x512, 16-bit) emulation + texture decode for NV2A.
 *
 * LoadImage writes TIM pixel data into VRAM. Textured prims (gpu_xbox.c) ask for
 * a texture by (tpage, clut): we decode that 256x256 texel page out of VRAM into
 * an A8R8G8B8 buffer — 4-bit/8-bit indexed via the CLUT, or 16-bit direct — cache
 * it (keyed by tpage|clut, invalidated whenever VRAM is written), and hand back a
 * GPU-DMA-able pointer. PSX texel UVs (0..255) index the decoded page 1:1, so
 * gpu_xbox.c needs no UV scaling.
 *
 * PSX 16-bit colour is 1-5-5-5 (STP,B,G,R); 0x0000 = fully transparent.
 */
#include <stdint.h>
#include <string.h>
#include "gpu_nv2a.h"
#include "sh_log.h"
#include "sh_hwperf.h"
#include "pc_config.h"    /* texture_paletted escape hatch */
#include "hires_override.h" /* HIRES_POOL_* virtual-slot clut encoding */
#include "xbox_respool.h"   /* resident chunk textures (resident_textures) */

#define VRAM_W   1024
#define VRAM_H   512
#define TEX_DIM  256                 /* a PSX texture page is 256x256 texels */
/* 96 * 256KB = 24 MB of decoded textures. 48 fit map0_s00, but the cafe
 * cutscene's working set (interior + Cybil + Harry closeups + air-screamer
 * effects, each (tpage,clut) pair its own slot) exceeded it and the round-robin
 * eviction thrashed: 38k decodes in one session = <1 fps. 64MB RAM has room. */
#define CACHE_N  96

static uint16_t s_vram[VRAM_W * VRAM_H];

typedef struct {
    int       key;
    int       lastKey;        /* key before an invalidation cleared it (miss-cause probe) */
    uint32_t* argb;
    unsigned  lastUse;        /* frame of last GetTexture hit (LRU eviction) */
    unsigned  seq;            /* monotonic bind order; > s_drainedSeq => GPU may still read it */
    unsigned  hits;           /* aged use count: protects every-frame pages from churn */
    /* Source-VRAM footprint of this decoded page, in 16-bit words, [x0,x1) x
     * [y0,y1). Page rect and CLUT rect are tracked separately so a CLUT-only
     * write still invalidates an indexed page that samples it. */
    int px0, py0, px1, py1;   /* texture-page rect */
    int cx0, cy0, cx1, cy1;   /* CLUT rect (empty for 16-bit direct) */
} TexEntry;
/* ---- Paletted texture path -------------------------------------------------
 * An indexed PSX page is cached ONCE per tpage as 8-bit indices (64KB) and its
 * CLUT becomes a GPU palette (1KB). The ARGB path below caches every
 * (tpage,clut) PAIR as a full 256KB decode, so a page drawn with five palettes
 * cost 1.25MB and five decodes; log 050 measured that as 20.6ms of a 31.8ms
 * render walk, and log 048 showed 99.5% of misses were capacity evictions.
 * 64KB per page instead of 256KB per pair is ~4x the effective capacity, and the
 * CLUT no longer forces a re-decode at all.
 *
 * QUALITY IS UNCHANGED. Palette entries are the same full A8R8G8B8 values
 * Psx16ToArgb already produces, including all three PSX alpha levels (0 =
 * transparent, 0x80 = STP semi-transparent, 0xFF = opaque). This is a memory
 * layout change, not a precision one.
 *
 * The NV2A's only palettized format is SWIZZLED (SZ_I8_A8R8G8B8), so indices are
 * stored Morton-interleaved. The scatter runs in a CACHED scratch buffer and is
 * copied out sequentially -- scattering straight into write-combined memory
 * would hit the very partial-line eviction cost that made vertex staging slow
 * (7c62ad599). 16-bit direct pages cannot be palettized and still use the ARGB
 * cache below; they are the rare framebuffer-feedback textures. */
#define PAGE_N      64                        /* 64 * 64KB = 4MB of index pages */
#define PAL_N       128                       /* 128 * 1KB  = 128KB of palettes */
#define PAGE_BYTES  (TEX_DIM * TEX_DIM)
#define PAL_ENTRIES 256

typedef struct {
    int       key;                            /* tpage, or -1 */
    uint8_t*  data;                           /* swizzled indices */
    unsigned  lastUse, seq, hits;
    int       px0, py0, px1, py1;             /* source page rect (invalidation) */
} PageEntry;

typedef struct {
    int       key;                            /* clut, or -1 */
    uint32_t* data;                           /* PAL_ENTRIES A8R8G8B8 */
    unsigned  lastUse, seq;
    int       cx0, cy0, cx1, cy1;             /* source CLUT rect (invalidation) */
} PalEntry;

static PageEntry s_pages[PAGE_N];
static PalEntry  s_pals[PAL_N];
static int       s_palPathReady;
static unsigned  s_swzX[TEX_DIM], s_swzY[TEX_DIM];
static uint8_t   s_swzScratch[PAGE_BYTES];    /* cached staging for the scatter */

static TexEntry s_cache[CACHE_N];
static int      s_cacheReady;
static int      s_cacheAlloc;   /* slots with real memory (<= CACHE_N) */
static int      s_memoSlot = -1;/* last slot returned; re-checked against key before reuse */
static unsigned s_bindSeq;      /* increments on every bind (hit or fill) */
static unsigned s_drainedSeq;   /* s_bindSeq at the last GPU drain: everything <= this is consumed */
static unsigned s_ageFrame;     /* last frame the hit counts were halved */
static int      s_decodeTotal;
static int      s_stpLogged;    /* [STP] census: only the first 12 decodes are logged */
unsigned        g_PsxDecodeMs;  /* ms spent in DecodePage this frame (gpu_nv2a.c reports it) */
unsigned        g_PsxDecodeCount;

extern int g_Nv2aFrameCount;  /* LRU clock (gpu_nv2a.c, ticks per frame) */

/* Record the VRAM footprint for (tpage,clut) using the SAME derivation as
 * DecodePage, so the overlap test below matches exactly what was sampled. */
static void TexEntry_SetBBox(TexEntry* e, int tpage, int clut)
{
    int px = (tpage & 0x0F) * 64;
    int py = ((tpage >> 4) & 1) * 256;
    int tp = (tpage >> 7) & 3;                         /* 0=4bit 1=8bit 2,3=16bit */
    int pw = (tp == 0) ? 64 : (tp == 1) ? 128 : 256;   /* page width in words */
    e->px0 = px;      e->py0 = py;
    e->px1 = px + pw; e->py1 = py + 256;
    if (tp <= 1) {                                     /* indexed: CLUT participates */
        int cx = (clut & 0x3F) * 16;
        int cy = (clut >> 6) & 0x1FF;
        int cw = (tp == 0) ? 16 : 256;
        e->cx0 = cx;      e->cy0 = cy;
        e->cx1 = cx + cw; e->cy1 = cy + 1;
    } else {
        e->cx0 = e->cx1 = e->cy0 = e->cy1 = 0;         /* 16-bit: no CLUT */
    }
}

static int RectsOverlap(int ax0, int ay0, int ax1, int ay1,
                        int bx0, int by0, int bx1, int by1)
{
    if (ax0 >= ax1 || ay0 >= ay1) return 0;            /* empty rect never overlaps */
    return ax0 < bx1 && bx0 < ax1 && ay0 < by1 && by0 < ay1;
}

/* Drop only the cache entries whose page OR clut rect overlaps the written rect.
 * The old code nuked all 48 entries on every LoadImage, forcing the whole
 * textured working set to re-decode (65536 texels/page) on the 733 MHz CPU every
 * frame the game touched VRAM — the dominant in-game cost. */
static void InvalidateRegion(int wx0, int wy0, int wx1, int wy1)
{
    int i;
    for (i = 0; i < CACHE_N; i++) {
        TexEntry* e = &s_cache[i];
        if (e->key == -1) continue;
        if (RectsOverlap(e->px0, e->py0, e->px1, e->py1, wx0, wy0, wx1, wy1) ||
            RectsOverlap(e->cx0, e->cy0, e->cx1, e->cy1, wx0, wy0, wx1, wy1)) {
            e->lastKey = e->key;   /* survives the kill: lets a later miss say WHY */
            e->key     = -1;
            if (i == s_memoSlot) s_memoSlot = -1;
        }
    }
    /* Paletted caches. The point of splitting them is that a CLUT write no longer
     * touches the index page and a texel write no longer touches the palette --
     * the animated-palette churn that cost a 256KB re-decode now costs a 1KB
     * palette rebuild. */
    for (i = 0; i < PAGE_N; i++) {
        PageEntry* e = &s_pages[i];
        if (e->key == -1) continue;
        if (RectsOverlap(e->px0, e->py0, e->px1, e->py1, wx0, wy0, wx1, wy1))
            e->key = -1;
    }
    for (i = 0; i < PAL_N; i++) {
        PalEntry* e = &s_pals[i];
        if (e->key == -1) continue;
        if (RectsOverlap(e->cx0, e->cy0, e->cx1, e->cy1, wx0, wy0, wx1, wy1))
            e->key = -1;
    }
    /* Do NOT reset s_cacheNext: round-robin reuse still finds key==-1 slots. */
}

/* The USA font atlas is a 16-row strip at VRAM y=496..511, words x=0..255 (one
 * 21-glyph row per tpage 16..19). The glyph prims reaching the GPU are provably
 * correct -- their cell/page decode inverts the game's own atlas formula -- so
 * the remaining suspect for garbled text is the atlas CONTENT. These two probes
 * settle it: FONTCLOB names anyone writing into the strip, FONTDUMP prints the
 * bitmap itself (once at the menu where text is correct, once in-game where it
 * is garbled, so a clobber shows as a diff). */
#define FONT_STRIP_Y0 496
#define FONT_STRIP_Y1 512
#define FONT_STRIP_X1 256

static void FontStripWriteProbe(const char* what, int x, int y, int w, int h)
{
    static int s_n = 0;
    if (s_n < 40 && x < FONT_STRIP_X1 && x + w > 0 &&
        y < FONT_STRIP_Y1 && y + h > FONT_STRIP_Y0) {
        s_n++;
        SH_DBG("[FONTCLOB] %s x=%d y=%d w=%d h=%d", what, x, y, w, h);
    }
}

void PsxVram_DumpFontStrip(const char* tag)
{
    int p, v, u;
    char line[144];

    for (p = 16; p <= 19; p++) {
        int tx = (p & 0x0F) * 64;
        SH_DBG("[FONTDUMP] %s tpage=%d words=%d..%d (21 glyphs, 6 cols each)",
               tag, p, tx, tx + 63);
        for (v = 0; v < 16; v++) {
            const uint16_t* row = &s_vram[((FONT_STRIP_Y0 + v) & (VRAM_H - 1)) * VRAM_W];
            int n = 0;
            for (u = 0; u < 252; u += 2) {   /* 2:1 horizontal, 12px glyph -> 6 cols */
                uint16_t word = row[(tx + (u >> 2)) & (VRAM_W - 1)];
                line[n++] = (((word >> ((u & 3) * 4)) & 0x0F) != 0) ? '#' : '.';
            }
            line[n] = '\0';
            SH_DBG("[FONTDUMP] %s %2d|%s", tag, v, line);
        }
    }
}

/* LoadImage: copy w*h 16-bit source pixels into VRAM at (x,y), row by row. */
void PsxVram_Load(int x, int y, int w, int h, const uint16_t* src)
{
    int row;
    int cx0 = w, cx1 = -1, cy0 = -1, cy1 = -1;   /* exact changed sub-rect */

    if (!src || w <= 0 || h <= 0 || x < 0 || y < 0 || x >= VRAM_W || y >= VRAM_H)
        return;

    if (x + w > VRAM_W) w = VRAM_W - x;
    FontStripWriteProbe("Load", x, y, w, h);

    /* Invalidate the sub-rect that ACTUALLY changed, not the whole upload.
     *
     * This was a boolean: any differing word anywhere dirtied the entire write
     * rect, killing every cached page overlapping it. The game uploads CLUTs in
     * blocks, so one changed palette threw out every page whose palette merely
     * shared the block -- each costing a 256KB re-decode. Log 047 measured 20ms
     * per frame (~36 decodes) inside DecodePage during the cafe fight, with only
     * 961 drains against 25088 decodes: the working set FITS in the cache, so
     * those decodes were not capacity, they were pages being invalidated and
     * immediately wanted again. Narrowing to the true changed extent is exact --
     * regions provably identical cannot need invalidating -- so it cannot serve
     * stale texels. */
    for (row = 0; row < h; row++) {
        int             vy = y + row;
        uint16_t*       d;
        const uint16_t* s;
        int             a, b;

        if (vy >= VRAM_H) break;
        d = &s_vram[vy * VRAM_W + x];
        s = &src[row * w];

        a = 0;
        b = w - 1;
        while (a <= b && d[a] == s[a]) a++;
        if (a <= b) {
            while (b > a && d[b] == s[b]) b--;
            if (a < cx0) cx0 = a;
            if (b > cx1) cx1 = b;
            if (cy0 < 0)  cy0 = vy;
            cy1 = vy + 1;
        }
        memcpy(d, s, (size_t)w * 2);
    }
    if (cy0 >= 0)
        InvalidateRegion(x + cx0, cy0, x + cx1 + 1, cy1);
}

/* MoveImage / DR_MOVE: VRAM->VRAM rectangle copy (water refraction, copy-based
 * effects). memmove row-wise handles overlap; the destination region's cached
 * textures are invalidated like a LoadImage. */
void PsxVram_Move(int sx, int sy, int w, int h, int dx, int dy)
{
    int row;

    if (w <= 0 || h <= 0)
        return;
    if (sx < 0 || sy < 0 || dx < 0 || dy < 0)
        return;
    if (sx + w > VRAM_W) w = VRAM_W - sx;
    if (dx + w > VRAM_W) w = VRAM_W - dx;
    if (w <= 0)
        return;
    FontStripWriteProbe("Move", dx, dy, w, h);

    if (dy <= sy) {
        for (row = 0; row < h; row++) {
            int syy = sy + row, dyy = dy + row;
            if (syy >= VRAM_H || dyy >= VRAM_H) break;
            memmove(&s_vram[dyy * VRAM_W + dx], &s_vram[syy * VRAM_W + sx], (size_t)w * 2);
        }
    } else {
        for (row = h - 1; row >= 0; row--) {
            int syy = sy + row, dyy = dy + row;
            if (syy >= VRAM_H || dyy >= VRAM_H) continue;
            memmove(&s_vram[dyy * VRAM_W + dx], &s_vram[syy * VRAM_W + sx], (size_t)w * 2);
        }
    }
    InvalidateRegion(dx, dy, dx + w, dy + h);
}

/* ClearImage: fill a VRAM rect with a BGR555 colour (colour wipes/flashes).
 * Repeated identical fills (per-frame interlaced clears) don't dirty the cache. */
void PsxVram_Fill(int x, int y, int w, int h, uint16_t c)
{
    int row, col;
    int cx0 = w, cx1 = -1, cy0 = -1, cy1 = -1;   /* exact changed sub-rect */

    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x >= VRAM_W || y >= VRAM_H)
        return;
    if (x + w > VRAM_W) w = VRAM_W - x;
    FontStripWriteProbe("Fill", x, y, w, h);
    for (row = 0; row < h; row++) {
        int vy = y + row;
        uint16_t* d;
        if (vy >= VRAM_H) break;
        d = &s_vram[vy * VRAM_W + x];
        for (col = 0; col < w; col++) {
            if (d[col] != c) {
                d[col] = c;
                if (col < cx0) cx0 = col;
                if (col > cx1) cx1 = col;
                if (cy0 < 0)   cy0 = vy;
                cy1 = vy + 1;
            }
        }
    }
    if (cy0 >= 0)
        InvalidateRegion(x + cx0, cy0, x + cx1 + 1, cy1);
}

/* StoreImage: read VRAM back out (rarely used; provided for completeness). */
void PsxVram_Store(int x, int y, int w, int h, uint16_t* dst)
{
    int row;

    if (!dst || w <= 0 || h <= 0 || x < 0 || y < 0 || x >= VRAM_W || y >= VRAM_H)
        return;

    if (x + w > VRAM_W) w = VRAM_W - x;
    for (row = 0; row < h; row++) {
        int vy = y + row;
        if (vy >= VRAM_H) break;
        memcpy(&dst[row * w], &s_vram[vy * VRAM_W + x], (size_t)w * 2);
    }
}

static uint32_t Psx16ToArgb(uint16_t c)
{
    int r, g, b;
    uint32_t a;
    if (c == 0)
        return 0;                    /* PSX: 0x0000 = fully transparent */
    /* STP bit (bit 15): in a semi-transparent prim, STP=1 texels blend and
     * STP=0 texels stay OPAQUE. Encode as alpha 0x80 vs 0xFF — with ABR0's
     * SRC_ALPHA blending that is PSX-exact per texel; opaque prims ignore
     * alpha (blend off) and the alpha test only kills true 0x0000 texels. */
    a = (c & 0x8000) ? 0x80000000u : 0xFF000000u;
    r = (c & 0x1F) << 3;
    g = ((c >> 5) & 0x1F) << 3;
    b = ((c >> 10) & 0x1F) << 3;
    return a | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Expand an indexed page to 8-bit indices in NV2A swizzle (Morton) order. */
static void PageDecodeIndices(int tpage, uint8_t* out)
{
    const int tx = (tpage & 0x0F) * 64;
    const int ty = ((tpage >> 4) & 1) * 256;
    const int tp = (tpage >> 7) & 3;          /* 0 = 4bit, 1 = 8bit */
    int u, v;

    for (v = 0; v < TEX_DIM; v++) {
        const uint16_t* row = &s_vram[((ty + v) & (VRAM_H - 1)) * VRAM_W];
        unsigned        sy  = s_swzY[v];
        int             i   = 0;

        if (tp == 0) {                        /* one VRAM word feeds 4 texels */
            for (u = 0; u < TEX_DIM; u += 4, i++) {
                uint16_t w = row[(tx + i) & (VRAM_W - 1)];
                s_swzScratch[s_swzX[u]     | sy] = (uint8_t)( w        & 0x0F);
                s_swzScratch[s_swzX[u + 1] | sy] = (uint8_t)((w >>  4) & 0x0F);
                s_swzScratch[s_swzX[u + 2] | sy] = (uint8_t)((w >>  8) & 0x0F);
                s_swzScratch[s_swzX[u + 3] | sy] = (uint8_t)((w >> 12) & 0x0F);
            }
        } else {                              /* one VRAM word feeds 2 texels */
            for (u = 0; u < TEX_DIM; u += 2, i++) {
                uint16_t w = row[(tx + i) & (VRAM_W - 1)];
                s_swzScratch[s_swzX[u]     | sy] = (uint8_t)( w       & 0xFF);
                s_swzScratch[s_swzX[u + 1] | sy] = (uint8_t)((w >> 8) & 0xFF);
            }
        }
    }
    memcpy(out, s_swzScratch, PAGE_BYTES);    /* sequential: write-combines cleanly */
    SH_STORE_BARRIER();
}

/* Same scatter, but sourced from a RESIDENT pool slab instead of s_vram (see
 * xbox_respool.c). The slab holds the TIM's word block verbatim, so the only
 * differences from the s_vram path are the base pointer, the row pitch and the
 * fact that a slab can be narrower or shorter than a full page — the rows and
 * columns past its extent are left at index 0 rather than wrapping into a
 * neighbour, since there IS no neighbour. */
static void PageDecodeIndicesResident(const uint16_t* src, const s_XbResidentDesc* d,
                                      uint8_t* out)
{
    int u, v;

    memset(s_swzScratch, 0, PAGE_BYTES);
    for (v = 0; v < TEX_DIM && v < d->h; v++) {
        const uint16_t* row = src + (size_t)v * d->pitchWords;
        unsigned        sy  = s_swzY[v];
        int             i   = 0;

        if (d->bpp == 4) {
            for (u = 0; u < TEX_DIM && i < d->pitchWords; u += 4, i++) {
                uint16_t w = row[i];
                s_swzScratch[s_swzX[u]     | sy] = (uint8_t)( w        & 0x0F);
                s_swzScratch[s_swzX[u + 1] | sy] = (uint8_t)((w >>  4) & 0x0F);
                s_swzScratch[s_swzX[u + 2] | sy] = (uint8_t)((w >>  8) & 0x0F);
                s_swzScratch[s_swzX[u + 3] | sy] = (uint8_t)((w >> 12) & 0x0F);
            }
        } else {
            for (u = 0; u < TEX_DIM && i < d->pitchWords; u += 2, i++) {
                uint16_t w = row[i];
                s_swzScratch[s_swzX[u]     | sy] = (uint8_t)( w       & 0xFF);
                s_swzScratch[s_swzX[u + 1] | sy] = (uint8_t)((w >> 8) & 0xFF);
            }
        }
    }
    memcpy(out, s_swzScratch, PAGE_BYTES);
    SH_STORE_BARRIER();
}

/* Palette row `row` of a resident slab. Rows past what the TIM shipped fall back
 * to row 0, matching the PC pool's behaviour for a missing row. */
static void PaletteBuildResident(const s_XbResidentDesc* d, int row, uint32_t* out)
{
    const uint16_t* src;
    int i;

    if (!d->clut || d->rows <= 0) {
        for (i = 0; i < PAL_ENTRIES; i++) out[i] = 0;
        SH_STORE_BARRIER();
        return;
    }
    if (row < 0 || row >= d->rows) row = 0;
    src = d->clut + (size_t)row * 256;

    for (i = 0; i < PAL_ENTRIES; i++)
        out[i] = Psx16ToArgb(src[i]);
    SH_STORE_BARRIER();
}

/* Build a 256-entry GPU palette from the CLUT at `clut`. A 4-bit CLUT only
 * occupies 16 VRAM words; the rest are never indexed, but must not read past the
 * row. Same Psx16ToArgb conversion the ARGB path uses, so colour and all three
 * alpha levels are bit-identical. */
static void PaletteBuild(int clut, uint32_t* out)
{
    const int cx  = (clut & 0x3F) * 16;
    const int cy  = (clut >> 6) & 0x1FF;
    const uint16_t* row = &s_vram[(cy & (VRAM_H - 1)) * VRAM_W];
    int i;

    for (i = 0; i < PAL_ENTRIES; i++) {
        int sx = cx + i;
        out[i] = (sx < VRAM_W) ? Psx16ToArgb(row[sx]) : 0;
    }
    SH_STORE_BARRIER();
}

static void DecodePage(int tpage, int clut, uint32_t* out)
{
    const int tx = (tpage & 0x0F) * 64;          /* VRAM x (16-bit words) */
    const int ty = ((tpage >> 4) & 1) * 256;     /* VRAM y */
    const int tp = (tpage >> 7) & 3;             /* 0=4bit 1=8bit 2,3=16bit */
    const int cx = (clut & 0x3F) * 16;           /* CLUT VRAM x */
    const int cy = (clut >> 6) & 0x1FF;          /* CLUT VRAM y */
    const uint16_t* clutRow = &s_vram[(cy & (VRAM_H - 1)) * VRAM_W + cx];
    int u, v;
    int zeroCount = 0, stp1Count = 0;

    /* Log 043 measured 7680 page decodes in one short session, densely clustered
     * in the cafe fight -- 65536 texels each. The old loop ran the tp test AND a
     * full BGR555->ARGB conversion PER TEXEL (16.7M conversions there). Indexed
     * pages only ever reference 16 or 256 distinct colours, so convert the CLUT
     * once and make the inner loop a table lookup, with the format branch hoisted
     * out of the loops entirely. Byte-identical output, a fraction of the work.
     * The transparent/STP census is diagnostic and only consumed for the first
     * 12 decodes, so it is gated instead of running per texel forever. */
    {
        const int census = (s_decodeTotal > 60 && s_stpLogged < 12);
        uint32_t  pal[256];
        int       palN = (tp == 0) ? 16 : (tp == 1) ? 256 : 0;

        for (u = 0; u < palN; u++) {
            uint16_t c = clutRow[u];
            pal[u] = Psx16ToArgb(c);
            if (census) {
                if (c == 0)          zeroCount++;
                else if (c & 0x8000) stp1Count++;
            }
        }

        for (v = 0; v < TEX_DIM; v++) {
            const uint16_t* row = &s_vram[((ty + v) & (VRAM_H - 1)) * VRAM_W];
            uint32_t*       o   = &out[v * TEX_DIM];

            /* The word index still wraps (an 8-bit page is 128 words wide and a
             * high tpage x would walk past the row end), but now once per word
             * instead of once per texel. */
            if (tp == 0) {                        /* 4-bit: one word feeds 4 texels */
                int i = 0;
                for (u = 0; u < TEX_DIM; u += 4, i++) {
                    uint16_t w = row[(tx + i) & (VRAM_W - 1)];
                    o[u    ] = pal[w & 0x0F];
                    o[u + 1] = pal[(w >> 4) & 0x0F];
                    o[u + 2] = pal[(w >> 8) & 0x0F];
                    o[u + 3] = pal[(w >> 12) & 0x0F];
                }
            } else if (tp == 1) {                 /* 8-bit: one word feeds 2 texels */
                int i = 0;
                for (u = 0; u < TEX_DIM; u += 2, i++) {
                    uint16_t w = row[(tx + i) & (VRAM_W - 1)];
                    o[u    ] = pal[w & 0xFF];
                    o[u + 1] = pal[(w >> 8) & 0xFF];
                }
            } else {                              /* 16-bit direct: no palette */
                for (u = 0; u < TEX_DIM; u++) {
                    uint16_t texel = row[(tx + u) & (VRAM_W - 1)];
                    if (census) {
                        if (texel == 0)          zeroCount++;
                        else if (texel & 0x8000) stp1Count++;
                    }
                    o[u] = Psx16ToArgb(texel);
                }
            }
        }
    }
    SH_STORE_BARRIER();  /* flush write-combined texels */

    /* Probe: transparent-texel + STP census — proves the alpha-test fix is
     * load-bearing (zero>0 = pages that painted black cut-out boxes) and that
     * per-texel semi-transparency is in use (stp1>0). First 12 pages decoded
     * past the boot/menu churn only. */
    {
        if (s_decodeTotal > 60 && s_stpLogged < 12) {
            s_stpLogged++;
            SH_DBG("[STP] tpage=0x%x clut=0x%x zero=%d stp1=%d", tpage, clut, zeroCount, stp1Count);
        }
    }
}

/* Decode (or fetch cached) the texture page for (tpage,clut). Returns a 256x256
 * A8R8G8B8 buffer, or NULL if the texture allocator failed. */
uint32_t* PsxVram_GetTexture(int tpage, int clut)
{
    int key = ((tpage & 0xFFFF) << 16) | (clut & 0xFFFF);
    int i;
    int missWasInvalidated = 0, missTookLiveSlot = 0;

    if (!s_cacheReady) {
        /* The cache fills on the FIRST textured prim (the boot logo) — long before
         * the map heap is claimed — and MmAllocateContiguousMemoryEx draws from the
         * same pool as malloc. Filling it greedily therefore starves the map: at
         * 720p it took 86/96 slots and left the 22 IPD chunk buffers (~2MB) with
         * nothing, so every chunk got ipdHdr=NULL and the loader spun forever.
         * Stop short of HEAP_RESERVE_KB so the map/cutscene allocations always fit;
         * a smaller cache only costs decode thrash, never a boot failure. */
        extern unsigned Xbox_MemFreeKB(void);   /* sh_log_xbox.c */
        /* CACHE_MIN_SLOTS is a floor the reserve may NOT undercut. A 16MB packet
         * arena regression once left only 4.4MB free here, the reserve refused
         * every slot, and the game rendered ENTIRELY UNTEXTURED (white screens) —
         * a far worse failure than being tight on heap. Below the floor we
         * allocate regardless and let the map allocations degrade instead. */
        enum { HEAP_RESERVE_KB = 8 * 1024, SLOT_KB = (TEX_DIM * TEX_DIM * 4) / 1024,
               CACHE_MIN_SLOTS = 24 };
        int ok = 0;
        for (i = 0; i < CACHE_N; i++) {
            unsigned freeKB = Xbox_MemFreeKB();
            s_cache[i].key  = -1;
            s_cache[i].argb = NULL;
            if (ok >= CACHE_MIN_SLOTS && freeKB && freeKB < HEAP_RESERVE_KB + SLOT_KB)
                continue;                       /* reserve floor reached — leave the rest NULL */
            s_cache[i].argb = (uint32_t*)GpuNv2a_AllocTexMem(TEX_DIM * TEX_DIM * 4);
            if (s_cache[i].argb) ok++;
        }
        s_cacheReady = 1;
        s_cacheAlloc = ok;
        SH_DBG("[VRAM] texture cache: %d/%d slots (256KB each), free=%uKB after",
               ok, CACHE_N, Xbox_MemFreeKB());
        if (ok < CACHE_N)
            SH_DBG("[VRAM] cache capped by %dMB heap reserve (expect decode thrash)",
                   HEAP_RESERVE_KB / 1024);
    }

    /* One-entry memo. This is called PER PRIMITIVE and used to linear-scan up to
     * 96 slots every time; the cafe fight submits several hundred prims a frame.
     * Adjacent prims almost always share (tpage,clut) -- that is exactly why the
     * draw batcher can merge them -- so the previous slot hits most of the time.
     * Validated against the live key before use, so an evicted or re-keyed slot
     * falls through to the full scan. */
    if (s_memoSlot >= 0 && s_cache[s_memoSlot].key == key && s_cache[s_memoSlot].argb) {
        s_cache[s_memoSlot].lastUse = (unsigned)g_Nv2aFrameCount;
        s_cache[s_memoSlot].seq     = ++s_bindSeq;
        return s_cache[s_memoSlot].argb;
    }

    for (i = 0; i < CACHE_N; i++)
        if (s_cache[i].key == key && s_cache[i].argb) {
            s_cache[i].lastUse = (unsigned)g_Nv2aFrameCount;
            s_cache[i].seq     = ++s_bindSeq;
            if (s_cache[i].hits < 0xFFFF) s_cache[i].hits++;
            s_memoSlot         = i;
            return s_cache[i].argb;
        }

    /* Classify the miss BEFORE any slot is overwritten. A dead slot still
     * carrying this key in lastKey means the page WAS cached and something wrote
     * its source VRAM (animated CLUT) -- capacity cannot fix that. */
    missWasInvalidated = 0;
    for (i = 0; i < CACHE_N; i++)
        if (s_cache[i].key == -1 && s_cache[i].lastKey == key && s_cache[i].argb) {
            missWasInvalidated = 1;
            break;
        }

    /* Miss: fill a free slot, else evict the least-recently-used. (The old
     * round-robin evicted HOT entries under pressure — with a working set just
     * over capacity that meant re-decoding almost every page every frame.)
     *
     * PIN THIS FRAME'S ENTRIES. GpuNv2a_BindTexture writes the slot's raw pointer
     * into the NV2A pushbuffer as a bare TX_OFFSET and the draw is asynchronous —
     * nothing waits for the GPU to consume it. Evicting a slot that was bound
     * earlier in THIS frame means the CPU's DecodePage overwrites 256KB the GPU
     * has not rasterized yet, so already-submitted geometry samples half-rewritten
     * texels. lastUse used to gate only which victim we PREFER; it must also gate
     * which victims are LEGAL.
     *
     * If EVERY slot is pinned (working set > 96 distinct pages in one frame) we
     * deliberately fall back to evicting the LRU anyway, accepting the race. The
     * alternative — returning 0 — drops the primitive, and dropped geometry is
     * precisely the "world vanishes" class of bug being chased elsewhere; a rare
     * one-frame texture glitch is the lesser failure. The counter says which
     * happened. */
    {
        int      best = -1, bestPinned = -1;
        unsigned bestUse = 0xFFFFFFFFu, bestPinnedUse = 0xFFFFFFFFu;
        unsigned bestHits = 0xFFFFFFFFu;
        unsigned thisFrame = (unsigned)g_Nv2aFrameCount;

        /* Age every count periodically. Without decay an early-scene page keeps a
         * high score forever and becomes unevictable long after the room changed;
         * halving lets the ranking follow the CURRENT scene. */
        if (thisFrame != s_ageFrame && (thisFrame & 255) == 0) {
            int k;
            s_ageFrame = thisFrame;
            for (k = 0; k < CACHE_N; k++)
                s_cache[k].hits >>= 1;
        }

        /* GROW ON DEMAND before considering eviction. The cache is sized ONCE,
         * on the first textured prim (the boot logo) — long before any map is
         * loaded — and it must stop early then, or it starves the map loader
         * (the documented white-screen/hang failures above). But it never grew
         * afterwards, so a scene whose working set exceeds that boot-time size
         * thrashes forever at a capacity chosen when the heap looked its
         * tightest: the church cutscene forced ~6400 evictions in ~5 s with the
         * cache stuck at 43 of 96 slots, which re-decodes pages every frame —
         * that is the frame-rate collapse there, and re-decoding the FONT page
         * mid-scene is what garbles the subtitles.
         *
         * Growing HERE is the safe moment: the map's allocations have already
         * happened, so taking a slot cannot starve them, and the same
         * HEAP_RESERVE_KB floor still guards whatever allocates next. Only ever
         * grows into slots the boot pass left NULL, never past CACHE_N. */
        for (i = 0; i < CACHE_N; i++) {
            if (s_cache[i].argb)
                continue;                       /* already allocated */
            {
                extern unsigned Xbox_MemFreeKB(void);
                /* 4MB, not the boot pass's 8MB. Growth happens AFTER the map's
                 * allocations have landed, so it does not need the boot-time
                 * margin — and at 8MB it never fired at all: the measured free
                 * heap in the town was 8444KB against a 8192+256 threshold, so
                 * the cache sat at 43 slots and drained the GPU instead. Each
                 * slot bought here removes stalls AND decode thrash. */
                enum { GROW_RESERVE_KB = 4 * 1024, SLOT_KB = (TEX_DIM * TEX_DIM * 4) / 1024 };
                unsigned freeKB = Xbox_MemFreeKB();
                if (!freeKB || freeKB < GROW_RESERVE_KB + SLOT_KB)
                    break;                      /* keep the reserve intact */
                s_cache[i].argb = (uint32_t*)GpuNv2a_AllocTexMem(TEX_DIM * TEX_DIM * 4);
                if (!s_cache[i].argb)
                    break;                      /* allocator said no — stop asking */
                s_cache[i].key     = -1;
                s_cache[i].lastUse = 0;
                s_cacheAlloc++;
                {   /* rare: only fires while a scene is still growing its set */
                    static unsigned s_grown;
                    if ((++s_grown & 15) == 1)
                        SH_DBG("[VRAM] cache grew to %d slots (free=%uKB)",
                               s_cacheAlloc, Xbox_MemFreeKB());
                }
            }
            break;                              /* one slot per miss — gentle ramp */
        }

        for (i = 0; i < CACHE_N; i++) {
            if (!s_cache[i].argb) continue;
            if (s_cache[i].key == -1) { best = i; break; }
            if (s_cache[i].lastUse < bestPinnedUse) { bestPinnedUse = s_cache[i].lastUse; bestPinned = i; }
            /* In flight = bound this frame AND not yet consumed by a drain. Once a
             * drain has happened, everything bound before it has been rasterized,
             * so those slots are legal again WITHOUT losing their LRU age. */
            if (s_cache[i].lastUse == thisFrame && s_cache[i].seq > s_drainedSeq)
                continue;                                    /* in flight — not evictable */
            /* Pick the COLDEST page, not merely the oldest. Log 048's miss
             * classifier reads evict=78501 inval=150: the working set genuinely
             * exceeds the 42 slots 720p leaves us, and under that pressure plain
             * LRU is the worst possible policy -- it keeps throwing out the pages
             * drawn EVERY frame (font, UI, the room's own walls) because they are
             * touched early in the frame and so look "old" by the end of it. Rank
             * by aged use-count first, oldest as tie-break, so pages that keep
             * proving useful survive and one-shot pages absorb the churn. */
            if (s_cache[i].hits < bestHits ||
                (s_cache[i].hits == bestHits && s_cache[i].lastUse < bestUse)) {
                bestHits = s_cache[i].hits;
                bestUse  = s_cache[i].lastUse;
                best     = i;
            }
        }
        /* Out of evictable slots: every allocated slot was bound earlier in THIS
         * frame. Stealing one anyway rewrites texels the GPU has not sampled yet,
         * so the already-submitted prims pointing at that slot draw a DIFFERENT
         * page — that is the garbled in-game text (all four font pages are
         * fetched every frame, so a stolen font slot renders as another glyph
         * row) and it got worse exactly where texture pressure is highest: the
         * church cutscene. Drain the GPU instead. Once it is idle every slot has
         * been consumed and is legal to reuse, so unpin them all and take the
         * LRU. Costs a stall, but only when the working set genuinely exceeds
         * capacity, and it is self-limiting: after a drain the whole cache is
         * evictable again. */
        if (best < 0) {
            static unsigned s_drains;
            static unsigned s_drainFrame = 0xFFFFFFFFu;
            static int      s_drainThisFrame, s_drainWorst, s_drainMsTotal;
            extern void GpuNv2a_DrainGpu(void);
            extern int  GpuNv2a_Ms(void);
            int         td = GpuNv2a_Ms();
            GpuNv2a_DrainGpu();
            s_drainMsTotal += GpuNv2a_Ms() - td;   /* what the stalls actually cost */
            /* Mark everything bound so far as consumed. This used to zero every
             * slot's lastUse, which unpinned them but ALSO destroyed the LRU
             * ages -- afterwards every slot looked equally old, so the next
             * evictions picked near-arbitrary victims, threw out hot pages, and
             * caused more misses, which caused more drains. That feedback loop is
             * why the drains arrived in dense bursts (all 2049 of log 042 fell in
             * one stretch: the cafe fight) instead of tapering off. Recording the
             * bind counter unpins without touching the ages. */
            s_drainedSeq = s_bindSeq;
            best = bestPinned;
            /* Each drain is a full GPU stall, so track the per-frame worst case:
             * that is the number to watch if a scene feels slow after this. */
            if (thisFrame != s_drainFrame) { s_drainFrame = thisFrame; s_drainThisFrame = 0; }
            if (++s_drainThisFrame > s_drainWorst) s_drainWorst = s_drainThisFrame;
            if ((++s_drains & 63) == 1)
                SH_DBG("[VRAM] out of slots -> GPU drain #%u (alloc=%d worstPerFrame=%d totalMs=%d)",
                       s_drains, s_cacheAlloc, s_drainWorst, s_drainMsTotal);
        }
        if (best < 0)
            return 0;                      /* cache never allocated */
        i = best;
    }
    missTookLiveSlot = (s_cache[i].key != -1);     /* threw out a live page => capacity */
    TexEntry_SetBBox(&s_cache[i], tpage, clut);   /* record VRAM footprint for selective invalidation */
    /* Framebuffer feedback: a 16-bit direct page overlapping the framebuffer
     * rows is the pause/save background, crossfade or window-crash distortion
     * sampling the rendered frame (getTPage(2, ...) in the shared game code).
     * Pull the last completed frame into s_vram FIRST so this decode sees
     * rendered output, not stale uploads. 16-bit only: the framebuffer is
     * 16bpp, and regular 4/8-bit world textures near the pages must not
     * trigger the (expensive) readback. Self-limits: the readback rewrites
     * these rows via PsxVram_Load, whose memcmp only invalidates on change —
     * a static screen settles into cache hits with zero further readbacks. */
    if (((tpage >> 7) & 3) >= 2 &&
        GpuXbox_FbRegionOverlap(s_cache[i].px0, s_cache[i].py0, s_cache[i].px1, s_cache[i].py1)) {
        extern void GpuXbox_FbReadbackForTexture(int px0, int py0, int px1, int py1);
        GpuXbox_FbReadbackForTexture(s_cache[i].px0, s_cache[i].py0,
                                     s_cache[i].px1, s_cache[i].py1);
    }
    {   /* charge the decode to this frame so [FRAME] can separate CPU decode
         * cost from submission cost from GPU wait -- the cafe question. */
        extern int GpuNv2a_Ms(void);
        int t0 = GpuNv2a_Ms();
        DecodePage(tpage, clut, s_cache[i].argb);
        g_PsxDecodeMs += (unsigned)(GpuNv2a_Ms() - t0);
        g_PsxDecodeCount++;
    }
    s_cache[i].key     = key;
    s_cache[i].lastUse = (unsigned)g_Nv2aFrameCount;
    s_cache[i].seq     = ++s_bindSeq;
    /* Seed at 1, not 0: a page we just paid 256KB to decode must not be the very
     * next victim, or every miss evicts the previous miss and the cache degrades
     * into a one-slot ring. It earns its keep from here by being hit again. */
    s_cache[i].hits    = 1;
    s_memoSlot         = i;

    /* WHY this miss happened. The cafe fight burns 20ms/frame decoding (36
     * decodes/frame) and the two causes need OPPOSITE fixes:
     *   evict = the working set exceeds the 42 slots we can afford at 720p
     *           -> capacity (paletted pages: 64KB/tpage instead of 256KB per
     *              tpage+clut) or fewer GPU surfaces.
     *   inval = the source VRAM changed under a cached page (animated CLUTs)
     *           -> capacity does NOTHING; only CLUT/page separation helps.
     * Guessing between them has already cost two wrong fixes this session. */
    {
        static unsigned s_missEvict, s_missInval, s_missCold;
        if (missWasInvalidated)   s_missInval++;
        else if (missTookLiveSlot) s_missEvict++;
        else                       s_missCold++;
        if ((s_decodeTotal & 511) == 1)
            SH_DBG("[VRAM] miss cause: evict=%u inval=%u cold=%u (alloc=%d)",
                   s_missEvict, s_missInval, s_missCold, s_cacheAlloc);
    }
    /* A miss = a 256x256 decode. After the cache fills this should go quiet; if it
     * keeps climbing the working set exceeds CACHE_N (thrashing -> slow). */
    if ((++s_decodeTotal & 511) == 0)
        SH_DBG("[VRAM] decode #%d", s_decodeTotal);
    return s_cache[i].argb;
}

/* ---- Paletted lookup -------------------------------------------------------
 * Returns the swizzled index page for `tpage` and, via palOut, the palette for
 * `clut`. Returns 0 for 16-bit direct pages (not palettizable -- the caller
 * falls back to PsxVram_GetTexture) or if the allocation failed, so a failure
 * degrades to the old path rather than to a black screen.
 *
 * Both caches carry the same in-flight rule as the ARGB cache: the NV2A is
 * handed a raw pointer and reads it asynchronously, so an entry bound this frame
 * and not yet consumed by a drain must never be overwritten -- that is exactly
 * the bug 09202d93e fixed, and it applies to index pages and palettes too. */
/* Drop every decoded page and palette belonging to a resident slot.
 *
 * REQUIRED whenever a slab's CONTENT changes, which the engine does constantly:
 * it recycles pool slot ids as chunks stream, re-registering the same id with a
 * different TIM ("replacing in place on engine slot reuse", hires_override.h).
 * The decoded page is keyed by SLOT, so without this it keeps serving the
 * PREVIOUS occupant's texels forever -- the room you walk into gets the room you
 * left, drawn on geometry whose UVs were authored for something else, which
 * reads as garbled/tiled/misaligned. Real VRAM pages never needed this because
 * their key is the page address and the write itself invalidates by rect; a
 * private slab has no rect to overlap, which is exactly why it was missed.
 *
 * Marked dead (-2) rather than free (-1) deliberately: -1 is the victim scan's
 * fast path and bypasses the in-flight check, which would hand the GPU's
 * still-referenced page straight to the next decode. -2 never matches a lookup,
 * and zeroing hits makes it the preferred victim through the NORMAL path, where
 * the lastUse/seq in-flight guard still applies. */
void PsxVram_InvalidateResidentSlot(int slot)
{
    const int pageKey = 0x40000000 | slot;
    const int palBase = 0x20000000 | (slot << 5);
    int i;

    if (!s_palPathReady)
        return;
    for (i = 0; i < PAGE_N; i++)
        if (s_pages[i].key == pageKey) { s_pages[i].key = -2; s_pages[i].hits = 0; }
    for (i = 0; i < PAL_N; i++)
        if ((s_pals[i].key & ~0x1F) == palBase && s_pals[i].key != -1)
            s_pals[i].key = -2;
}

const void* PsxVram_GetPaletted(int tpage, int clut, const void** palOut)
{
    int       tp = (tpage >> 7) & 3;
    unsigned  thisFrame = (unsigned)g_Nv2aFrameCount;
    int       i, best;
    /* Resident (virtual) chunk-pool slot: the clut word carries a slot id and a
     * palette row instead of VRAM coordinates, and the pixels live in a private
     * slab rather than s_vram. Everything below — the page LRU, the palette LRU,
     * the eviction and drain logic — is shared with real pages; only the decode
     * source and the cache KEYS differ. The keys must be disjoint from real
     * tpage/clut values (a virtual tpage aliases a real page, and two slots can
     * share one), hence the high-bit tags. */
    const uint16_t*  resWords = 0;
    s_XbResidentDesc resDesc;
    int              pageKey = tpage, palKey = clut;

    if (palOut) *palOut = 0;
    {
        int vrow = (clut >> 6) & 0x3FF;
        if (vrow >= HIRES_POOL_CLUT_ROW_BASE) {
            int slot = ((vrow - HIRES_POOL_CLUT_ROW_BASE) / HIRES_POOL_MAX_ROWS) * 64
                     + (clut & 0x3F);
            int row  = (vrow - HIRES_POOL_CLUT_ROW_BASE) % HIRES_POOL_MAX_ROWS;

            resWords = Xbox_ResidentPoolGet(slot, &resDesc);
            if (!resWords)
                return 0;                      /* not resident: RGBA table / skip */
            tp      = (resDesc.bpp == 4) ? 0 : 1;
            pageKey = 0x40000000 | slot;
            palKey  = 0x20000000 | (slot << 5) | row;
        }
    }
    if (tp >= 2)
        return 0;                              /* 16-bit direct: ARGB path */
    /* Escape hatch: texture_paletted=0 in silenthill.cfg falls back to the ARGB
     * cache without a rebuild, in case this path misbehaves on hardware (it
     * programs a texture format and a palette register that cannot be verified
     * off-console). */
    if (!g_PcConfig.xboxPalettedTex)
        return 0;


    if (!s_palPathReady) {
        extern unsigned Xbox_MemFreeKB(void);
        int b, ok = 0, okp = 0;
        for (i = 0; i < TEX_DIM; i++) {        /* Morton spread tables */
            unsigned x = 0, y = 0;
            for (b = 0; b < 8; b++) {
                x |= (unsigned)((i >> b) & 1) << (2 * b);
                y |= (unsigned)((i >> b) & 1) << (2 * b + 1);
            }
            s_swzX[i] = x;
            s_swzY[i] = y;
        }
        for (i = 0; i < PAGE_N; i++) {
            s_pages[i].key  = -1;
            s_pages[i].data = (uint8_t*)GpuNv2a_AllocTexMem(PAGE_BYTES);
            if (s_pages[i].data) ok++;
        }
        for (i = 0; i < PAL_N; i++) {
            s_pals[i].key  = -1;
            s_pals[i].data = (uint32_t*)GpuNv2a_AllocTexMem(PAL_ENTRIES * 4);
            if (s_pals[i].data) okp++;
        }
        s_palPathReady = 1;
        {   /* which palette DMA variant to program (config-selected) */
            extern void GpuNv2a_SetPaletteDmaVariant(int variant);
            GpuNv2a_SetPaletteDmaVariant(g_PcConfig.xboxPalettedTex);
        }
        SH_DBG("[VRAM] paletted cache: %d/%d pages (64KB) + %d/%d palettes, free=%uKB",
               ok, PAGE_N, okp, PAL_N, Xbox_MemFreeKB());
    }

    /* --- index page, keyed by tpage alone (the CLUT no longer matters) --- */
    best = -1;
    for (i = 0; i < PAGE_N; i++)
        if (s_pages[i].key == pageKey && s_pages[i].data) { best = i; break; }

    if (best < 0) {
        int      victim = -1, pinned = -1;
        unsigned bh = 0xFFFFFFFFu, bu = 0xFFFFFFFFu, pu = 0xFFFFFFFFu;
        for (i = 0; i < PAGE_N; i++) {
            if (!s_pages[i].data) continue;
            if (s_pages[i].key == -1) { victim = i; break; }
            if (s_pages[i].lastUse < pu) { pu = s_pages[i].lastUse; pinned = i; }
            if (s_pages[i].lastUse == thisFrame && s_pages[i].seq > s_drainedSeq)
                continue;                      /* GPU may still be reading it */
            if (s_pages[i].hits < bh || (s_pages[i].hits == bh && s_pages[i].lastUse < bu)) {
                bh = s_pages[i].hits; bu = s_pages[i].lastUse; victim = i;
            }
        }
        if (victim < 0) {                      /* all in flight: drain, then reuse */
            extern void GpuNv2a_DrainGpu(void);
            GpuNv2a_DrainGpu();
            s_drainedSeq = s_bindSeq;
            victim = pinned;
        }
        if (victim < 0) return 0;
        if (resWords) {
            /* A resident slab is private: no VRAM write can reach it, so the
             * invalidation rect is deliberately empty. That is the whole point
             * of the mode — this page cannot be stolen or overwritten. */
            s_pages[victim].px0 = s_pages[victim].px1 = 0;
            s_pages[victim].py0 = s_pages[victim].py1 = 0;
            PageDecodeIndicesResident(resWords, &resDesc, s_pages[victim].data);
        } else {
            {   /* record the source rect so texel writes invalidate this page */
                int px = (tpage & 0x0F) * 64;
                int py = ((tpage >> 4) & 1) * 256;
                s_pages[victim].px0 = px;
                s_pages[victim].py0 = py;
                s_pages[victim].px1 = px + ((tp == 0) ? 64 : 128);
                s_pages[victim].py1 = py + 256;
            }
            PageDecodeIndices(tpage, s_pages[victim].data);
        }
        s_pages[victim].key  = pageKey;
        s_pages[victim].hits = 1;
        best = victim;
    }
    s_pages[best].lastUse = thisFrame;
    s_pages[best].seq     = ++s_bindSeq;
    if (s_pages[best].hits < 0xFFFF) s_pages[best].hits++;

    /* --- palette, keyed by clut alone --- */
    {
        int p = -1;
        for (i = 0; i < PAL_N; i++)
            if (s_pals[i].key == palKey && s_pals[i].data) { p = i; break; }
        if (p < 0) {
            int      victim = -1, pinned = -1;
            unsigned bu = 0xFFFFFFFFu, pu = 0xFFFFFFFFu;
            for (i = 0; i < PAL_N; i++) {
                if (!s_pals[i].data) continue;
                if (s_pals[i].key == -1) { victim = i; break; }
                if (s_pals[i].lastUse < pu) { pu = s_pals[i].lastUse; pinned = i; }
                if (s_pals[i].lastUse == thisFrame && s_pals[i].seq > s_drainedSeq)
                    continue;
                if (s_pals[i].lastUse < bu) { bu = s_pals[i].lastUse; victim = i; }
            }
            if (victim < 0) {
                extern void GpuNv2a_DrainGpu(void);
                GpuNv2a_DrainGpu();
                s_drainedSeq = s_bindSeq;
                victim = pinned;
            }
            if (victim < 0) return 0;
            if (resWords) {
                s_pals[victim].cx0 = s_pals[victim].cx1 = 0;   /* private: never invalidated */
                s_pals[victim].cy0 = s_pals[victim].cy1 = 0;
                PaletteBuildResident(&resDesc, palKey & 0x1F, s_pals[victim].data);
            } else {
                {
                    int cx = (clut & 0x3F) * 16;
                    int cy = (clut >> 6) & 0x1FF;
                    s_pals[victim].cx0 = cx;
                    s_pals[victim].cy0 = cy;
                    s_pals[victim].cx1 = cx + ((tp == 0) ? 16 : 256);
                    s_pals[victim].cy1 = cy + 1;
                }
                PaletteBuild(clut, s_pals[victim].data);
            }
            s_pals[victim].key = palKey;
            p = victim;
        }
        s_pals[p].lastUse = thisFrame;
        s_pals[p].seq     = ++s_bindSeq;
        if (palOut) *palOut = s_pals[p].data;
    }
    return s_pages[best].data;
}
