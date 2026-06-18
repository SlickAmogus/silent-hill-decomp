/*
 * main_xbox.c - Original Xbox (NXDK / NV2A) entry point for the Silent Hill 1 port.
 *
 * Mirror of pc_port/src/main_pc.c: the platform shell that brings up the Xbox HAL
 * and then hands control to the game's MainLoop() (in BODYPROG). main_pc.c has no
 * per-frame loop of its own — PsyCross hooks the PSX libgpu/libspu/libpad calls and
 * renders/pumps each frame. On Xbox that HAL is replaced piece by piece:
 *   - GPU:   PSX OT/prim rasterizer -> NV2A via pbkit (gpu_nv2a.c, TODO)
 *   - Audio: PSX SPU mix -> Xbox hardware audio (spu_xbox.c, TODO)
 *   - Input: USB controller -> PSX pad bits (pad_xbox.c, TODO)
 *   - File:  BIN disc image on HDD, sector reads via reused xa_player.c (fs_xbox.c, TODO)
 *
 * MILESTONE 1 (this file, current): prove the toolchain. Set a video mode, init
 * pbkit, clear to black, and log to D:\silenthill.log. The game MainLoop() is not
 * wired in yet (next milestone links the shared game code).
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <windows.h>

#include "sh_log.h"

extern void XboxFs_MountHomeDrive(void);   /* mount the XBE's dir as D: (fs_xbox.c) */
extern void Gte_SelfTest(void);           /* milestone-2b software-GTE proof (gte_selftest.c) */
extern void GpuNv2a_Init(void);           /* milestone-2a NV2A backend (gpu_nv2a.c) */
extern void GpuNv2a_DrawTestTriangle(void);

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    /* Mount the XBE's own directory as D: BEFORE opening the log there. */
    XboxFs_MountHomeDrive();

    SH_DebugLogInit();
    debugPrint("Silent Hill (Xbox) booting...\n");
    SH_DBG("[SH-XBOX] boot: video 640x480x32");

    Gte_SelfTest();

    int status = pb_init();
    if (status)
    {
        debugPrint("pb_init failed: %d\n", status);
        SH_DBG("[SH-XBOX] pb_init FAILED (%d)", status);
        Sleep(5000);
        return 1;
    }
    pb_show_front_screen();
    SH_DBG("[SH-XBOX] pbkit initialised");

    GpuNv2a_Init();

    int width  = pb_back_buffer_width();
    int height = pb_back_buffer_height();

    while (1)
    {
        pb_wait_for_vbl();
        pb_reset();
        pb_target_back_buffer();

        pb_erase_depth_stencil_buffer(0, 0, width, height);
        pb_fill(0, 0, width, height, 0xff000000); /* black */
        pb_erase_text_screen();

        GpuNv2a_DrawTestTriangle();

        while (pb_busy()) { }

        pb_print("Silent Hill - Xbox port\n");
        pb_print("Milestone 2: GTE + NV2A textured triangle\n");
        pb_print("Log: D:\\silenthill.log\n");
        pb_draw_text_screen();

        while (pb_busy()) { }
        while (pb_finished()) { }
    }

    pb_kill();
    return 0;
}
