/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * gpu_d3d360.cpp - the 360 side of the PSX libgpu, on the XDK's Direct3D.
 *
 * gpu_xbox.c owns the hardware-agnostic half of the renderer (ordering-table
 * walk, PSX primitive decode, screen transform) and compiles unchanged. This
 * file is only the hardware end: batch the vertices it produces, map PSX draw
 * state onto D3D state, and submit. Same interface and the same batching
 * rules as gpu_xenos.c (libXenon): a batch is flushed on any state change and
 * at frame end, because the PSX ordering table is painter-order.
 *
 * The shaders are the pair compiled for the libXenon build (shaders/vs.hlsl,
 * ps.hlsl through the XDK's fxc, embedded by shaders_embedded.c). fxc's output
 * is the 360 shader container D3D consumes directly; the vertex declaration
 * below binds the ShVertex layout to the HLSL semantics, which is what
 * Xe_ShaderApplyVFetchPatches did by patching the microcode.
 *
 * Geometry goes through DrawPrimitiveUP, which copies the vertices into the
 * command buffer: no vertex-buffer lifetime to manage, and the staging pool
 * can be reused the moment the call returns. Textures are the one resource
 * the GPU reads asynchronously, so they sit in a ring and a frame fence keeps
 * the CPU from overwriting a slot the previous frame still samples.
 */
#include <xtl.h>
#include <string.h>
#include <malloc.h>
#include <ppcintrinsics.h>

extern "C" {
#include "gpu_nv2a.h"
#include "sh_log.h"
#include "fs_xbox360.h"

extern unsigned char      g_ShaderVsEmbedded[];
extern const unsigned int g_ShaderVsEmbedded_len;
extern unsigned char      g_ShaderPsEmbedded[];
extern const unsigned int g_ShaderPsEmbedded_len;
const char* Sh360Fs_WritableRoot(void);

int                g_Nv2aFrameCount;
int                g_Nv2aContentX = 0;
int                g_Nv2aContentW = 640;
int                g_Nv2aContentH = 480;
unsigned long long g_Nv2aDrawCycles;
}

static void ApplyBlendState(void);
static void ApplyRenderState(void);
static void FlushBatch(void);

/* --- device ---------------------------------------------------------------- */

#define FB_W 1280
#define FB_H 720

static IDirect3D9*                  s_d3d;
static IDirect3DDevice9*            s_dev;
static IDirect3DVertexShader9*      s_vs;
static IDirect3DPixelShader9*       s_ps;
static IDirect3DVertexDeclaration9* s_vdecl;
static int                          s_ready;
static int                          s_warned;
/* The PSX's 4:3 content, letterboxed into the 16:9 framebuffer. */
static D3DVIEWPORT9                 s_vp;

/* --- per-frame counters ---------------------------------------------------- */

static unsigned s_frame;
static unsigned s_batchVertsThisFrame;
static unsigned s_batchCalls;
static unsigned s_drawsThisFrame;
static unsigned s_texBindsThisFrame;
static unsigned s_blendChanges;
static unsigned s_scissorChanges;

/* --- vertex staging -------------------------------------------------------- */

#define GPU_POOL_VERTS 32768
/* Per DrawPrimitiveUP call. 3072 x 64 B = 192 KB, comfortably inside what the
 * command buffer takes in one copy. */
#define GPU_CHUNK_VERTS 3072
static ShVertex s_pool[GPU_POOL_VERTS];
static int      s_poolUsed;
static int      s_flushStart;

/* --- PSX draw state (mirrored, applied at flush) --------------------------- */

static const void* s_texAddr;
static int         s_texW, s_texH;
static int         s_blendMode;                  /* 0 = opaque, 1..4 = PSX ABR */
static int         s_scX, s_scY, s_scW, s_scH;

/* --- textures -------------------------------------------------------------- */

#define GPU_TEX_MAX 256
#define TEX_RING    48
static IDirect3DTexture9* s_texRing[TEX_RING];
static int                s_texRingPos;
static int                s_texRingUsedThisFrame;
static IDirect3DTexture9* s_white;
static IDirect3DTexture9* s_curTex;
static float              s_texScale[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
static DWORD              s_frameFence;
static int                s_frameFenceValid;

/* ShVertex is pos[4], col[4], tex[2], spec[4], pad[2] -- 64 bytes. The pad is
 * not declared: DrawPrimitiveUP takes the stride explicitly. */
static const D3DVERTEXELEMENT9 s_vertexElements[] = {
    { 0,  0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0 },
    { 0, 32, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    { 0, 40, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
    D3DDECL_END()
};

static int FileExists(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* Escape hatch kept from the libXenon build: a file named "nogpu" beside the
 * disc image leaves D3D down, so the game runs in count-only mode. */
static int GpuDisabledByFile(void)
{
    char path[SH360_PATH_MAX * 2];
    _snprintf(path, sizeof(path), "%snogpu", Sh360Fs_DataRoot());
    path[sizeof(path) - 1] = '\0';
    if (!FileExists(path))
        return 0;
    SH_DBG("[GPU] '%s' present - leaving D3D down", path);
    return 1;
}

/* The embedded arrays are byte arrays; D3D wants the shader as DWORDs. */
static DWORD* AlignedShaderCopy(const unsigned char* src, unsigned len)
{
    DWORD* p = (DWORD*)_aligned_malloc((len + 3) & ~3u, 128);
    if (p)
        memcpy(p, src, len);
    return p;
}

static int LoadShaders(void)
{
    DWORD* vs = AlignedShaderCopy(g_ShaderVsEmbedded, g_ShaderVsEmbedded_len);
    DWORD* ps = AlignedShaderCopy(g_ShaderPsEmbedded, g_ShaderPsEmbedded_len);
    HRESULT hr;

    if (!vs || !ps) {
        SH_DBG("[GPU] shader copy allocation failed");
        return 0;
    }
    if (g_ShaderVsEmbedded[0] != 0x10 || g_ShaderVsEmbedded[1] != 0x2a ||
        g_ShaderPsEmbedded[0] != 0x10 || g_ShaderPsEmbedded[1] != 0x2a) {
        SH_DBG("[GPU] shader magic bad (want 102a) - not loading");
        return 0;
    }
    hr = s_dev->CreateVertexShader(vs, &s_vs);
    if (FAILED(hr) || !s_vs) {
        SH_DBG("[GPU] CreateVertexShader failed 0x%08x", (unsigned)hr);
        return 0;
    }
    hr = s_dev->CreatePixelShader(ps, &s_ps);
    if (FAILED(hr) || !s_ps) {
        SH_DBG("[GPU] CreatePixelShader failed 0x%08x", (unsigned)hr);
        return 0;
    }
    hr = s_dev->CreateVertexDeclaration(s_vertexElements, &s_vdecl);
    if (FAILED(hr) || !s_vdecl) {
        SH_DBG("[GPU] CreateVertexDeclaration failed 0x%08x", (unsigned)hr);
        return 0;
    }
    SH_DBG("[GPU] shaders loaded (vs=%u ps=%u bytes), declaration built",
           g_ShaderVsEmbedded_len, g_ShaderPsEmbedded_len);
    return 1;
}

static IDirect3DTexture9* CreateLinearTexture(int w, int h)
{
    IDirect3DTexture9* t = NULL;
    HRESULT hr = s_dev->CreateTexture(w, h, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &t, NULL);
    if (FAILED(hr) || !t) {
        SH_DBG("[GPU] CreateTexture %dx%d failed 0x%08x", w, h, (unsigned)hr);
        return NULL;
    }
    return t;
}

extern "C" void GpuNv2a_Init(void)
{
    D3DPRESENT_PARAMETERS pp;
    HRESULT               hr;
    int                   i;

    s_frame = 0;
    s_scX = s_scY = 0;
    s_scW = g_Nv2aContentW;
    s_scH = g_Nv2aContentH;

    if (GpuDisabledByFile())
        return;

    s_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!s_d3d) {
        SH_DBG("[GPU] Direct3DCreate9 returned NULL - count-only mode");
        return;
    }
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth        = FB_W;
    pp.BackBufferHeight       = FB_H;
    pp.BackBufferFormat       = D3DFMT_A8R8G8B8;
    pp.BackBufferCount        = 1;
    pp.EnableAutoDepthStencil = FALSE;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;
    hr = s_d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &s_dev);
    if (FAILED(hr) || !s_dev) {
        SH_DBG("[GPU] CreateDevice failed 0x%08x - count-only mode", (unsigned)hr);
        s_dev = NULL;
        return;
    }
    SH_DBG("[GPU] D3D up: backbuffer %dx%d", FB_W, FB_H);

    /* 4:3 content centred in the 16:9 buffer; the hardware scaler takes the
     * buffer to whatever mode the display is in. */
    s_vp.Height = FB_H;
    s_vp.Width  = FB_H * 4 / 3;
    s_vp.X      = (FB_W - s_vp.Width) / 2;
    s_vp.Y      = 0;
    s_vp.MinZ   = 0.0f;
    s_vp.MaxZ   = 1.0f;

    s_ready = LoadShaders();
    if (!s_ready) {
        SH_DBG("[GPU] shaders failed to load - count-only mode");
        return;
    }

    for (i = 0; i < TEX_RING; i++) {
        s_texRing[i] = CreateLinearTexture(GPU_TEX_MAX, GPU_TEX_MAX);
        if (!s_texRing[i]) {
            s_ready = 0;
            return;
        }
    }
    s_white = CreateLinearTexture(1, 1);
    if (!s_white) {
        s_ready = 0;
        return;
    }
    {   /* Untextured primitives sample this instead of whatever happened to be
         * bound, so their vertex colour comes through unmodified. */
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(s_white->LockRect(0, &lr, NULL, 0))) {
            *(unsigned*)lr.pBits = 0xFFFFFFFFu;
            s_white->UnlockRect(0);
        }
    }
    s_curTex = s_white;

    s_dev->SetVertexShader(s_vs);
    s_dev->SetPixelShader(s_ps);
    s_dev->SetVertexDeclaration(s_vdecl);
    /* PSX texturing is point-sampled. */
    s_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    s_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    s_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    s_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    ApplyRenderState();
    SH_DBG("[GPU] textures ready, renderer ARMED");
    SH_DBG("[GPU] d3d backend: %d-vertex pool, %d B/vertex, %d-texture ring",
           GPU_POOL_VERTS, (int)sizeof(ShVertex), TEX_RING);
}

/* Submit whatever has accumulated since the last flush under the CURRENT state.
 * Called before every state change so the batch that was built under the old
 * state is the batch that gets drawn with it. */
static void FlushBatch(void)
{
    int count = s_poolUsed - s_flushStart;

    if (count <= 0)
        return;
    if (!s_ready) {                 /* count-only mode: account and drop */
        s_flushStart = s_poolUsed;
        return;
    }

    {   /* Screen pixels -> clip space. Y is negated because PSX screen Y grows
         * downward. Recomputed per flush so a content-rect change cannot leave
         * a stale mapping behind. */
        float vp[4];
        vp[0] =  2.0f / (float)g_Nv2aContentW;
        vp[1] = -2.0f / (float)g_Nv2aContentH;
        vp[2] = -1.0f;
        vp[3] =  1.0f;
        s_dev->SetVertexShaderConstantF(0, vp, 1);
        s_dev->SetVertexShaderConstantF(1, s_texScale, 1);

        /* Output alpha = texel.a * x + y; see ps.hlsl. ABR0 on a TEXTURED
         * primitive uses the texel's own alpha (psx_vram encodes STP as 0x80
         * vs 0xFF); untextured takes the weight from the constant. */
        {
            float blend[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
            int   textured = (s_curTex && s_curTex != s_white);
            if (s_blendMode == 1) {
                if (textured) { blend[0] = 1.0f;  blend[1] = 0.0f;  }
                else          { blend[0] = 0.0f;  blend[1] = 0.5f;  }
            } else if (s_blendMode == 4) {
                blend[0] = 0.0f; blend[1] = 0.25f;
            }
            s_dev->SetPixelShaderConstantF(0, blend, 1);
        }
    }
    s_dev->SetTexture(0, s_curTex ? s_curTex : s_white);

    while (count > 0) {
        int n = count > GPU_CHUNK_VERTS ? GPU_CHUNK_VERTS : count;
        n -= n % 3;
        if (n <= 0)
            break;
        s_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n / 3, &s_pool[s_flushStart], sizeof(ShVertex));
        s_drawsThisFrame++;
        s_flushStart += n;
        count -= n;
    }
    s_flushStart = s_poolUsed;
}

/* --- selftest probes ------------------------------------------------------ */

static unsigned s_clearOverride;
static int      s_clearOverrideOn;

extern "C" void GpuXenos_DebugClearOverride(unsigned argb, int enable)
{
    s_clearOverride   = argb;
    s_clearOverrideOn = enable;
}

extern "C" void GpuXenos_DebugFbProbe(const char* tag)
{
    SH_DBG("[FBPROBE] %s: (no CPU readback on the D3D path) ready=%d frame=%u", tag, s_ready, s_frame);
}

extern "C" void GpuNv2a_FrameBegin(void)
{
    /* Doubles as psx_vram.c's texture-cache LRU clock, so it has to tick even
     * with no rendering behind it or every cached page looks equally stale. */
    g_Nv2aFrameCount++;
    s_poolUsed             = 0;
    s_flushStart           = 0;
    s_batchVertsThisFrame  = 0;
    s_batchCalls           = 0;
    s_drawsThisFrame       = 0;
    s_texBindsThisFrame    = 0;
    s_blendChanges         = 0;
    s_scissorChanges       = 0;
    s_texRingUsedThisFrame = 0;

    if (s_dev) {
        /* The texture ring is reused from here on; the previous frame must
         * have finished sampling it. */
        if (s_frameFenceValid) {
            s_dev->BlockOnFence(s_frameFence);
            s_frameFenceValid = 0;
        }
        s_dev->SetViewport(&s_vp);
        s_dev->Clear(0, NULL, D3DCLEAR_TARGET,
                     s_clearOverrideOn ? s_clearOverride : GpuXbox_GetClearColor(), 1.0f, 0);
        ApplyRenderState();
    }
}

extern "C" void GpuNv2a_FrameEnd(void)
{
    FlushBatch();

    if (s_dev) {
        s_dev->Present(NULL, NULL, NULL, NULL);
        s_frameFence      = s_dev->InsertFence();
        s_frameFenceValid = 1;
    }

    if ((s_frame % 60) == 0) {
        SH_DBG("[GPU] frame=%u prims=%u verts=%u draws=%u texbind=%u blend=%u scissor=%u ready=%d",
               s_frame, s_batchCalls, s_batchVertsThisFrame, s_drawsThisFrame,
               s_texBindsThisFrame, s_blendChanges, s_scissorChanges, s_ready);
    }
    s_frame++;
}

extern "C" void GpuNv2a_WaitVbl(void)
{
    /* Present at interval one already paces a live device; this keeps the loop
     * at 60 Hz when there is none, so timing-dependent behaviour in the log
     * stays meaningful. Resyncs rather than sprinting to catch up after a
     * blocking CD load. */
    static unsigned long long s_next;
    const unsigned long long  period = 50000000ULL / 60;
    unsigned long long        now    = __mftb();

    if (s_next == 0 || now > s_next + period * 8)
        s_next = now;
    s_next += period;
    while (__mftb() < s_next) { }
}

extern "C" void GpuNv2a_EmitTris(const ShVertex* verts, int count)
{
    ShVertex* d;
    if (!verts || count <= 0)
        return;
    d = GpuNv2a_BatchAlloc(count);
    if (d)
        memcpy(d, verts, (size_t)count * sizeof(ShVertex));
}

extern "C" ShVertex* GpuNv2a_BatchAlloc(int count)
{
    ShVertex* p;

    if (count <= 0 || count > GPU_POOL_VERTS)
        return NULL;
    if (s_poolUsed + count > GPU_POOL_VERTS) {
        FlushBatch();
        s_poolUsed = s_flushStart = 0;
    }
    p = &s_pool[s_poolUsed];
    s_poolUsed += count;
    s_batchVertsThisFrame += (unsigned)count;
    s_batchCalls++;
    return p;
}

/* Composed ARGB copy of the current paletted page. static, not stack: 256KB. */
static unsigned s_palComposed[GPU_TEX_MAX * GPU_TEX_MAX];

extern "C" void GpuNv2a_BindPaletted(const void* page, const void* palette)
{
    /* No palette stage in this backend: compose CPU-side. The page is 256x256
     * 8-bit indices (linear on this port) and the palette 256 ARGB words. */
    const unsigned char* idx = (const unsigned char*)page;
    const unsigned*      pal = (const unsigned*)palette;
    int i;

    if (!page || !palette) {
        GpuNv2a_BindWhite();
        return;
    }
    for (i = 0; i < GPU_TEX_MAX * GPU_TEX_MAX; i++)
        s_palComposed[i] = pal[idx[i]];

    s_texAddr = NULL;   /* force the upload: same address, new pixels */
    GpuNv2a_BindTexture(s_palComposed, GPU_TEX_MAX, GPU_TEX_MAX);
    /* gpu_xbox.c pre-scales the UVs of paletted pages to 0..1 (PAL_UV_SCALE,
     * an NV2A palette-texture convention), so the shader must NOT normalise
     * them again. Scaling twice samples texel (0,0) for every paletted
     * primitive -- logos invisible, world black -- which is what the first
     * pictures from both backends showed. */
    s_texScale[0] = 1.0f;
    s_texScale[1] = 1.0f;
}

extern "C" void GpuNv2a_BindTexture(const void* addr, int w, int h)
{
    IDirect3DTexture9* t;
    D3DLOCKED_RECT     lr;

    if (addr == s_texAddr && w == s_texW && h == s_texH)
        return;                     /* redundant bind: do not break the batch */
    FlushBatch();
    s_texAddr = addr;
    s_texW = w;
    s_texH = h;
    s_texBindsThisFrame++;

    if (!s_ready || !addr || w <= 0 || h <= 0) {
        s_curTex = s_white;
        return;
    }
    if (w > GPU_TEX_MAX || h > GPU_TEX_MAX) {
        static int s_told;
        if (!s_told) { s_told = 1; SH_DBG("[GPU] texture %dx%d exceeds %d, using white",
                                          w, h, GPU_TEX_MAX); }
        s_curTex = s_white;
        return;
    }

    /* Ring wrap inside one frame means a slot this frame already drew with
     * is about to be overwritten: drain first. Logged once because it is a
     * performance fact worth knowing, not an error. */
    if (s_texRingUsedThisFrame >= TEX_RING) {
        static int s_toldWrap;
        if (!s_toldWrap) { s_toldWrap = 1; SH_DBG("[GPU] texture ring wrapped in one frame (%d binds)", s_texRingUsedThisFrame + 1); }
        s_dev->BlockUntilIdle();
        s_texRingUsedThisFrame = 0;
    }
    t = s_texRing[s_texRingPos];
    s_texRingPos = (s_texRingPos + 1) % TEX_RING;
    s_texRingUsedThisFrame++;

    /* Diagnostic: the first pages bound after the self-test go to disk as raw
     * ARGB, so the PC can show exactly what the sampler was given. Cheap and
     * bounded; reads back with tools/argb2png.py. */
    {
        static int s_dumped;
        if (w == GPU_TEX_MAX && s_dumped < 24) {
            char  path[96];
            FILE* f;
            _snprintf(path, sizeof(path), "%stex_%02d_%dx%d_f%u.argb", Sh360Fs_WritableRoot(), s_dumped, w, h, s_frame);
            path[sizeof(path) - 1] = 0;
            f = fopen(path, "wb");
            if (f) {
                fwrite(addr, 4, (size_t)w * h, f);
                fclose(f);
            }
            s_dumped++;
        }
    }

    if (SUCCEEDED(t->LockRect(0, &lr, NULL, 0))) {
        const unsigned* src = (const unsigned*)addr;
        unsigned char*  dst = (unsigned char*)lr.pBits;
        int y;
        for (y = 0; y < h; y++)
            memcpy(dst + (size_t)y * lr.Pitch, src + (size_t)y * w, (size_t)w * 4);
        t->UnlockRect(0);
    }
    /* UVs arrive in TEXELS; the shader normalises them against the surface's
     * real width -- GPU_TEX_MAX, not w, because that is the surface sampled. */
    s_texScale[0] = 1.0f / (float)GPU_TEX_MAX;
    s_texScale[1] = 1.0f / (float)GPU_TEX_MAX;
    s_curTex = t;
}

extern "C" void GpuNv2a_BindWhite(void)
{
    if (!s_texAddr && s_curTex == s_white)
        return;
    FlushBatch();
    s_texAddr = NULL;
    s_curTex  = s_white;
    s_texScale[0] = 0.0f;
    s_texScale[1] = 0.0f;
}

/* psx_vram.c really uses this memory, so it must be a genuine allocation. */
extern "C" void* GpuNv2a_AllocTexMem(int bytes)
{
    void* p;
    if (bytes <= 0)
        return NULL;
    p = _aligned_malloc((size_t)bytes, 128);
    if (!p)
        SH_DBG("[GPU] AllocTexMem(%d) FAILED", bytes);
    return p;
}

/* PSX semi-transparency: 0 = off, then ABR 0..3 =
 *   0.5B+0.5F, B+F, B-F, B+0.25F.
 * The two fractional modes carry their weight as SOURCE ALPHA from the pixel
 * shader (FlushBatch sets it), so the pair are always chosen together. ABR 2
 * is a REVERSE-SUBTRACT: background minus foreground. */
static void ApplyBlendState(void)
{
    if (!s_dev)
        return;
    switch (s_blendMode) {
    case 0:
        s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        break;
    case 1:
        s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        s_dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
        s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        s_dev->SetRenderState(D3DRS_BLENDOP,   D3DBLENDOP_ADD);
        break;
    case 2:
        s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        s_dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_ONE);
        s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        s_dev->SetRenderState(D3DRS_BLENDOP,   D3DBLENDOP_ADD);
        break;
    case 3:
        s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        s_dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_ONE);
        s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        s_dev->SetRenderState(D3DRS_BLENDOP,   D3DBLENDOP_REVSUBTRACT);
        break;
    case 4:
        s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        s_dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
        s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        s_dev->SetRenderState(D3DRS_BLENDOP,   D3DBLENDOP_ADD);
        break;
    default:
        break;
    }
}

