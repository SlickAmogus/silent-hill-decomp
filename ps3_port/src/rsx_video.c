/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * rsx_video.c - RSX display setup, clear and flip.
 *
 * Includes PSL1GHT and NO decomp headers, which is the contract in ps3_hal.h:
 * the two header sets define incompatible u64/s64 and cannot share a TU. So
 * this file owns everything that touches libgcm, and gpu_rsx.c -- which does
 * include the decomp's primitive types -- drives it through the plain-C
 * entry points declared in ps3_hal.h.
 *
 * Split out from gpu_rsx.c rather than merged into it for exactly that reason,
 * not for tidiness: gpu_rsx.c needs ShVertex, ShVertex comes from gpu_nv2a.h,
 * and once the vertex path grows it will need more of the decomp's types.
 *
 * Double buffered with a Z24S8 depth surface. The PSX has no depth buffer --
 * the ordering table IS the depth sort -- but the Xbox ports found that the
 * item/inventory pass genuinely wants one, so the surface is allocated up front
 * rather than retrofitted later.
 */
#include <string.h>
#include <malloc.h>
#include <stdio.h>

#include <rsx/rsx.h>
#include <sysutil/video.h>
#include <sysmodule/sysmodule.h>

#include "ps3_hal.h"

/* The command buffer is carved OUT OF the host/IO region, so the two cannot be
 * the same size -- sizing both at 1 MB left no room for anything else and
 * rsxInit simply returned NULL with no diagnostic. These are PSL1GHT's own
 * sample values. 32 MB of IO is affordable against the ~213 MB a GameOS
 * homebrew process gets. */
#define CB_SIZE   (0x100000)          /* command buffer */
#define HOST_SIZE (32 * 1024 * 1024)  /* host memory mapped for the RSX */

static gcmContextData* s_ctx;
static videoResolution s_res;
static int             s_ready;
static int             s_curBuf;

static u32*  s_color[2];
static u32   s_colorOffset[2];
static u32   s_colorPitch;

static u32*  s_depth;
static u32   s_depthOffset;
static u32   s_depthPitch;

int  Ps3Rsx_Width(void)  { return s_ready ? (int)s_res.width  : 0; }
int  Ps3Rsx_Height(void) { return s_ready ? (int)s_res.height : 0; }
int  Ps3Rsx_Ready(void)  { return s_ready; }

int Ps3Rsx_Init(void)
{
    void*               host;
    videoState          state;
    videoConfiguration  vcfg;
    int                 i;

    if (s_ready)
        return 1;

    /* GCM_SYS is not in a GameOS process by default and rsxInit fails flatly
     * without it -- no diagnostic, just a NULL context, which is how the first
     * attempt here reported "rsxInit failed" with everything else correct. */
    sysModuleLoad(SYSMODULE_GCM_SYS);

    /* The IO buffer must be 1 MB aligned and a whole number of megabytes; the
     * RSX maps it through the GPU MMU at that granularity. */
    host = memalign(1024 * 1024, HOST_SIZE);
    if (!host) {
        printf("[SH-PS3][RSX] host alloc failed\n");
        return 0;
    }

    s_ctx = rsxInit(CB_SIZE, HOST_SIZE, host);
    if (!s_ctx) {
        printf("[SH-PS3][RSX] rsxInit(cb=%u io=%u addr=%p) failed\n",
               (unsigned)CB_SIZE, (unsigned)HOST_SIZE, host);
        return 0;
    }

    /* Take whatever mode the console is already in rather than forcing one.
     * The user's display may not do 720p, and a mode the TV rejects is a black
     * screen that looks exactly like a renderer bug. */
    if (videoGetState(0, 0, &state) != 0 ||
        videoGetResolution(state.displayMode.resolution, &s_res) != 0) {
        printf("[SH-PS3][RSX] videoGetState/Resolution failed\n");
        return 0;
    }

    memset(&vcfg, 0, sizeof(vcfg));
    vcfg.resolution = state.displayMode.resolution;
    vcfg.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vcfg.pitch      = s_res.width * 4;
    vcfg.aspect     = state.displayMode.aspect;
    if (videoConfigure(0, &vcfg, NULL, 0) != 0) {
        printf("[SH-PS3][RSX] videoConfigure failed\n");
        return 0;
    }

    gcmSetFlipMode(GCM_FLIP_VSYNC);

    s_colorPitch = s_res.width * 4;
    for (i = 0; i < 2; i++) {
        s_color[i] = (u32*)rsxMemalign(64, s_colorPitch * s_res.height);
        if (!s_color[i] || rsxAddressToOffset(s_color[i], &s_colorOffset[i]) != 0) {
            printf("[SH-PS3][RSX] colour buffer %d alloc failed\n", i);
            return 0;
        }
        if (gcmSetDisplayBuffer(i, s_colorOffset[i], s_colorPitch,
                                s_res.width, s_res.height) != 0) {
            printf("[SH-PS3][RSX] gcmSetDisplayBuffer %d failed\n", i);
            return 0;
        }
    }

    s_depthPitch = s_res.width * 4;
    s_depth = (u32*)rsxMemalign(64, s_depthPitch * s_res.height);
    if (!s_depth || rsxAddressToOffset(s_depth, &s_depthOffset) != 0) {
        printf("[SH-PS3][RSX] depth buffer alloc failed\n");
        return 0;
    }

    s_curBuf = 0;
    s_ready  = 1;
    printf("[SH-PS3][RSX] up: %ux%u pitch=%u\n",
           (unsigned)s_res.width, (unsigned)s_res.height, (unsigned)s_colorPitch);
    return 1;
}

