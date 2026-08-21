/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gpu_selftest.c - draw a KNOWN image, so "can this console render at all?"
 * stops being a matter of opinion.
 *
 * Every screen so far has been garbage, and garbage is ambiguous: it looks the
 * same whether the Xenos pipeline is broken or whether the pipeline is fine and
 * the game is feeding it nonsense. Four bugs in a row turned out to be the
 * second kind (a spawn position 400 million units outside the world, an overlay
 * header with its pointers zeroed, every transform collapsed to identity), but
 * none of them PROVED the first kind was not also true.
 *
 * This settles it without the game. It builds its own vertices, its own
 * texture, and its own colours, and draws them through the same backend the
 * game uses -- GpuNv2a_FrameBegin / BatchAlloc / BindTexture / FrameEnd. If the
 * pattern below appears on the TV, then the vertex format, the viewport
 * mapping, both shaders, the blend state, texture upload and binding, the EDRAM
 * resolve and the present are all working, and every remaining problem is data
 * being handed to a renderer that was ready for it. If it does NOT appear, the
 * fault is in this file's path and nowhere near the game.
 *
 * What is on screen, top to bottom:
 *   1. Four solid bars: red, green, blue, white. Proves untextured colour, the
 *      pixel-to-clip mapping and the x2 colour convention. If these are the
 *      wrong colours the NORMAL-slot colour path is wrong; if they are in the
 *      wrong place the viewport constant is.
 *   2. A checkerboard quad, uploaded through GpuNv2a_BindTexture. Proves the
 *      texture upload, the texel-to-UV scale and the sampler. This is the one
 *      that matters most right now, because the game logs texbind=0 -- the game
 *      never binds a texture, so nothing has ever exercised this path.
 *   3. A white block that steps one square right each frame, so a FROZEN image
 *      is distinguishable from a live one. A still frame that looks correct can
 *      still mean the loop died after one present.
 *
 * Deliberately NOT gated behind a config flag: the whole point is that it runs
 * before anything can go wrong, on a console where a test cycle costs a
 * BadUpdate run. It ends on its own after a few seconds and the game boots
 * normally afterwards.
 */
#include <string.h>

#include "sh_log.h"
#include "gpu_nv2a.h"

extern void  GpuNv2a_FrameBegin(void);
extern void  GpuNv2a_FrameEnd(void);
extern void  GpuNv2a_WaitVbl(void);
extern ShVertex* GpuNv2a_BatchAlloc(int count);
extern void  GpuNv2a_BindTexture(const void* addr, int w, int h);
extern void  GpuNv2a_BindWhite(void);
extern void  GpuNv2a_SetBlendMode(int mode);
extern void* GpuNv2a_AllocTexMem(int bytes);
extern int   g_Nv2aContentW;
extern int   g_Nv2aContentH;
extern void  SH_DebugLogFlush(void);

#define TEST_TEX_DIM 64

/* Colour reaches the shader already x2-modulated (PSX 0x80 means 1.0), so 1.0
 * here is full brightness and the pixel shader's texel*col leaves a white
 * texture alone. */
static void Quad(float x0, float y0, float x1, float y1,
                 float r, float g, float b,
                 float u0, float v0, float u1, float v1)
{
    ShVertex* v = GpuNv2a_BatchAlloc(6);
    int       i;
    /* Two triangles, counter-clockwise, matching what gpu_xbox.c emits. */
    const float px[6] = { x0, x1, x0, x1, x1, x0 };
    const float py[6] = { y0, y0, y1, y0, y1, y1 };
    const float pu[6] = { u0, u1, u0, u1, u1, u0 };
    const float pv[6] = { v0, v0, v1, v0, v1, v1 };

    if (!v)
        return;

    for (i = 0; i < 6; i++) {
        v[i].pos[0] = px[i]; v[i].pos[1] = py[i]; v[i].pos[2] = 0.0f; v[i].pos[3] = 1.0f;
        v[i].col[0] = r;     v[i].col[1] = g;     v[i].col[2] = b;    v[i].col[3] = 1.0f;
        v[i].tex[0] = pu[i]; v[i].tex[1] = pv[i];
        /* spec is FOG and is ADDED after the modulate -- leaving it uninitialised
         * would add stack garbage to every pixel and look like a colour bug. */
        v[i].spec[0] = v[i].spec[1] = v[i].spec[2] = v[i].spec[3] = 0.0f;
        v[i].pad[0]  = v[i].pad[1]  = 0.0f;
    }
}

