/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gpu_xenos.c - the 360 side of the PSX libgpu, replacing gpu_nv2a.c.
 *
 * gpu_xbox.c owns the hardware-agnostic half of the renderer (ordering-table
 * walk, PSX primitive decode, screen transform) and compiles for PPC unchanged.
 * This file is only the hardware end: batch the vertices it produces, map PSX
 * draw state onto Xenos state, and submit.
 *
 * SHADERS ARE LOADED FROM DISK, not linked in. libXenon consumes compiled Xenos
 * microcode (.vsu/.psu) and the documented way to produce it -- Tser's
 * XNA-based compiler -- needs Visual Studio 2008 plus XNA 3.0, and its download
 * has been gone for years. The surviving open tools (XenosRecomp, Xenia) all run
 * the other way, microcode -> HLSL. So the microcode is treated as an asset:
 * drop vs.vsu / ps.psu next to the disc image and the renderer comes up. That
 * keeps the whole backend testable and swappable the moment working shaders
 * exist, from whatever source, without touching this code.
 *
 * With no shader files present everything below degrades to the counting no-op
 * the previous phase used, and says so once in the log. That is deliberate: a
 * missing asset must not turn into a crash or a silently black screen with no
 * explanation.
 */
#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include <xenos/xe.h>
#include <xenos/xenos.h>
#include <console/console.h>

#include "gpu_nv2a.h"
#include "sh_log.h"
#include "sh_hwperf.h"
#include "fs_xbox360.h"

/* --- device ---------------------------------------------------------------- */

static struct XenosDevice  s_xeDev;
static struct XenosDevice* s_xe;                 /* NULL until Init succeeds */
static struct XenosSurface* s_fb;
static struct XenosShader*  s_vs;
static struct XenosShader*  s_ps;
static int                  s_ready;             /* device AND shaders up */
static int                  s_warned;

/* --- per-frame counters ---------------------------------------------------- */

static unsigned s_frame;
static unsigned s_batchVertsThisFrame;
static unsigned s_batchCalls;
static unsigned s_drawsThisFrame;
static unsigned s_texBindsThisFrame;
static unsigned s_blendChanges;
static unsigned s_scissorChanges;

/* --- vertex staging -------------------------------------------------------- */

/* gpu_xbox.c reserves vertices through BatchAlloc and writes them in place, so
 * this must be real writable memory at the ShVertex stride whether or not a GPU
 * is behind it. Batches are flushed on any state change and at frame end, which
 * is what keeps draw order correct -- the PSX ordering table is painter-order,
 * so a primitive drawn under the wrong blend or texture is not merely wrong, it
 * is wrong in a way that looks like a depth bug. */
#define GPU_POOL_VERTS 32768
static ShVertex s_pool[GPU_POOL_VERTS];
static int      s_poolUsed;
static int      s_flushStart;                    /* first unsubmitted vertex */

/* --- PSX draw state (mirrored, applied at flush) --------------------------- */

static const void* s_texAddr;
static int         s_texW, s_texH;
static int         s_blendMode;                  /* 0 = opaque, 1..4 = PSX ABR */
static int         s_scX, s_scY, s_scW, s_scH;

int                g_Nv2aFrameCount;
int                g_Nv2aContentX = 0;
int                g_Nv2aContentW = 640;
int                g_Nv2aContentH = 480;
unsigned long long g_Nv2aDrawCycles;

/* ShVertex is pos[4], col[4], tex[2], spec[4], pad[2] -- 64 bytes. The pad
 * exists for NV2A write-combining and is harmless here; the stride comes from
 * sizeof(ShVertex) so it stays correct either way. */
static const struct XenosVBFFormat s_vbf = {
    4,
    {
        { XE_USAGE_POSITION, 0, XE_TYPE_FLOAT4 },
        { XE_USAGE_NORMAL,   0, XE_TYPE_FLOAT4 },   /* carries diffuse colour */
        { XE_USAGE_TEXCOORD, 0, XE_TYPE_FLOAT2 },
        { XE_USAGE_TEXCOORD, 1, XE_TYPE_FLOAT4 },   /* carries fog/specular   */
    }
};

