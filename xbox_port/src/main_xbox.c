/*
 * main_xbox.c - Original Xbox (NXDK / NV2A) entry point for the Silent Hill 1 port.
 *
 * Mirror of pc_port/src/main_pc.c: brings up the Xbox HAL and (eventually) hands
 * control to the game's MainLoop(). Until the shared game code is wired in, this
 * exercises the HAL pieces built so far: the software GTE (gte_selftest.c) and
 * the PSX libgpu (gpu_xbox.c DrawOTag) on the NV2A backend, by walking a small
 * hand-built ordering table of polygons each frame.
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <windows.h>

#include <libgte.h>
#include <libgpu.h>

#include "gpu_nv2a.h"
#include "sh_log.h"

extern void XboxFs_MountHomeDrive(void);
extern void Gte_SelfTest(void);

/* Hand-built ordering table: a flat-white triangle + a gouraud quad, linked
 * head -> quad -> tri -> terminator. DrawOTag walks it and renders via NV2A. */
static OT_TAG   g_testOt;
static POLY_G4  g_testQuad;
static POLY_F3  g_testTri;

static void BuildTestOT(void)
{
    /* empty OT head bucket (len 0), terminates the chain */
    setlen(&g_testOt, 0);
    setaddr(&g_testOt, 0xffffffff);

    /* gouraud quad (code 0x38, 8 longs): R/G/B/Y corners */
    setlen(&g_testQuad, 8);
    setcode(&g_testQuad, 0x38);
    g_testQuad.r0 = 255; g_testQuad.g0 = 0;   g_testQuad.b0 = 0;
    g_testQuad.r1 = 0;   g_testQuad.g1 = 255; g_testQuad.b1 = 0;
    g_testQuad.r2 = 0;   g_testQuad.g2 = 0;   g_testQuad.b2 = 255;
    g_testQuad.r3 = 255; g_testQuad.g3 = 255; g_testQuad.b3 = 0;
    g_testQuad.x0 = 80;  g_testQuad.y0 = 80;
    g_testQuad.x1 = 300; g_testQuad.y1 = 90;
    g_testQuad.x2 = 90;  g_testQuad.y2 = 300;
    g_testQuad.x3 = 300; g_testQuad.y3 = 300;

    /* flat white triangle (code 0x20, 4 longs) */
    setlen(&g_testTri, 4);
    setcode(&g_testTri, 0x20);
    g_testTri.r0 = 255; g_testTri.g0 = 255; g_testTri.b0 = 255;
    g_testTri.x0 = 420; g_testTri.y0 = 100;
    g_testTri.x1 = 360; g_testTri.y1 = 320;
    g_testTri.x2 = 540; g_testTri.y2 = 340;

    /* link: head -> quad -> tri -> terminator */
    setaddr(&g_testTri,  0xffffffff);
    setaddr(&g_testQuad, (uintptr_t)&g_testTri);
    setaddr(&g_testOt,   (uintptr_t)&g_testQuad);
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    XboxFs_MountHomeDrive();
    SH_DebugLogInit();
    debugPrint("Silent Hill (Xbox) booting...\n");
    SH_DBG("[SH-XBOX] boot: video 640x480x32");

    Gte_SelfTest();

    int status = pb_init();
    if (status) {
        debugPrint("pb_init failed: %d\n", status);
        SH_DBG("[SH-XBOX] pb_init FAILED (%d)", status);
        Sleep(5000);
        return 1;
    }
    pb_show_front_screen();
    SH_DBG("[SH-XBOX] pbkit initialised");

    GpuNv2a_Init();
    BuildTestOT();
    SH_DBG("[SH-XBOX] entering render loop (DrawOTag test)");

    while (1) {
        GpuNv2a_FrameBegin();

        DrawOTag((u_long*)&g_testOt);

        pb_print("Silent Hill - Xbox port\n");
        pb_print("Milestone 3: PSX libgpu DrawOTag on NV2A\n");
        pb_draw_text_screen();

        GpuNv2a_FrameEnd();
    }

    pb_kill();
    return 0;
}