/* Everything the device needs that the game does not re-send every frame. */
static void ApplyRenderState(void)
{
    if (!s_dev)
        return;
    /* Depth comes from the ordering table (painter order), not a Z test. */
    s_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    ApplyBlendState();

    /* The scissor is in FRAMEBUFFER pixels; the game's rect is in PSX content
     * pixels (640x480). Scale into the letterboxed viewport. */
    if (g_Nv2aContentW > 0 && g_Nv2aContentH > 0) {
        RECT r;
        r.left   = (LONG)s_vp.X + s_scX * (LONG)s_vp.Width  / g_Nv2aContentW;
        r.top    = (LONG)s_vp.Y + s_scY * (LONG)s_vp.Height / g_Nv2aContentH;
        r.right  = r.left + s_scW * (LONG)s_vp.Width  / g_Nv2aContentW;
        r.bottom = r.top  + s_scH * (LONG)s_vp.Height / g_Nv2aContentH;
        s_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        s_dev->SetScissorRect(&r);
    } else {
        s_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    }
}

extern "C" void GpuNv2a_SetBlendMode(int mode)
{
    if (mode == s_blendMode)
        return;
    FlushBatch();
    s_blendMode = mode;
    s_blendChanges++;
    ApplyBlendState();
}

extern "C" void GpuNv2a_SetScissor(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {          /* reset to the full content rect */
        x = g_Nv2aContentX; y = 0; w = g_Nv2aContentW; h = g_Nv2aContentH;
    }
    if (x == s_scX && y == s_scY && w == s_scW && h == s_scH)
        return;
    FlushBatch();
    s_scX = x; s_scY = y; s_scW = w; s_scH = h;
    s_scissorChanges++;
    ApplyRenderState();
}

/* Readback needs a resolved copy of the framebuffer. Reporting NULL makes
 * callers draw the scene live instead of blitting a buffer that was never
 * filled. */
extern "C" const void* GpuNv2a_ReadbackSurface(int fromLastQueued, int* w, int* h, int* pitchBytes)
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

extern "C" int GpuNv2a_Ms(void) { return 0; }

extern "C" void GpuNv2a_DrainGpu(void)
{
    FlushBatch();
    if (s_dev)
        s_dev->BlockUntilIdle();
}

extern "C" void GpuNv2a_SetDepthWrite(int enable)
{
    FlushBatch();
    if (s_dev)
        s_dev->SetRenderState(D3DRS_ZWRITEENABLE, enable ? TRUE : FALSE);
}

extern "C" void GpuNv2a_SetPaletteDmaVariant(int variant) { (void)variant; }

/* Freeze-frame capture (pause/save backgrounds). 0 = "no frame captured". */
extern "C" int  GpuNv2a_FreezeCapture(void) { return 0; }
extern "C" void GpuNv2a_FreezeBlit(void)    { }
extern "C" void GpuNv2a_FreezeRelease(void) { }