extern void GpuXenos_DebugClearOverride(unsigned argb, int enable);
extern void GpuXenos_DebugFbProbe(const char* tag);

void Gpu360_SelfTest(void)
{
    unsigned* tex;
    int       frame;
    int       W = g_Nv2aContentW;
    int       H = g_Nv2aContentH;

    /* psx_vram's ARGB8888 layout, which is what BindTexture expects to copy. */
    tex = (unsigned*)GpuNv2a_AllocTexMem(TEST_TEX_DIM * TEST_TEX_DIM * 4);
    if (!tex) {
        SH_DBG("[SELFTEST] texture alloc failed - skipping");
        return;
    }
    {
        int x, y;
        for (y = 0; y < TEST_TEX_DIM; y++) {
            for (x = 0; x < TEST_TEX_DIM; x++) {
                int on = ((x >> 3) ^ (y >> 3)) & 1;
                /* Alpha must be 0xFF: the pixel shader clips anything at alpha 0,
                 * so a zero-alpha checker would discard every texel and the quad
                 * would simply not be there. */
                tex[y * TEST_TEX_DIM + x] = on ? 0xFFFFC000u   /* amber */
                                               : 0xFF2040FFu;  /* blue  */
            }
        }
    }

    SH_DBG("[SELFTEST] start: %dx%d content, %dx%d checker", W, H, TEST_TEX_DIM, TEST_TEX_DIM);
    SH_DebugLogFlush();

    /* PHASE 0, ~2s: draw NOTHING with the clear forced to bright green. This
     * splits the pipeline at the resolve: a green screen proves EDRAM clear ->
     * resolve -> scanout with no draw, no shader and no vertex fetch involved.
     * Log 034 strongly suggested exactly this split (the screen went to the
     * black CLEAR colour while 6 prims/frame went nowhere), and one green
     * screen turns that inference into fact. */
    GpuXenos_DebugClearOverride(0xFF00FF00u, 1);
    for (frame = 0; frame < 120; frame++) {
        GpuNv2a_FrameBegin();
        GpuNv2a_FrameEnd();
        GpuNv2a_WaitVbl();
        if (frame == 2)
            GpuXenos_DebugFbProbe("green-clear");   /* want 0xFF00FF00 x4 */
    }
    SH_DebugLogFlush();

    for (frame = 0; frame < 240; frame++) {   /* ~4s at 60Hz */
        float barH = (float)H * 0.18f;
        float y    = 0.0f;

        GpuNv2a_FrameBegin();
        GpuNv2a_SetBlendMode(0);

        /* 1. Untextured colour bars. */
        GpuNv2a_BindWhite();
        Quad(0.0f, y, (float)W * 0.25f, y + barH, 1.0f, 0.0f, 0.0f, 0,0,0,0);
        Quad((float)W * 0.25f, y, (float)W * 0.50f, y + barH, 0.0f, 1.0f, 0.0f, 0,0,0,0);
        Quad((float)W * 0.50f, y, (float)W * 0.75f, y + barH, 0.0f, 0.0f, 1.0f, 0,0,0,0);
        Quad((float)W * 0.75f, y, (float)W,         y + barH, 1.0f, 1.0f, 1.0f, 0,0,0,0);

        /* 2. Textured checkerboard. UVs are in TEXELS, not normalised. */
        GpuNv2a_BindTexture(tex, TEST_TEX_DIM, TEST_TEX_DIM);
        Quad((float)W * 0.25f, barH + 20.0f, (float)W * 0.75f, barH + 20.0f + (float)H * 0.45f,
             1.0f, 1.0f, 1.0f,
             0.0f, 0.0f, (float)TEST_TEX_DIM, (float)TEST_TEX_DIM);

        /* 3. A marker that moves, so a frozen frame is obvious. */
        GpuNv2a_BindWhite();
        {
            float step = (float)((frame * 4) % (W - 40));
            Quad(step, (float)H - 50.0f, step + 40.0f, (float)H - 10.0f,
                 1.0f, 1.0f, 0.0f, 0,0,0,0);
        }

        GpuNv2a_FrameEnd();
        GpuNv2a_WaitVbl();

        if (frame == 2)
            GpuXenos_DebugFbProbe("bars");   /* centre sits in the checker quad */

        if ((frame % 60) == 0) {
            SH_DBG("[SELFTEST] frame %d drawn", frame);
            SH_DebugLogFlush();
        }
    }

    GpuXenos_DebugClearOverride(0, 0);
    SH_DBG("[SELFTEST] done - green screen = resolve works; bars = draws work");
    SH_DebugLogFlush();
}