/* Point the RSX at the buffer we are about to draw into. Done every frame
 * because the two colour surfaces alternate. */
static void BindSurface(void)
{
    gcmSurface sf;

    memset(&sf, 0, sizeof(sf));
    sf.colorFormat      = GCM_TF_COLOR_X8R8G8B8;
    sf.colorTarget      = GCM_TF_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX;
    sf.colorOffset[0]   = s_colorOffset[s_curBuf];
    sf.colorPitch[0]    = s_colorPitch;
    /* The three unused MRT slots must still point somewhere legal; the RSX
     * validates them even with a single target selected. */
    sf.colorLocation[1] = sf.colorLocation[2] = sf.colorLocation[3] = GCM_LOCATION_RSX;
    sf.colorPitch[1]    = sf.colorPitch[2]    = sf.colorPitch[3]    = 64;

    sf.depthFormat   = GCM_TF_ZETA_Z24S8;
    sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset   = s_depthOffset;
    sf.depthPitch    = s_depthPitch;

    sf.type      = GCM_TF_TYPE_LINEAR;
    sf.antiAlias = GCM_TF_CENTER_1;
    sf.width     = s_res.width;
    sf.height    = s_res.height;
    sf.x         = 0;
    sf.y         = 0;

    rsxSetSurface(s_ctx, &sf);
}

void Ps3Rsx_FrameBegin(unsigned int clearArgb)
{
    if (!s_ready)
        return;

    BindSurface();
    rsxSetClearColor(s_ctx, clearArgb);
    rsxSetClearDepthValue(s_ctx, 0xFFFF);
    rsxClearSurface(s_ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B |
                           GCM_CLEAR_A | GCM_CLEAR_Z | GCM_CLEAR_S);
    rsxSetScissor(s_ctx, 0, 0, s_res.width, s_res.height);
}

void Ps3Rsx_FrameEnd(void)
{
    if (!s_ready)
        return;

    gcmSetFlip(s_ctx, s_curBuf);
    rsxFlushBuffer(s_ctx);
    /* Queue the wait on the GPU rather than spinning the PPU: the flip is
     * vsync-locked, so blocking here would burn a whole frame of CPU that the
     * game's own loop wants. */
    gcmSetWaitFlip(s_ctx);
    s_curBuf ^= 1;
}

/* Called once per presented frame to keep the flip status from saturating.
 * Separate from FrameEnd because the caller may end a frame it never presents
 * (the counting phase does exactly that). */
void Ps3Rsx_WaitFlip(void)
{
    if (!s_ready)
        return;
    while (gcmGetFlipStatus() != 0)
        Ps3_SleepMs(0);
    gcmResetFlipStatus();
}
