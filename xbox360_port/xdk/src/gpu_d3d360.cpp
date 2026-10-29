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
#include <xgraphics.h>
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
/* Visible width in content units (640 = 4:3, 853 = Hor+ 16:9); gpu_xbox.c
 * sizes its oversize-primitive rejection by it. */
int                g_Nv2aViewW    = 640;
unsigned long long g_Nv2aDrawCycles;
extern int g_PcWidescreenMode;    /* xbox_pcfeature_stubs.c: cfg widescreen_mode */
extern int g_PcHorPlusEnabled;    /* xbox_compat_globals.c, driven by game_main.c */
}

static void ApplyBlendState(void);
static void ApplyRenderState(void);
static void FlushBatch(void);
static void FreezeCaptureBeforePresent(void);
extern "C" void Pgxp_FrameAdvance(void);

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
/* The PSX's 4:3 content, letterboxed into the 16:9 framebuffer -- or, with
 * widescreen_mode 1 on an in-game frame (Hor+), the whole 1280x720 with the
 * 4:3 content centred at the same scale and the GTE's off-frame geometry
 * filling the sides. s_viewW/s_viewX are that mapping in content units:
 * clip x = (x + s_viewX) * 2 / s_viewW - 1. */
static D3DVIEWPORT9                 s_vp;
static float                        s_viewW = 640.0f;
static float                        s_viewX = 0.0f;
static int                          s_horPlus;

static void SelectView(void)
{
    const int want = (g_PcWidescreenMode == 1 && g_PcHorPlusEnabled) ? 1 : 0;
    if (want == s_horPlus && s_vp.Width)
        return;
    s_horPlus = want;
    if (want) {
        s_vp.X = 0; s_vp.Y = 0; s_vp.Width = FB_W; s_vp.Height = FB_H;
        s_viewW = (float)g_Nv2aContentH * (float)FB_W / (float)FB_H;   /* 853.33 */
        s_viewX = (s_viewW - (float)g_Nv2aContentW) * 0.5f;            /* 106.67 */
    } else {
        s_vp.Height = FB_H;
        s_vp.Width  = FB_H * 4 / 3;
        s_vp.X      = (FB_W - s_vp.Width) / 2;
        s_vp.Y      = 0;
        s_viewW = (float)g_Nv2aContentW;
        s_viewX = 0.0f;
    }
    s_vp.MinZ = 0.0f; s_vp.MaxZ = 1.0f;
    g_Nv2aViewW = (int)(s_viewW + 0.5f);
    SH_DBG("[GPU] view: %s (viewport %ux%u at %u, content %.1f wide, x offset %.1f)",
           want ? "Hor+ 16:9" : "4:3 pillarbox", (unsigned)s_vp.Width, (unsigned)s_vp.Height,
           (unsigned)s_vp.X, s_viewW, s_viewX);
}

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
static int         s_scFull = 1;              /* 1 = no clip: the whole viewport (Hor+ sides too) */

/* --- textures -------------------------------------------------------------- */

#define GPU_TEX_MAX 256
/* One ATLAS instead of one texture per page: 4096x2048 A8R8G8B8 (32 MB) =
 * 16x8 slots of 256x256. Slot 0 is solid white (alpha 0x80, see FlushBatch);
 * the other 127 cache decoded pages keyed by the psx_vram.c page/palette
 * pointers and their decode generations (PsxVram_DataGen). A bind no longer
 * breaks the batch: it records a SEGMENT {first vertex, slot origin, uv
 * scale} and FlushBatch rewrites that segment's UVs into atlas texels before
 * one DrawPrimitiveUP covers every texture. Hardware run 024 paid ~830
 * DrawPrimitiveUP calls a frame -- one per texture change, the depth sort
 * interleaves pages -- which was 17 of the 22 ms ordering-table walk for
 * ~1550 primitives. Only blend-mode and scissor changes split batches now. */
