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
#define CACHE_N  48                  /* 48 * 256KB = 12 MB of decoded textures */

static uint16_t s_vram[VRAM_W * VRAM_H];

typedef struct {
    int       key;
    uint32_t* argb;
    /* Source-VRAM footprint of this decoded page, in 16-bit words, [x0,x1) x
     * [y0,y1). Page rect and CLUT rect are tracked separately so a CLUT-only
     * write still invalidates an indexed page that samples it. */
    int px0, py0, px1, py1;   /* texture-page rect */
    int cx0, cy0, cx1, cy1;   /* CLUT rect (empty for 16-bit direct) */
} TexEntry;
static TexEntry s_cache[CACHE_N];
static int      s_cacheNext;
static int      s_cacheReady;
static int      s_decodeTotal;

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
    int row, yend;

    if (!src || w <= 0 || h <= 0 || x < 0 || y < 0 || x >= VRAM_W || y >= VRAM_H)
        return;

    if (x + w > VRAM_W) w = VRAM_W - x;
    yend = y;
    for (row = 0; row < h; row++) {
        int vy = y + row;
        if (vy >= VRAM_H) break;
        memcpy(&s_vram[vy * VRAM_W + x], &src[row * w], (size_t)w * 2);
        yend = vy + 1;
    }
    InvalidateRegion(x, y, x + w, yend);   /* selective, not nuke-all */
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
    if (c == 0)
        return 0;                    /* PSX: 0x0000 = fully transparent */
    r = (c & 0x1F) << 3;
    g = ((c >> 5) & 0x1F) << 3;
    b = ((c >> 10) & 0x1F) << 3;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
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
            o[u] = Psx16ToArgb(texel);
        }
    }
    __asm__ __volatile__("sfence" ::: "memory");  /* flush write-combined texels */
}

/* Decode (or fetch cached) the texture page for (tpage,clut). Returns a 256x256
 * A8R8G8B8 buffer, or NULL if the texture allocator failed. */
uint32_t* PsxVram_GetTexture(int tpage, int clut)
{
    int key = ((tpage & 0xFFFF) << 16) | (clut & 0xFFFF);
    int i;

    if (!s_cacheReady) {
        int ok = 0;
        for (i = 0; i < CACHE_N; i++) {
            s_cache[i].argb = (uint32_t*)GpuNv2a_AllocTexMem(TEX_DIM * TEX_DIM * 4);
            s_cache[i].key  = -1;
            if (s_cache[i].argb) ok++;
        }
        s_cacheReady = 1;
        SH_DBG("[VRAM] texture cache: %d/%d slots (256KB each)", ok, CACHE_N);
    }

    for (i = 0; i < CACHE_N; i++)
        if (s_cache[i].key == key && s_cache[i].argb)
            return s_cache[i].argb;

    i = s_cacheNext;
    s_cacheNext = (s_cacheNext + 1) % CACHE_N;
    if (!s_cache[i].argb)
        return 0;
    DecodePage(tpage, clut, s_cache[i].argb);
    s_cache[i].key = key;
    TexEntry_SetBBox(&s_cache[i], tpage, clut);   /* record VRAM footprint for selective invalidation */
    /* A miss = a 256x256 decode. After the cache fills this should go quiet; if it
     * keeps climbing the working set exceeds CACHE_N (thrashing -> slow). */
    if ((++s_decodeTotal & 511) == 0)
        SH_DBG("[VRAM] decode #%d", s_decodeTotal);
    return s_cache[i].argb;
}
