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
#define CACHE_N  24

static uint16_t s_vram[VRAM_W * VRAM_H];

typedef struct { int key; uint32_t* argb; } TexEntry;
static TexEntry s_cache[CACHE_N];
static int      s_cacheNext;
static int      s_cacheReady;

static void InvalidateCache(void)
{
    int i;
    for (i = 0; i < CACHE_N; i++)
        s_cache[i].key = -1;
    s_cacheNext = 0;
}

/* LoadImage: copy w*h 16-bit source pixels into VRAM at (x,y), row by row. */
void PsxVram_Load(int x, int y, int w, int h, const uint16_t* src)
{
    int row;

    if (!src || w <= 0 || h <= 0 || x < 0 || y < 0 || x >= VRAM_W || y >= VRAM_H)
        return;

    if (x + w > VRAM_W) w = VRAM_W - x;
    for (row = 0; row < h; row++) {
        int vy = y + row;
        if (vy >= VRAM_H) break;
        memcpy(&s_vram[vy * VRAM_W + x], &src[row * w], (size_t)w * 2);
    }
    InvalidateCache();
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
        for (i = 0; i < CACHE_N; i++) {
            s_cache[i].argb = (uint32_t*)GpuNv2a_AllocTexMem(TEX_DIM * TEX_DIM * 4);
            s_cache[i].key  = -1;
        }
        s_cacheReady = 1;
        SH_DBG("[VRAM] texture cache ready (%d x 256x256)", CACHE_N);
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
    return s_cache[i].argb;
}