#define ATLAS_W     4096
#define ATLAS_H     2048
#define ATLAS_COLS  (ATLAS_W / GPU_TEX_MAX)
#define ATLAS_ROWS  (ATLAS_H / GPU_TEX_MAX)
#define TEX_CACHE_N (ATLAS_COLS * ATLAS_ROWS - 1)   /* 127; slot 0 = white */
struct TexCacheEnt {
    int         slot;      /* 1..TEX_CACHE_N */
    const void* page;      /* key: decoded page (indices or ARGB); NULL = empty */
    const void* pal;       /* key: palette, NULL for ARGB pages */
    unsigned    pageGen, palGen;
    unsigned    epoch;     /* PsxVram_DecodeEpoch() when the gens were last verified */
    unsigned    lastUse;   /* g_Nv2aFrameCount of the last bind */
};
static TexCacheEnt        s_texCache[TEX_CACHE_N];
static unsigned char      s_texHint[256];           /* key hash -> cache index + 1 */
static IDirect3DTexture9* s_atlas;
static int                s_texRingUsedThisFrame;   /* uploads this frame */
static const void*        s_texPal;                 /* palette of the current bind */
extern "C" unsigned PsxVram_DataGen(const void* data);
extern "C" unsigned PsxVram_DecodeEpoch(void);
static IDirect3DTexture9* s_white;                  /* count-only fallback */
static IDirect3DTexture9* s_curTex;
static float              s_texScale[4] = { 1.0f, 1.0f, 0.0f, 0.0f };

/* The current bind in atlas terms: texel origin of its slot and the factor
 * that takes the incoming UV to texels (1 = texels already, 256 = the 0..1
 * pre-scaled paletted UVs, 0 = white: every vertex samples the slot centre). */
static float s_bindU, s_bindV, s_bindScale;
struct Seg { int start; float u, v, scale; };
#define SEG_MAX 4096
static Seg s_segs[SEG_MAX];
static int s_segCount;

/* Oversize binds (the 320x240 FMV frame) bypass the atlas: two textures
 * alternate so an upload never lands on the one the previous present reads. */
static IDirect3DTexture9* s_big[2];
static int                s_bigW[2], s_bigH[2], s_bigNext;
static DWORD              s_frameFence;
static int                s_frameFenceValid;

/* Start a fresh segment list holding only the current bind, at s_poolUsed. */
static void SegReset(void)
{
    s_segs[0].start = s_poolUsed;
    s_segs[0].u     = s_bindU;
    s_segs[0].v     = s_bindV;
    s_segs[0].scale = s_bindScale;
    s_segCount      = 1;
}

/* Record that vertices appended from here on sample the current bind. */
static void SegMark(void)
{
    Seg* last;
    if (s_segCount == 0) { SegReset(); return; }
    last = &s_segs[s_segCount - 1];
    if (last->start == s_poolUsed) {        /* nothing appended since: replace */
        last->u = s_bindU; last->v = s_bindV; last->scale = s_bindScale;
        return;
    }
    if (s_segCount >= SEG_MAX) {
        FlushBatch();                       /* resets the list to the current bind */
        return;
    }
    last = &s_segs[s_segCount++];
    last->start = s_poolUsed;
    last->u = s_bindU; last->v = s_bindV; last->scale = s_bindScale;
}

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

    /* 4:3 content centred in the 16:9 buffer (or Hor+, see SelectView); the
     * hardware scaler takes the buffer to whatever mode the display is in. */
    SelectView();

    s_ready = LoadShaders();
    if (!s_ready) {
        SH_DBG("[GPU] shaders failed to load - count-only mode");
        return;
    }

    s_atlas = CreateLinearTexture(ATLAS_W, ATLAS_H);
    if (!s_atlas) {
        s_ready = 0;
        return;
    }
    for (i = 0; i < TEX_CACHE_N; i++) {
        s_texCache[i].slot = i + 1;
        s_texCache[i].page = NULL; s_texCache[i].pal = NULL;
        s_texCache[i].pageGen = s_texCache[i].palGen = 0;
        s_texCache[i].epoch = 0; s_texCache[i].lastUse = 0;
    }
    memset(s_texHint, 0, sizeof(s_texHint));
    {   /* Slot 0: solid white. Untextured primitives sample its centre so
         * their vertex colour comes through unmodified. Alpha 0x80, not 0xFF:
         * FlushBatch treats every batch as textured (alpha = texel.a) and the
         * PSX ABR0 weight of an UNTEXTURED primitive is exactly 0.5. */
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(s_atlas->LockRect(0, &lr, NULL, 0))) {
            int x, y;
            for (y = 0; y < GPU_TEX_MAX; y++) {
                unsigned* row = (unsigned*)((unsigned char*)lr.pBits + (size_t)y * lr.Pitch);
                for (x = 0; x < GPU_TEX_MAX; x++)
                    row[x] = 0x80FFFFFFu;
            }
            s_atlas->UnlockRect(0);
        }
    }
    s_white = CreateLinearTexture(1, 1);
    if (!s_white) {
        s_ready = 0;
        return;
    }
    {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(s_white->LockRect(0, &lr, NULL, 0))) {
            *(unsigned*)lr.pBits = 0x80FFFFFFu;
            s_white->UnlockRect(0);
        }
    }
    s_curTex    = s_atlas;
    s_bindU     = 128.0f;   /* white slot centre */
    s_bindV     = 128.0f;
    s_bindScale = 0.0f;
    SegReset();

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
    SH_DBG("[GPU] d3d backend: %d-vertex pool, %d B/vertex, %dx%d atlas (%d page slots)",
           GPU_POOL_VERTS, (int)sizeof(ShVertex), ATLAS_W, ATLAS_H, TEX_CACHE_N);
}

