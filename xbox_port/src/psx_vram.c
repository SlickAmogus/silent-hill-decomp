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
    uint32_t* argb;
    unsigned  lastUse;        /* frame of last GetTexture hit (LRU eviction) */
    /* Source-VRAM footprint of this decoded page, in 16-bit words, [x0,x1) x
     * [y0,y1). Page rect and CLUT rect are tracked separately so a CLUT-only
     * write still invalidates an indexed page that samples it. */
    int px0, py0, px1, py1;   /* texture-page rect */
    int cx0, cy0, cx1, cy1;   /* CLUT rect (empty for 16-bit direct) */
} TexEntry;
static TexEntry s_cache[CACHE_N];
static int      s_cacheReady;
static int      s_decodeTotal;

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
            RectsOverlap(e->cx0, e->cy0, e->cx1, e->cy1, wx0, wy0, wx1, wy1))
            e->key = -1;
    }
    /* Do NOT reset s_cacheNext: round-robin reuse still finds key==-1 slots. */
}

/* LoadImage: copy w*h 16-bit source pixels into VRAM at (x,y), row by row. */
void PsxVram_Load(int x, int y, int w, int h, const uint16_t* src)
{
    int row, yend, changed = 0;

    if (!src || w <= 0 || h <= 0 || x < 0 || y < 0 || x >= VRAM_W || y >= VRAM_H)
        return;

    if (x + w > VRAM_W) w = VRAM_W - x;
    yend = y;
    for (row = 0; row < h; row++) {
        int       vy = y + row;
        uint16_t* d;
        if (vy >= VRAM_H) break;
        d = &s_vram[vy * VRAM_W + x];
        /* Only rows that actually CHANGE dirty the cache. The game re-uploads
         * identical CLUTs constantly (per-frame character transparency toggles,
         * repeated palette sets in cutscenes); invalidating on those forced
         * full page re-decodes every frame — a major thrash source. */
        if (!changed && memcmp(d, &src[row * w], (size_t)w * 2) != 0)
            changed = 1;
        memcpy(d, &src[row * w], (size_t)w * 2);
        yend = vy + 1;
    }
    if (changed)
        InvalidateRegion(x, y, x + w, yend);   /* selective, not nuke-all */
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
    int row, col, changed = 0;

    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x >= VRAM_W || y >= VRAM_H)
        return;
    if (x + w > VRAM_W) w = VRAM_W - x;
    for (row = 0; row < h; row++) {
        int vy = y + row;
        uint16_t* d;
        if (vy >= VRAM_H) break;
        d = &s_vram[vy * VRAM_W + x];
        for (col = 0; col < w; col++) {
            if (d[col] != c) { d[col] = c; changed = 1; }
        }
    }
    if (changed)
        InvalidateRegion(x, y, x + w, y + h);
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

    for (v = 0; v < TEX_DIM; v++) {
        const uint16_t* row = &s_vram[((ty + v) & (VRAM_H - 1)) * VRAM_W];
        uint32_t*       o   = &out[v * TEX_DIM];
        for (u = 0; u < TEX_DIM; u++) {
            uint16_t texel;
            if (tp == 0) {                        /* 4-bit indexed */
                uint16_t word = row[(tx + (u >> 2)) & (VRAM_W - 1)];
                texel = clutRow[(word >> ((u & 3) * 4)) & 0x0F];
            } else if (tp == 1) {                 /* 8-bit indexed */
                uint16_t word = row[(tx + (u >> 1)) & (VRAM_W - 1)];
                texel = clutRow[(word >> ((u & 1) * 8)) & 0xFF];
            } else {                              /* 16-bit direct */
                texel = row[(tx + u) & (VRAM_W - 1)];
            }
            if (texel == 0)          zeroCount++;
            else if (texel & 0x8000) stp1Count++;
            o[u] = Psx16ToArgb(texel);
        }
    }
    __asm__ __volatile__("sfence" ::: "memory");  /* flush write-combined texels */

    /* Probe: transparent-texel + STP census — proves the alpha-test fix is
     * load-bearing (zero>0 = pages that painted black cut-out boxes) and that
     * per-texel semi-transparency is in use (stp1>0). First 12 pages decoded
     * past the boot/menu churn only. */
    {
        static int s_stpLogged = 0;
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
        SH_DBG("[VRAM] texture cache: %d/%d slots (256KB each), free=%uKB after",
               ok, CACHE_N, Xbox_MemFreeKB());
        if (ok < CACHE_N)
            SH_DBG("[VRAM] cache capped by %dMB heap reserve (expect decode thrash)",
                   HEAP_RESERVE_KB / 1024);
    }

    for (i = 0; i < CACHE_N; i++)
        if (s_cache[i].key == key && s_cache[i].argb) {
            s_cache[i].lastUse = (unsigned)g_Nv2aFrameCount;
            return s_cache[i].argb;
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
        unsigned thisFrame = (unsigned)g_Nv2aFrameCount;
        for (i = 0; i < CACHE_N; i++) {
            if (!s_cache[i].argb) continue;
            if (s_cache[i].key == -1) { best = i; break; }
            if (s_cache[i].lastUse < bestPinnedUse) { bestPinnedUse = s_cache[i].lastUse; bestPinned = i; }
            if (s_cache[i].lastUse == thisFrame) continue;   /* in flight — not evictable */
            if (s_cache[i].lastUse < bestUse) { bestUse = s_cache[i].lastUse; best = i; }
        }
        if (best < 0) {
            static unsigned s_pinnedOut;
            if ((++s_pinnedOut & 255) == 1)
                SH_DBG("[VRAM] all %d slots in flight (#%u) — evicting anyway",
                       CACHE_N, s_pinnedOut);
            best = bestPinned;
        }
        if (best < 0)
            return 0;                      /* cache never allocated */
        i = best;
    }
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
        GpuXbox_FbRegionOverlap(s_cache[i].px0, s_cache[i].py0, s_cache[i].px1, s_cache[i].py1))
        GpuXbox_FbReadbackForTexture();
    DecodePage(tpage, clut, s_cache[i].argb);
    s_cache[i].key     = key;
    s_cache[i].lastUse = (unsigned)g_Nv2aFrameCount;
    /* A miss = a 256x256 decode. After the cache fills this should go quiet; if it
     * keeps climbing the working set exceeds CACHE_N (thrashing -> slow). */
    if ((++s_decodeTotal & 511) == 0)
        SH_DBG("[VRAM] decode #%d", s_decodeTotal);
    return s_cache[i].argb;
}
