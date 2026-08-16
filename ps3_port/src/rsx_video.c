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

/* Shaders, built by cgcomp and embedded with bin2s (see build_ps3.sh). */
extern const u8 sh_vp_vpo[];
extern const u8 sh_fp_fpo[];

static rsxVertexProgram*   s_vp;
static rsxFragmentProgram* s_fp;
static void*               s_vpUCode;
static void*               s_fpUCode;      /* must live in RSX memory */
static u32                 s_fpOffset;
static s32                 s_cScreen  = -1;
static s32                 s_cTexScale = -1;
static s32                 s_aPos = -1, s_aCol = -1, s_aTex = -1;

static u8*  s_vbuf;
static u32  s_vbufOffset;
static u32  s_vbufStride;

/* 1x1 white, for untextured primitives. */
static u32* s_white;
static u32  s_whiteOffset;

static int  s_texW = 1, s_texH = 1;

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

    /* --- shaders ---------------------------------------------------------
     * The vertex ucode may stay in main memory; the FRAGMENT ucode may not --
     * the RSX fetches it itself, so it has to be copied into RSX memory and
     * referenced by offset. */
    s_vp      = (rsxVertexProgram*)sh_vp_vpo;
    s_fp      = (rsxFragmentProgram*)sh_fp_fpo;
    s_vpUCode = rsxVertexProgramGetUCode(s_vp);
    {
        u32   fpSize = 0;
        void* src    = rsxFragmentProgramGetUCode(s_fp, &fpSize);
        s_fpUCode = rsxMemalign(64, fpSize);
        if (!s_fpUCode || rsxAddressToOffset(s_fpUCode, &s_fpOffset) != 0) {
            printf("[SH-PS3][RSX] fragment ucode alloc failed\n");
            return 0;
        }
        memcpy(s_fpUCode, src, fpSize);
    }
    s_cScreen   = rsxVertexProgramGetConst(s_vp, "screen");
    s_cTexScale = rsxVertexProgramGetConst(s_vp, "texScale");
    s_aPos      = rsxVertexProgramGetAttrib(s_vp, "position");
    s_aCol      = rsxVertexProgramGetAttrib(s_vp, "color");
    s_aTex      = rsxVertexProgramGetAttrib(s_vp, "texcoord");
    if (s_aPos < 0 || s_aCol < 0 || s_aTex < 0 || s_cScreen < 0) {
        printf("[SH-PS3][RSX] shader binding failed pos=%d col=%d tex=%d screen=%d\n",
               (int)s_aPos, (int)s_aCol, (int)s_aTex, (int)s_cScreen);
        return 0;
    }

    /* 1x1 white so untextured prims go through the same shader. */
    s_white = (u32*)rsxMemalign(128, 64);
    if (!s_white || rsxAddressToOffset(s_white, &s_whiteOffset) != 0) {
        printf("[SH-PS3][RSX] white texture alloc failed\n");
        return 0;
    }
    s_white[0] = 0xFFFFFFFFu;

    s_curBuf = 0;
    s_ready  = 1;
    printf("[SH-PS3][RSX] up: %ux%u pitch=%u\n",
           (unsigned)s_res.width, (unsigned)s_res.height, (unsigned)s_colorPitch);
    return 1;
}

void* Ps3Rsx_VertexPool(unsigned bytes, unsigned stride)
{
    if (!s_ready)
        return 0;
    if (s_vbuf)
        return s_vbuf;
    s_vbuf = (u8*)rsxMemalign(128, bytes);
    if (!s_vbuf || rsxAddressToOffset(s_vbuf, &s_vbufOffset) != 0) {
        printf("[SH-PS3][RSX] vertex pool (%u bytes) alloc failed\n", bytes);
        s_vbuf = 0;
        return 0;
    }
    s_vbufStride = stride;
    printf("[SH-PS3][RSX] vertex pool %u bytes, stride %u\n", bytes, stride);
    return s_vbuf;
}

void* Ps3Rsx_AllocTexMem(int bytes)
{
    if (bytes <= 0)
        return 0;
    /* Plain memalign when the RSX is down: psx_vram.c decodes into this buffer
     * either way, and a NULL here would turn "no display" into a crash. */
    if (!s_ready)
        return memalign(128, (unsigned)bytes);
    return rsxMemalign(128, (unsigned)bytes);
}