static void* LoadFile(const char* path, int* sizeOut)
{
    FILE* f = fopen(path, "rb");
    long  n;
    void* buf;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }

    buf = memalign(128, (size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    if (sizeOut) *sizeOut = (int)n;
    return buf;
}

static int LoadShaders(void)
{
    char  path[SH360_PATH_MAX * 2];
    void* vsBlob;
    void* psBlob;

    snprintf(path, sizeof(path), "%svs.vsu", Sh360Fs_DataRoot());
    vsBlob = LoadFile(path, NULL);
    if (!vsBlob) { SH_DBG("[GPU] no vertex shader at '%s'", path); return 0; }

    snprintf(path, sizeof(path), "%sps.psu", Sh360Fs_DataRoot());
    psBlob = LoadFile(path, NULL);
    if (!psBlob) { SH_DBG("[GPU] no pixel shader at '%s'", path); free(vsBlob); return 0; }

    s_ps = Xe_LoadShaderFromMemory(s_xe, psBlob);
    s_vs = Xe_LoadShaderFromMemory(s_xe, vsBlob);
    if (!s_ps || !s_vs) {
        SH_DBG("[GPU] shader load FAILED (vs=%p ps=%p)", (void*)s_vs, (void*)s_ps);
        return 0;
    }
    Xe_InstantiateShader(s_xe, s_ps, 0);
    Xe_InstantiateShader(s_xe, s_vs, 0);
    /* Patches the vertex-fetch instructions to match our layout, which is why
     * the shader's declared input order does not have to match ours. */
    Xe_ShaderApplyVFetchPatches(s_xe, s_vs, 0, &s_vbf);
    SH_DBG("[GPU] shaders loaded and instantiated");
    return 1;
}

void GpuNv2a_Init(void)
{
    s_frame = 0;
    s_scX = s_scY = 0;
    s_scW = g_Nv2aContentW;
    s_scH = g_Nv2aContentH;

    s_xe = &s_xeDev;
    memset(s_xe, 0, sizeof(*s_xe));
    Xe_Init(s_xe);

    s_fb = Xe_GetFramebufferSurface(s_xe);
    if (!s_fb) {
        SH_DBG("[GPU] Xe_GetFramebufferSurface returned NULL - staying in count-only mode");
        s_xe = NULL;
        return;
    }
    Xe_SetRenderTarget(s_xe, s_fb);
    SH_DBG("[GPU] Xe up: framebuffer %dx%d", s_fb->width, s_fb->height);

    s_ready = LoadShaders();
    if (!s_ready) {
        SH_DBG("[GPU] COUNT-ONLY MODE: put vs.vsu and ps.psu next to the disc "
               "image to enable rendering (see gpu_xenos.c header)");
    }
    SH_DBG("[GPU] xenos backend: %d-vertex pool, %d B/vertex",
           GPU_POOL_VERTS, (int)sizeof(ShVertex));
}

/* Submit whatever has accumulated since the last flush under the CURRENT state.
 * Called before every state change so the batch that was built under the old
 * state is the batch that gets drawn with it. */
static void FlushBatch(void)
{
    int count = s_poolUsed - s_flushStart;
    struct XenosVertexBuffer* vb;
    void* dst;

    if (count <= 0)
        return;
    if (!s_ready) {                 /* count-only mode: account and drop */
        s_flushStart = s_poolUsed;
        return;
    }

    vb = Xe_CreateVertexBuffer(s_xe, count * (int)sizeof(ShVertex));
    if (!vb) {
        SH_DBG("[GPU] Xe_CreateVertexBuffer(%d verts) failed", count);
        s_flushStart = s_poolUsed;
        return;
    }
    dst = Xe_VB_Lock(s_xe, vb, 0, count * (int)sizeof(ShVertex), XE_LOCK_WRITE);
    if (dst) {
        memcpy(dst, &s_pool[s_flushStart], (size_t)count * sizeof(ShVertex));
        Xe_VB_Unlock(s_xe, vb);

        Xe_SetShader(s_xe, SHADER_TYPE_PIXEL,  s_ps, 0);
        Xe_SetShader(s_xe, SHADER_TYPE_VERTEX, s_vs, 0);
        Xe_SetStreamSource(s_xe, 0, vb, 0, (int)sizeof(ShVertex));
        if (s_texAddr)
            Xe_SetTexture(s_xe, 0, (struct XenosSurface*)s_texAddr);
        Xe_DrawPrimitive(s_xe, XE_PRIMTYPE_TRIANGLELIST, 0, count / 3);
        s_drawsThisFrame++;
    }
    /* Returned to the pool rather than freed: the GPU may still be reading it,
     * and the pool defers reuse until the frame has drained. */
    Xe_VBPoolAdd(s_xe, vb);
    s_flushStart = s_poolUsed;
}

void GpuNv2a_FrameBegin(void)
{
    /* Doubles as psx_vram.c's texture-cache LRU clock, so it has to tick even
     * with no rendering behind it or every cached page looks equally stale. */
    g_Nv2aFrameCount++;
    s_poolUsed            = 0;
    s_flushStart          = 0;
    s_batchVertsThisFrame = 0;
    s_batchCalls          = 0;
    s_drawsThisFrame      = 0;
    s_texBindsThisFrame   = 0;
    s_blendChanges        = 0;
    s_scissorChanges      = 0;

    if (s_xe) {
        Xe_InvalidateState(s_xe);
        Xe_SetClearColor(s_xe, GpuXbox_GetClearColor());
    }
}

void GpuNv2a_FrameEnd(void)
{
    FlushBatch();

    if (s_xe) {
        Xe_Resolve(s_xe);            /* EDRAM tile -> framebuffer */
        Xe_Sync(s_xe);
    }

    if ((s_frame % 60) == 0) {
        /* verts/prims come from BatchAlloc, which is the path gpu_xbox.c
         * actually uses. An earlier version counted triangles in EmitTris --
         * which NOTHING calls -- so it read a constant zero and looked like a
         * rendering failure when it was a dead counter. */
        SH_DBG("[GPU] frame=%u prims=%u verts=%u draws=%u texbind=%u blend=%u scissor=%u ready=%d",
               s_frame, s_batchCalls, s_batchVertsThisFrame, s_drawsThisFrame,
               s_texBindsThisFrame, s_blendChanges, s_scissorChanges, s_ready);
    }
    s_frame++;
}

void GpuNv2a_WaitVbl(void)
{
    /* Xe_Sync in FrameEnd already blocks until the GPU drains once a device is
     * up. Without one there is nothing to block on, and returning immediately
     * let the loop free-run to TEN MILLION frames in a session, which makes
     * every timing-dependent behaviour in the log meaningless. */
    static unsigned long long s_next;
    const unsigned long long  period = PPC_TIMEBASE_FREQ / 60;
    unsigned long long        now    = mftb();

    /* Resync rather than sprint to catch up: a blocking CD load stalls the loop
     * for many frames, and replaying them at full speed is worse than dropping. */
    if (s_next == 0 || now > s_next + period * 8)
        s_next = now;

    s_next += period;
    while (mftb() < s_next) { }
}

void GpuNv2a_EmitTris(const ShVertex* verts, int count)
{
    /* gpu_xbox.c does not use this path -- it writes through BatchAlloc -- but
     * the entry point is part of the interface, so honour it rather than
     * silently dropping geometry if a caller ever appears. */
    ShVertex* d;
    if (!verts || count <= 0)
        return;
    d = GpuNv2a_BatchAlloc(count);
    if (d)
        memcpy(d, verts, (size_t)count * sizeof(ShVertex));
}

ShVertex* GpuNv2a_BatchAlloc(int count)
{
    ShVertex* p;

    if (count <= 0 || count > GPU_POOL_VERTS)
        return NULL;
    if (s_poolUsed + count > GPU_POOL_VERTS) {
        FlushBatch();               /* submit what we have, then reuse the pool */
        s_poolUsed = s_flushStart = 0;
    }

    p = &s_pool[s_poolUsed];
    s_poolUsed += count;
    s_batchVertsThisFrame += (unsigned)count;
    s_batchCalls++;
    return p;
}

void GpuNv2a_BindPaletted(const void* page, const void* palette)
{
    (void)page; (void)palette;
    /* The paletted path decodes through psx_vram's cache before reaching us, so
     * there is nothing to bind separately until that cache hands over a
     * XenosSurface. Counted so the log still shows the path is exercised. */
    s_texBindsThisFrame++;
}

void GpuNv2a_BindTexture(const void* addr, int w, int h)
{
    if (addr == s_texAddr && w == s_texW && h == s_texH)
        return;                     /* redundant bind: do not break the batch */
    FlushBatch();
    s_texAddr = addr;
    s_texW = w;
    s_texH = h;
    s_texBindsThisFrame++;
}

void GpuNv2a_BindWhite(void)
{
    if (!s_texAddr)
        return;
    FlushBatch();
    s_texAddr = NULL;
}

/* psx_vram.c really uses this memory, so it must be a genuine allocation.
 * 128-byte aligned because the GPU will DMA out of these buffers. */
void* GpuNv2a_AllocTexMem(int bytes)
{
    void* p;
    if (bytes <= 0)
        return NULL;
    p = memalign(128, (size_t)bytes);
    if (!p)
        SH_DBG("[GPU] AllocTexMem(%d) FAILED", bytes);
    return p;
}

/* PSX semi-transparency: 0 = off, then ABR 0..3 =
 *   0.5B+0.5F, B+F, B-F, B+0.25F.
 * ABR 2 is a REVERSE-SUBTRACT, not a plain subtract: the PSX computes
 * background minus foreground, so getting the operand order backwards inverts
 * every shadow and smoke effect in the game rather than merely dimming it. */
void GpuNv2a_SetBlendMode(int mode)
{
    if (mode == s_blendMode)
        return;
    FlushBatch();
    s_blendMode = mode;
    s_blendChanges++;

    if (!s_xe)
        return;

    switch (mode) {
    case 0:
        Xe_SetBlendControl(s_xe, XE_BLEND_ONE, XE_BLENDOP_ADD, XE_BLEND_ZERO,
                                 XE_BLEND_ONE, XE_BLENDOP_ADD, XE_BLEND_ZERO);
        break;
    /* SHADER CONTRACT for the two fractional modes: libXenon exposes no way to
     * set a blend constant (there is no Xe_SetBlendFactor, and BLENDFACTOR /
     * CONSTANTALPHA would need one), so the 0.5 and 0.25 weights have to arrive
     * as SOURCE ALPHA from the pixel shader. The shader must therefore emit
     * alpha = 0.5 for mode 1 and 0.25 for mode 4, not the texture's alpha.
     * Getting this wrong does not fail loudly -- it just makes every
     * semi-transparent surface the wrong strength. */
    case 1:  /* 0.5*B + 0.5*F */
        Xe_SetBlendControl(s_xe, XE_BLEND_SRCALPHA, XE_BLENDOP_ADD, XE_BLEND_INVSRCALPHA,
                                 XE_BLEND_ONE, XE_BLENDOP_ADD, XE_BLEND_ZERO);
        break;
    case 2:  /* B + F (additive) */
        Xe_SetBlendControl(s_xe, XE_BLEND_ONE, XE_BLENDOP_ADD, XE_BLEND_ONE,
                                 XE_BLEND_ONE, XE_BLENDOP_ADD, XE_BLEND_ZERO);
        break;
    case 3:  /* B - F. REVSUBTRACT is dst - src, which is the PSX's
              * background-minus-foreground. Plain SUBTRACT would invert every
              * shadow and smoke effect rather than merely dimming it. */
        Xe_SetBlendControl(s_xe, XE_BLEND_ONE, XE_BLENDOP_REVSUBTRACT, XE_BLEND_ONE,
                                 XE_BLEND_ONE, XE_BLENDOP_ADD, XE_BLEND_ZERO);
        break;
    case 4:  /* B + 0.25*F -- see the shader contract above. */
        Xe_SetBlendControl(s_xe, XE_BLEND_SRCALPHA, XE_BLENDOP_ADD, XE_BLEND_ONE,
                                 XE_BLEND_ONE, XE_BLENDOP_ADD, XE_BLEND_ZERO);
        break;
    default:
        break;
    }
}

void GpuNv2a_SetScissor(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {          /* reset to the full content rect */
        x = g_Nv2aContentX; y = 0; w = g_Nv2aContentW; h = g_Nv2aContentH;
    }
    if (x == s_scX && y == s_scY && w == s_scW && h == s_scH)
        return;
    FlushBatch();
    s_scX = x; s_scY = y; s_scW = w; s_scH = h;
    s_scissorChanges++;
    if (s_xe)
        Xe_SetScissor(s_xe, 1, x, y, x + w, y + h);
}

/* Readback needs a resolved copy of the framebuffer, which the count-only path
 * has no way to produce. Reporting NULL makes callers draw the scene live
 * instead of blitting a buffer that was never filled, which would show as a
 * black pane rather than a missing effect. */
const void* GpuNv2a_ReadbackSurface(int fromLastQueued, int* w, int* h, int* pitchBytes)
{
    (void)fromLastQueued;
    if (w) *w = 0;
    if (h) *h = 0;
    if (pitchBytes) *pitchBytes = 0;
    if (!s_warned) {
        s_warned = 1;
        SH_DBG("[GPU] ReadbackSurface unavailable (no resolve target yet)");
    }
    return NULL;
}

int GpuNv2a_Ms(void) { return 0; }

/* The GpuXbox_Fb* entry points are NOT defined here: gpu_xbox.c owns them (it
 * holds the draw/display envs and the screen transform), and it is compiled into
 * this build unchanged. */

void GpuNv2a_DrainGpu(void)
{
    FlushBatch();
    if (s_xe)
        Xe_Sync(s_xe);
}

void GpuNv2a_SetDepthWrite(int enable)
{
    FlushBatch();
    if (s_xe)
        Xe_SetZWrite(s_xe, enable ? 1 : 0);
}

void GpuNv2a_SetPaletteDmaVariant(int variant) { (void)variant; }

/* Freeze-frame capture (pause/save backgrounds). 0 = "no frame captured", so
 * callers fall back to drawing the scene live instead of blitting a buffer that
 * was never filled. */
int  GpuNv2a_FreezeCapture(void) { return 0; }
void GpuNv2a_FreezeBlit(void)    { }
void GpuNv2a_FreezeRelease(void) { }
