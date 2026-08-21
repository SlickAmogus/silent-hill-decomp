/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gpu_min.c - the smallest possible Xe program, because the game proved
 * nothing about the GPU that a probe this small cannot prove better.
 *
 * Log 037 pinned the fault precisely: the display scans the surface Xe
 * resolves into (scan == fb.ptr), the resolve DOES write that memory every
 * frame (the probe pixels change), but what it writes is structured garbage
 * instead of the programmed clear colour. That is one step from a working
 * display, and the remaining suspects are (a) something in the game's large
 * runtime interfering and (b) this console's Xe state as left by the
 * BadUpdate/XeUnshackle chain -- no libXenon 3D code has ever been proven on
 * this machine, XeLL itself is framebuffer-2D only.
 *
 * This is the canonical libXenon sequence and nothing else. Two phases:
 *   1. ~10s: clear to GREEN through Xe_Resolve's own clear   (copy path)
 *   2. ~10s: clear to RED through Xe_Clear, then resolve      (adds the
 *      internal clear-draw, still no shader of ours anywhere)
 * On-screen printfs carry the CPU-readback pixels, so a photo of the screen
 * is the data. GREEN screen in phase 1 = Xe works in a minimal environment
 * and the game runtime is the suspect. Same garbage = this console's Xe
 * bring-up is the suspect, with the game exonerated.
 */
#include <stdio.h>
#include <string.h>

#include <xenos/xe.h>
#include <xenos/xenos.h>
#include <console/console.h>
#include <ppc/cache.h>
#include <time/time.h>

static struct XenosDevice s_dev;

static void Probe(struct XenosDevice* xe, const char* tag, int frame)
{
    struct XenosSurface* fb   = Xe_GetFramebufferSurface(xe);
    unsigned char*       base = (unsigned char*)(0x80000000u | (unsigned)fb->ptr);
    unsigned             off  = (unsigned)(fb->wpitch * (fb->height / 2)
                                           + (fb->width / 2) * 4);
    unsigned             px[4];
    int                  i;

    memdcbf(base + off, 128);
    for (i = 0; i < 4; i++)
        px[i] = ((unsigned*)(base + off))[i];
    printf("[%s f=%d] scan=%08x centre= %08x %08x %08x %08x\n",
           tag, frame, *(volatile unsigned*)0xec806110,
           px[0], px[1], px[2], px[3]);
}

int main(void)
{
    struct XenosDevice* xe = &s_dev;
    int f;

    xenos_init(VIDEO_MODE_AUTO);
    console_init();
    printf("gpu_min: video up\n");

    Xe_Init(xe);
    Xe_SetRenderTarget(xe, Xe_GetFramebufferSurface(xe));
    {
        struct XenosSurface* fb = Xe_GetFramebufferSurface(xe);
        printf("gpu_min: Xe up %dx%d ptr=%08x pitch=%d\n",
               fb->width, fb->height, (unsigned)fb->ptr, fb->wpitch);
    }

    /* Phase 1: the exact loop every libXenon demo runs, minus the draw.
     * Expect: solid GREEN, this text flickering or vanishing under it. */
    for (f = 0; f < 600; f++) {
        Xe_InvalidateState(xe);
        Xe_SetClearColor(xe, 0xFF00FF00u);
        Xe_Resolve(xe);
        Xe_Sync(xe);
        if ((f % 150) == 0)
            Probe(xe, "green", f);
    }

    /* Phase 2: same, plus Xe_Clear -- the library's own internal clear-draw
     * machinery, still nothing of ours. Expect: solid RED. */
    for (f = 0; f < 600; f++) {
        Xe_InvalidateState(xe);
        Xe_SetClearColor(xe, 0xFFFF0000u);
        Xe_Clear(xe, XE_CLEAR_COLOR | XE_CLEAR_DS);
        Xe_Resolve(xe);
        Xe_Sync(xe);
        if ((f % 150) == 0)
            Probe(xe, "red", f);
    }

    printf("gpu_min: done. Screen stayed garbage => console Xe bring-up issue.\n");
    printf("gpu_min: photo this screen; power off when done.\n");
    for (;;) { }
    return 0;
}