void Ps3Rsx_SetScissorRect(int x, int y, int w, int h)
{
    if (!s_ready)
        return;
    if (w <= 0 || h <= 0) { x = 0; y = 0; w = s_res.width; h = s_res.height; }
    rsxSetScissor(s_ctx, (u16)x, (u16)y, (u16)w, (u16)h);
}

/* PSX semi-transparency. ABR 0..3 are 0.5B+0.5F, B+F, B-F, B+0.25F.
 *
 * Textured prims carry the STP bit per texel as alpha out of the VRAM decode,
 * so SRC_ALPHA/ONE_MINUS_SRC_ALPHA reproduces ABR0 for them; untextured ABR0
 * prims are given vertex alpha 0.5 by gpu_xbox.c for the same reason. B-F is a
 * REVERSE_SUBTRACT because the PSX subtracts the FOREGROUND from the
 * background, not the other way round. */
void Ps3Rsx_SetBlend(int mode)
{
    if (!s_ready)
        return;

    if (mode == 0) {
        rsxSetBlendEnable(s_ctx, GCM_FALSE);
        return;
    }

    rsxSetBlendEnable(s_ctx, GCM_TRUE);
    switch (mode) {
    case 1:  /* ABR 0: 0.5 B + 0.5 F */
        rsxSetBlendFunc(s_ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA,
                               GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA);
        rsxSetBlendEquation(s_ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
        break;
    case 2:  /* ABR 1: B + F */
        rsxSetBlendFunc(s_ctx, GCM_SRC_ALPHA, GCM_ONE, GCM_SRC_ALPHA, GCM_ONE);
        rsxSetBlendEquation(s_ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
        break;
    case 3:  /* ABR 2: B - F */
        rsxSetBlendFunc(s_ctx, GCM_SRC_ALPHA, GCM_ONE, GCM_SRC_ALPHA, GCM_ONE);
        rsxSetBlendEquation(s_ctx, GCM_FUNC_REVERSE_SUBTRACT, GCM_FUNC_REVERSE_SUBTRACT);
        break;
    default: /* ABR 3: B + 0.25 F */
        rsxSetBlendFunc(s_ctx, GCM_CONSTANT_ALPHA, GCM_ONE, GCM_CONSTANT_ALPHA, GCM_ONE);
        rsxSetBlendColor(s_ctx, 0x40404040u, 0x40404040u);
        rsxSetBlendEquation(s_ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
        break;
    }
}

#define SH_TEX_REMAP \
    ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) | \
     (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) | \
     (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) | \
     (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) | \
     (GCM_TEXTURE_REMAP_COLOR_B    << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) | \
     (GCM_TEXTURE_REMAP_COLOR_G    << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) | \
     (GCM_TEXTURE_REMAP_COLOR_R    << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) | \
     (GCM_TEXTURE_REMAP_COLOR_A    << GCM_TEXTURE_REMAP_COLOR_A_SHIFT))

static void BindTexOffset(u32 offset, int w, int h)
{
    gcmTexture tex;

    memset(&tex, 0, sizeof(tex));
    /* LIN: the decoded pages are plain linear images, not swizzled.
     * NRM: normalised texcoords, which is what the vertex program produces
     * after dividing the PSX texel UVs by texScale. */
    tex.format    = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN |
                    GCM_TEXTURE_FORMAT_NRM;
    tex.mipmap    = 1;
    tex.dimension = GCM_TEXTURE_DIMS_2D;
    tex.cubemap   = GCM_FALSE;
    tex.remap     = SH_TEX_REMAP;
    tex.width     = (u16)w;
    tex.height    = (u16)h;
    tex.depth     = 1;
    tex.location  = GCM_LOCATION_RSX;
    tex.pitch     = (u32)w * 4;
    tex.offset    = offset;

    rsxLoadTexture(s_ctx, 0, &tex);
    rsxTextureControl(s_ctx, 0, GCM_TRUE, 0, 0, GCM_TEXTURE_MAX_ANISO_1);
    /* NEAREST, deliberately: the PSX has no filtering, and bilinear on a
     * paletted-decode page bleeds neighbouring atlas cells into each other --
     * which on this game's font atlas shows up as haloed glyph edges. */
    rsxTextureFilter(s_ctx, 0, GCM_TEXTURE_NEAREST, GCM_TEXTURE_NEAREST,
                     GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(s_ctx, 0, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
                       GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_NEVER, 0);
    s_texW = w;
    s_texH = h;
}

void Ps3Rsx_BindTexture(const void* pixels, int w, int h)
{
    u32 off;
    if (!s_ready || !pixels || w <= 0 || h <= 0)
        return;
    if (rsxAddressToOffset((void*)pixels, &off) != 0)
        return;                 /* not RSX-visible: leave the last bind alone */
    BindTexOffset(off, w, h);
}

void Ps3Rsx_BindWhite(void)
{
    if (!s_ready)
        return;
    BindTexOffset(s_whiteOffset, 1, 1);
}

void Ps3Rsx_DrainGpu(void)
{
    static u32 s_ref;
    if (!s_ready)
        return;
    /* rsxFinish both flushes and waits on an incrementing reference value, so
     * the counter must advance or the second call returns immediately. */
    rsxFinish(s_ctx, ++s_ref);
}

void Ps3Rsx_DrawTris(unsigned firstVert, unsigned vertCount)
{
    float screen[4];
    float texScale[4];

    if (!s_ready || !s_vbuf || vertCount < 3)
        return;

    rsxLoadVertexProgram(s_ctx, s_vp, s_vpUCode);
    rsxLoadFragmentProgramLocation(s_ctx, s_fp, s_fpOffset, GCM_LOCATION_RSX);

    /* Pixels -> clip space. y is negated because PSX screen space grows down. */
    screen[0] = 2.0f / (float)s_res.width;
    screen[1] = -2.0f / (float)s_res.height;
    screen[2] = 0.0f;
    screen[3] = 0.0f;
    rsxSetVertexProgramParameter(s_ctx, s_vp, s_cScreen, screen);

    if (s_cTexScale >= 0) {
        texScale[0] = 1.0f / (float)s_texW;
        texScale[1] = 1.0f / (float)s_texH;
        texScale[2] = 0.0f;
        texScale[3] = 0.0f;
        rsxSetVertexProgramParameter(s_ctx, s_vp, s_cTexScale, texScale);
    }

    {
        const u32 base = s_vbufOffset + firstVert * s_vbufStride;
        /* Offsets match ShVertex: pos at 0, col at 16, tex at 32. */
        rsxBindVertexArrayAttrib(s_ctx, (u8)s_aPos, base + 0,  (u8)s_vbufStride, 4,
                                 GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
        rsxBindVertexArrayAttrib(s_ctx, (u8)s_aCol, base + 16, (u8)s_vbufStride, 4,
                                 GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
        rsxBindVertexArrayAttrib(s_ctx, (u8)s_aTex, base + 32, (u8)s_vbufStride, 2,
                                 GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    }

    rsxDrawVertexArray(s_ctx, GCM_TYPE_TRIANGLES, 0, vertCount);
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

    /* The ordering table IS the depth sort -- DrawOTag walks far to near and
     * the primitives are submitted in that order -- so depth testing must be
     * OFF or later (nearer) geometry would be rejected against whatever the
     * clear left behind. Culling is off for the same reason the Xbox ports have
     * it off: PSX primitives carry no consistent winding.
     *
     * The viewport must be set every frame; it is part of the surface state the
     * clear/bind sequence resets. */
    {
        f32 scale[4], offset[4];
        scale[0]  =  (f32)s_res.width  * 0.5f;
        scale[1]  = -(f32)s_res.height * 0.5f;
        scale[2]  =  0.5f;
        scale[3]  =  0.0f;
        offset[0] =  (f32)s_res.width  * 0.5f;
        offset[1] =  (f32)s_res.height * 0.5f;
        offset[2] =  0.5f;
        offset[3] =  0.0f;
        rsxSetViewport(s_ctx, 0, 0, s_res.width, s_res.height, 0.0f, 1.0f, scale, offset);
    }
    rsxSetDepthTestEnable(s_ctx, GCM_FALSE);
    rsxSetDepthWriteEnable(s_ctx, GCM_FALSE);
    rsxSetCullFaceEnable(s_ctx, GCM_FALSE);
    rsxSetBlendEnable(s_ctx, GCM_FALSE);
    rsxInvalidateTextureCache(s_ctx, GCM_INVALIDATE_TEXTURE);
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