/* Submit whatever has accumulated since the last flush under the CURRENT state.
 * Called before every state change so the batch that was built under the old
 * state is the batch that gets drawn with it. */
static void FlushBatch(void)
{
    int                count = s_poolUsed - s_flushStart;
    unsigned long long t0    = __mftb();

    if (count <= 0) {
        SegReset();
        return;
    }
    if (!s_ready) {                 /* count-only mode: account and drop */
        s_flushStart = s_poolUsed;
        SegReset();
        return;
    }

    if (s_curTex == s_atlas) {
        /* Rewrite each segment's UVs into atlas texels. Clamp inside the slot
         * BEFORE adding the origin: a point sample at exactly 256 would read
         * the neighbouring slot's first texel. */
        int i;
        for (i = 0; i < s_segCount; i++) {
            int   a  = s_segs[i].start;
            int   b  = (i + 1 < s_segCount) ? s_segs[i + 1].start : s_poolUsed;
            float sc = s_segs[i].scale, uo = s_segs[i].u, vo = s_segs[i].v;
            if (a < s_flushStart) a = s_flushStart;
            for (; a < b; a++) {
                float u = s_pool[a].tex[0] * sc;
                float v = s_pool[a].tex[1] * sc;
                if (u < 0.0f) u = 0.0f; else if (u > 255.99f) u = 255.99f;
                if (v < 0.0f) v = 0.0f; else if (v > 255.99f) v = 255.99f;
                s_pool[a].tex[0] = u + uo;
                s_pool[a].tex[1] = v + vo;
            }
        }
        s_texScale[0] = 1.0f / (float)ATLAS_W;
        s_texScale[1] = 1.0f / (float)ATLAS_H;
    }

    {   /* Screen pixels -> clip space. Y is negated because PSX screen Y grows
         * downward. Recomputed per flush so a content-rect change cannot leave
         * a stale mapping behind. */
        float vp[4];
        vp[0] =  2.0f / s_viewW;
        vp[1] = -2.0f / (float)g_Nv2aContentH;
        vp[2] = -1.0f + 2.0f * s_viewX / s_viewW;
        vp[3] =  1.0f;
        s_dev->SetVertexShaderConstantF(0, vp, 1);
        s_dev->SetVertexShaderConstantF(1, s_texScale, 1);

        /* Output alpha = texel.a * x + y; see ps.hlsl. Every batch is
         * textured now: ABR0 takes the texel's own alpha (psx_vram encodes
         * STP as 0x80 vs 0xFF, and the white slot carries 0x80 so untextured
         * primitives get the PSX 0.5), ABR3 the constant quarter. */
        {
            float blend[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
            if (s_blendMode == 1)      { blend[0] = 1.0f; blend[1] = 0.0f;  }
            else if (s_blendMode == 4) { blend[0] = 0.0f; blend[1] = 0.25f; }
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
    SegReset();
    g_Nv2aDrawCycles += __mftb() - t0;   /* [OTT] submitUs */
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
    SegReset();

    if (s_dev) {
        /* The texture ring is reused from here on; the previous frame must
         * have finished sampling it. */
        if (s_frameFenceValid) {
            s_dev->BlockOnFence(s_frameFence);
            s_frameFenceValid = 0;
        }
        SelectView();                /* 4:3 for 2D screens, Hor+ in game */
        /* Clear covers only the current viewport. With the 4:3 viewport set
         * first, the pillarbox strips were never written and showed whatever
         * EDRAM held (garbage on hardware, black in Xenia). */
        {
            D3DVIEWPORT9 full = s_vp;
            full.X = 0; full.Y = 0; full.Width = FB_W; full.Height = FB_H;
            s_dev->SetViewport(&full);
            s_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
            s_dev->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF000000u, 1.0f, 0);
        }
        s_dev->SetViewport(&s_vp);
        s_dev->Clear(0, NULL, D3DCLEAR_TARGET,
                     s_clearOverrideOn ? s_clearOverride : GpuXbox_GetClearColor(), 1.0f, 0);
        ApplyRenderState();
    }
}

/* Framebuffer readback for unattended runs: with game:\silenthillbdump.txt
 * present, every 300th frame is resolved to a texture, untiled and written as
 * raw ARGB next to the log (tools/argb2png.py turns it into a PNG). Bounded
 * to 16 dumps; a locked desktop makes Xenia screenshots impossible and the
 * console has no other way to show what it drew. */
static void FbDumpMaybe(void)
{
    static int                 s_enabled = -1;
    static int                 s_count   = 0;
    static int                 s_every   = 300;
    static int                 s_max     = 16;
    static IDirect3DTexture9*  s_fbTex   = 0;
    static unsigned char*      s_buf     = 0;
    HRESULT                    hr;
    D3DLOCKED_RECT             lr;

    if (s_enabled < 0) {
        /* The file may hold "<every frames> <max dumps>"; empty = 300 16. */
        FILE* f = fopen("game:\\silenthill\\fbdump.txt", "r");
        s_enabled = f ? 1 : 0;
        if (f) {
            int e = 0, m = 0;
            if (fscanf(f, "%d %d", &e, &m) >= 1 && e >= 60) s_every = e;
            if (m > 0 && m <= 64) s_max = m;
            fclose(f);
            SH_DBG("[FBDUMP] enabled: every %d frames, max %d", s_every, s_max);
        }
    }
    if (!s_enabled || s_count >= s_max || (s_frame % s_every) != (s_every / 2))
        return;
    if (!s_fbTex) {
        hr = s_dev->CreateTexture(FB_W, FB_H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_fbTex, NULL);
        if (FAILED(hr)) { SH_DBG("[FBDUMP] CreateTexture failed 0x%08x", (unsigned)hr); s_enabled = 0; return; }
        s_buf = (unsigned char*)malloc((size_t)FB_W * FB_H * 4);
        if (!s_buf) { s_enabled = 0; return; }
    }
    hr = s_dev->Resolve(D3DRESOLVE_RENDERTARGET0, NULL, s_fbTex, NULL, 0, 0, NULL, 0, 0, NULL);
    if (FAILED(hr)) { SH_DBG("[FBDUMP] Resolve failed 0x%08x", (unsigned)hr); s_enabled = 0; return; }
    s_dev->BlockUntilIdle();
    if (SUCCEEDED(s_fbTex->LockRect(0, &lr, NULL, D3DLOCK_READONLY))) {
        char  path[96];
        FILE* f;
        XGUntileSurface(s_buf, FB_W * 4, NULL, lr.pBits, FB_W, FB_H, NULL, 4);
        s_fbTex->UnlockRect(0);
        _snprintf(path, sizeof(path), "%sfb_%02d_1280x720_f%u.argb", Sh360Fs_WritableRoot(), s_count, s_frame);
        path[sizeof(path) - 1] = 0;
        f = fopen(path, "wb");
        if (f) { fwrite(s_buf, 4, (size_t)FB_W * FB_H, f); fclose(f); SH_DBG("[FBDUMP] wrote %s", path); }
        else   { SH_DBG("[FBDUMP] cannot open %s", path); }
    } else {
        SH_DBG("[FBDUMP] LockRect failed");
    }
    s_count++;
}

extern "C" void GpuNv2a_FrameEnd(void)
{
    FlushBatch();

    if (s_dev) {
        FbDumpMaybe();
        FreezeCaptureBeforePresent();
        s_dev->Present(NULL, NULL, NULL, NULL);
        /* PGXP shadow-table generation (pgxp_xbox.c); no-op with use_pgxp=0. */
        Pgxp_FrameAdvance();
        s_frameFence      = s_dev->InsertFence();
        s_frameFenceValid = 1;
    }

    if ((s_frame % 60) == 0) {
        SH_DBG("[GPU] frame=%u prims=%u verts=%u draws=%u texbind=%u texup=%u blend=%u scissor=%u ready=%d",
               s_frame, s_batchCalls, s_batchVertsThisFrame, s_drawsThisFrame,
               s_texBindsThisFrame, (unsigned)s_texRingUsedThisFrame, s_blendChanges, s_scissorChanges, s_ready);
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
        SegReset();
    }
    p = &s_pool[s_poolUsed];
    s_poolUsed += count;
    s_batchVertsThisFrame += (unsigned)count;
    s_batchCalls++;
    return p;
}

/* Composed ARGB copy of the current paletted page. static, not stack: 256KB. */
static unsigned s_palComposed[GPU_TEX_MAX * GPU_TEX_MAX];

static unsigned TexKeyHash(const void* page, const void* pal)
{
    unsigned h = (unsigned)((size_t)page >> 6) ^ (unsigned)((size_t)pal >> 4);
    h ^= h >> 8;
    return h & 255u;
}

static TexCacheEnt* TexCacheFind(const void* page, const void* pal)
{
    unsigned hint = s_texHint[TexKeyHash(page, pal)];
    int i;
    if (hint) {
        TexCacheEnt* e = &s_texCache[hint - 1];
        if (e->page == page && e->pal == pal)
            return e;
    }
    for (i = 0; i < TEX_CACHE_N; i++)
        if (s_texCache[i].page == page && s_texCache[i].pal == pal)
            return &s_texCache[i];
    return NULL;
}

/* Least recently used entry that this frame has not drawn with; if every
 * entry was bound this frame the GPU may still read any of them, so drain. */
static TexCacheEnt* TexCacheVictim(void)
{
    const unsigned now = (unsigned)g_Nv2aFrameCount;
    TexCacheEnt* best = NULL;
    TexCacheEnt* oldest = NULL;
    int i;
    for (i = 0; i < TEX_CACHE_N; i++) {
        TexCacheEnt* e = &s_texCache[i];
        if (!e->page) return e;
        if (!oldest || e->lastUse < oldest->lastUse) oldest = e;
        if (e->lastUse == now) continue;
        if (!best || e->lastUse < best->lastUse) best = e;
    }
    if (!best) {
        static int s_toldWrap;
        if (!s_toldWrap) { s_toldWrap = 1; SH_DBG("[GPU] texture cache exhausted within one frame (%d entries)", TEX_CACHE_N); }
        FlushBatch();
        s_dev->BlockUntilIdle();
        best = oldest;
    }
    return best;
}

/* Copy a w x h ARGB image into the slot's top-left. D3DLOCK_NOOVERWRITE: the
 * GPU may be reading OTHER slots of the atlas right now (this frame's earlier
 * batches); the slot being written was not bound this frame (TexCacheVictim)
 * and the previous frame is fenced at FrameBegin, so no wait is needed. */
static void AtlasUpload(int slot, const unsigned* src, int w, int h)
{
    D3DLOCKED_RECT lr;
    const int sx = (slot % ATLAS_COLS) * GPU_TEX_MAX;
    const int sy = (slot / ATLAS_COLS) * GPU_TEX_MAX;
    if (SUCCEEDED(s_atlas->LockRect(0, &lr, NULL, D3DLOCK_NOOVERWRITE))) {
        unsigned char* dst = (unsigned char*)lr.pBits + (size_t)sy * lr.Pitch + (size_t)sx * 4;
        int y;
        for (y = 0; y < h; y++)
            memcpy(dst + (size_t)y * lr.Pitch, src + (size_t)y * w, (size_t)w * 4);
        s_atlas->UnlockRect(0);
    }
    s_texRingUsedThisFrame++;
}

/* Find the atlas entry for (page, pal), (re)uploading when psx_vram.c has
 * re-decoded either since the copy was made. The decode EPOCH short-circuits
 * the common case: if nothing was decoded anywhere since this entry was last
 * verified, the two generation lookups (linear scans in psx_vram.c, ~830 a
 * frame) are skipped. */
static TexCacheEnt* TexCacheLookup(const void* page, const void* pal, int w, int h)
{
    const unsigned     ep  = PsxVram_DecodeEpoch();
    const unsigned     now = (unsigned)g_Nv2aFrameCount;
    unsigned long long t0  = __mftb();
    TexCacheEnt*       e   = TexCacheFind(page, pal);

    if (!(e && e->epoch == ep)) {
        unsigned pg = PsxVram_DataGen(page);
        unsigned pl = pal ? PsxVram_DataGen(pal) : 0;
        if (e && pg && (!pal || pl) && e->pageGen == pg && e->palGen == pl) {
            e->epoch = ep;
        } else {
            if (!e) e = TexCacheVictim();
            e->page = page; e->pal = pal; e->pageGen = pg; e->palGen = pl;
            /* A source psx_vram.c does not own (gen 0) can change under us
             * without moving the epoch: never let it pass the epoch test. */
            e->epoch = (pg && (!pal || pl)) ? ep : ep - 1u;
            s_texHint[TexKeyHash(page, pal)] = (unsigned char)((e - s_texCache) + 1);
            if (pal) {
                /* No palette stage in this backend: compose CPU-side. The page
                 * is 256x256 8-bit indices (linear on this port), the palette
                 * 256 ARGB. */
                const unsigned char* idx = (const unsigned char*)page;
                const unsigned*      cl  = (const unsigned*)pal;
                int i;
                for (i = 0; i < GPU_TEX_MAX * GPU_TEX_MAX; i++)
                    s_palComposed[i] = cl[idx[i]];
                AtlasUpload(e->slot, s_palComposed, GPU_TEX_MAX, GPU_TEX_MAX);
            } else {
                AtlasUpload(e->slot, (const unsigned*)page, w, h);
            }
        }
    }
    e->lastUse = now;
    g_Nv2aDrawCycles += __mftb() - t0;
    return e;
}

/* Make the current bind an atlas region: slot origin + uv->texel factor. The
 * batch is NOT broken unless we are coming from a non-atlas texture. */
static void AtlasSelect(float u, float v, float scale)
{
    if (s_curTex != s_atlas) {
        FlushBatch();
        s_curTex = s_atlas;
    }
    s_bindU = u; s_bindV = v; s_bindScale = scale;
    SegMark();
}

static void AtlasSelectSlot(int slot, float scale)
{
    AtlasSelect((float)((slot % ATLAS_COLS) * GPU_TEX_MAX),
                (float)((slot / ATLAS_COLS) * GPU_TEX_MAX), scale);
}

/* Oversize texture (FMV frames, 320x240): its own texture, uploaded on every
 * bind of a NEW pointer. Two alternate because the previous present may still
 * be sampling the other. The batch breaks here, which is fine: a movie frame
 * is one quad. */
static void BindBig(const void* addr, int w, int h)
{
    D3DLOCKED_RECT lr;
    int i = s_bigNext & 1;
    s_bigNext++;
    FlushBatch();
    if (!s_big[i] || s_bigW[i] != w || s_bigH[i] != h) {
        if (s_big[i]) { s_dev->BlockUntilIdle(); s_big[i]->Release(); s_big[i] = NULL; }
        s_big[i] = CreateLinearTexture(w, h);
        s_bigW[i] = w; s_bigH[i] = h;
        if (!s_big[i]) { s_curTex = s_white; return; }
        SH_DBG("[GPU] oversize texture %dx%d: dedicated surface %d", w, h, i);
    }
    if (SUCCEEDED(s_big[i]->LockRect(0, &lr, NULL, 0))) {
        unsigned char* dst = (unsigned char*)lr.pBits;
        int y;
        for (y = 0; y < h; y++)
            memcpy(dst + (size_t)y * lr.Pitch, (const unsigned*)addr + (size_t)y * w, (size_t)w * 4);
        s_big[i]->UnlockRect(0);
    }
    s_texRingUsedThisFrame++;
    s_curTex = s_big[i];
    s_texScale[0] = 1.0f / (float)w;
    s_texScale[1] = 1.0f / (float)h;
}

extern "C" void GpuNv2a_BindPaletted(const void* page, const void* palette)
{
    TexCacheEnt* e;

    if (!page || !palette) {
        GpuNv2a_BindWhite();
        return;
    }
    if (page == s_texAddr && palette == s_texPal)
        return;                     /* redundant bind */
    s_texAddr = page;
    s_texPal  = palette;
    s_texW = s_texH = GPU_TEX_MAX;
    s_texBindsThisFrame++;
    if (!s_ready) { s_curTex = s_white; return; }

    e = TexCacheLookup(page, palette, GPU_TEX_MAX, GPU_TEX_MAX);
    /* gpu_xbox.c pre-scales the UVs of paletted pages to 0..1 (PAL_UV_SCALE,
     * an NV2A palette-texture convention): 256 takes them back to texels. */
    AtlasSelectSlot(e->slot, (float)GPU_TEX_MAX);
}

extern "C" void GpuNv2a_BindTexture(const void* addr, int w, int h)
{
    TexCacheEnt* e;

    if (addr == s_texAddr && s_texPal == NULL && w == s_texW && h == s_texH)
        return;                     /* redundant bind */
    s_texAddr = addr;
    s_texPal  = NULL;
    s_texW = w;
    s_texH = h;
    s_texBindsThisFrame++;

    if (!s_ready) { s_curTex = s_white; return; }
    if (!addr || w <= 0 || h <= 0) {
        AtlasSelect(128.0f, 128.0f, 0.0f);
        return;
    }
    if (w > GPU_TEX_MAX || h > GPU_TEX_MAX) {
        BindBig(addr, w, h);
        return;
    }
    e = TexCacheLookup(addr, NULL, w, h);
    /* UVs arrive in TEXELS. */
    AtlasSelectSlot(e->slot, 1.0f);
}

extern "C" void GpuNv2a_BindWhite(void)
{
    if (!s_texAddr && s_curTex == s_atlas && s_bindScale == 0.0f)
        return;
    s_texAddr = NULL;
    s_texPal  = NULL;
    if (!s_ready) { s_curTex = s_white; return; }
    AtlasSelect(128.0f, 128.0f, 0.0f);     /* every vertex samples the white slot centre */
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
    if (!s_scFull && g_Nv2aContentW > 0 && g_Nv2aContentH > 0) {
        RECT  r;
        float ppu = (float)s_vp.Width / s_viewW;           /* framebuffer px per content unit */
        r.left   = (LONG)s_vp.X + (LONG)((s_viewX + (float)s_scX) * ppu);
        r.top    = (LONG)s_vp.Y + s_scY * (LONG)s_vp.Height / g_Nv2aContentH;
        r.right  = r.left + (LONG)((float)s_scW * ppu);
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
    const int full = (w <= 0 || h <= 0);
    if (full) {                      /* reset: the whole viewport, not the 4:3 rect */
        x = g_Nv2aContentX; y = 0; w = g_Nv2aContentW; h = g_Nv2aContentH;
    }
    if (full == s_scFull && x == s_scX && y == s_scY && w == s_scW && h == s_scH)
        return;
    FlushBatch();
    s_scX = x; s_scY = y; s_scW = w; s_scH = h; s_scFull = full;
    s_scissorChanges++;
    SH_DBG("[GPU] scissor content=(%d,%d %dx%d) of %dx%d", x, y, w, h, g_Nv2aContentW, g_Nv2aContentH);
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

/* Freeze-frame (pause / item pickup / map message backgrounds). The shared
 * code arms g_PsxPresentLastFrame on the tick that still renders the world;
 * FrameEnd resolves that frame into s_freezeTex before presenting it, and
 * FreezeBlit lays it back under the next frames' UI. On a PSX the VRAM simply
 * kept the last picture; here every frame is cleared, so without this the
 * PAUSED screen was the bare clear colour (hardware run 021). */
static IDirect3DTexture9* s_freezeTex;
static int                s_freezeHave;
extern "C" int            g_PsxPresentLastFrame;

static void FreezeCaptureBeforePresent(void)
{
    HRESULT    hr;

    if (!g_PsxPresentLastFrame || s_freezeHave || !s_dev)
        return;
    if (!s_freezeTex) {
        hr = s_dev->CreateTexture(FB_W, FB_H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_freezeTex, NULL);
        if (FAILED(hr)) { SH_DBG("[FREEZE] CreateTexture failed 0x%08x", (unsigned)hr); return; }
    }
    hr = s_dev->Resolve(D3DRESOLVE_RENDERTARGET0, NULL, s_freezeTex, NULL, 0, 0, NULL, 0, 0, NULL);
    if (FAILED(hr)) { SH_DBG("[FREEZE] Resolve failed 0x%08x", (unsigned)hr); return; }
    s_freezeHave = 1;
    SH_DBG("[FREEZE] captured frame %u", s_frame);
}

extern "C" int  GpuNv2a_FreezeCapture(void) { return s_freezeHave; }
extern "C" void GpuNv2a_FreezeRelease(void) { s_freezeHave = 0; }

extern "C" void GpuNv2a_FreezeBlit(void)
{
    ShVertex v[6];
    float    vp[4], ts[4], blend[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
    float    cw, ch, u0, u1, v1;
    int      i;

    if (!s_freezeHave || !s_freezeTex || !s_dev || !s_ready)
        return;
    FlushBatch();

    /* Content-pixel positions through the same pixel->clip mapping as the
     * batches; the texture is the whole 1280x720 frame, so the content rect's
     * texels are the 4:3 viewport's pixels. */
    cw = s_viewW;                 /* the whole viewport, Hor+ or 4:3 */
    ch = (float)g_Nv2aContentH;
    u0 = (float)s_vp.X;
    u1 = (float)(s_vp.X + s_vp.Width);
    v1 = (float)(s_vp.Y + s_vp.Height);
    memset(v, 0, sizeof(v));
    for (i = 0; i < 6; i++) {
        int right  = (i == 1 || i == 2 || i == 4);
        int bottom = (i == 2 || i == 4 || i == 5);
        v[i].pos[0] = (right ? cw : 0.0f) - s_viewX;
        v[i].pos[1] = bottom ? ch : 0.0f;
        v[i].pos[2] = 0.0f;
        v[i].pos[3] = 1.0f;
        v[i].col[0] = v[i].col[1] = v[i].col[2] = v[i].col[3] = 1.0f;
        v[i].tex[0] = right  ? u1 : u0;
        v[i].tex[1] = bottom ? v1 : (float)s_vp.Y;
    }
    vp[0] = 2.0f / cw; vp[1] = -2.0f / ch; vp[2] = -1.0f + 2.0f * s_viewX / cw; vp[3] = 1.0f;
    ts[0] = 1.0f / (float)FB_W; ts[1] = 1.0f / (float)FB_H; ts[2] = 0.0f; ts[3] = 0.0f;

    s_dev->SetViewport(&s_vp);
    s_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    s_dev->SetVertexShaderConstantF(0, vp, 1);
    s_dev->SetVertexShaderConstantF(1, ts, 1);
    s_dev->SetPixelShaderConstantF(0, blend, 1);
    s_dev->SetTexture(0, s_freezeTex);
    s_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v, sizeof(ShVertex));
    s_dev->SetTexture(0, s_curTex ? s_curTex : s_white);
    ApplyRenderState();
}
