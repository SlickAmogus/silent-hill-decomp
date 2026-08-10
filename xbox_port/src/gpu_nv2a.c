/*
 * gpu_nv2a.c - NV2A rendering backend (pbkit) for the Silent Hill Xbox port.
 *
 * Low-level NV2A surface used by the PSX libgpu (gpu_xbox.c, which walks the OT
 * and converts PSX primitives into screen-space triangles). Provides frame
 * begin/end (pbkit double-buffer) and a triangle-list emit. PSX prims arrive
 * already projected by the software GTE, so vertices are in screen pixels and
 * the vertex shader (vs.vs.cg) is a window-space passthrough; the pixel shader
 * (ps.ps.cg) does texture * diffuse. Untextured prims bind a 1x1 white texture
 * so texture*colour collapses to the per-vertex colour.
 *
 * pbkit sequences mirror nxdk's `mesh` sample. See the nv2a-backend memory for
 * the gotchas (window coords, DRAW_ARRAYS vs 16-bit indices, texel UVs, sfence).
 */
#include <pbkit/pbkit.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdint.h>
#include <string.h>

#include "gpu_nv2a.h"
#include "sh_log.h"

#define MAXRAM 0x03FFAFFF
#define MASK(mask, val) (((val) << (__builtin_ffs(mask) - 1)) & (mask))

/* Fine-grained CPU cycle counter (KeTickCount's 1ms is too coarse to attribute
 * a 50ms frame across ~320 sub-ms draws). 733MHz P3 -> cycles/733000 = ms. */
static inline unsigned long long sh_rdtsc(void)
{
    unsigned lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}
/* CPU cycles spent in the actual GPU-command submission (FlushBatch draws) this
 * frame — gpu_xbox.c reads + resets it to split "draw submit" from "prim parse/
 * transform" in the [OTS] timing line. */
unsigned long long g_Nv2aDrawCycles = 0;

/* Sized for a whole frame's geometry. At 1024 (~170 quads) the pool filled mid-
 * frame and EmitTris silently DROPPED the rest of the scene — and since DrawOTag
 * walks far->near, the dropped remainder was the near-camera geometry (Harry + his
 * surroundings), leaving the clear colour as a "grey void". 65536 verts = ~2.3 MB. */
/* Sized for the heaviest frame measured plus ~2x headroom, not for a round
 * number. The busiest cafe frame in the logs is ~1400 primitives = ~8400 verts;
 * at 65536 the pool reserved 4MB of contiguous memory to hold ~8k verts, and
 * that memory comes out of the SAME pool the texture cache draws from. With
 * texture thrash now the dominant cost (log 050: texUs=20645 of a 31757us walk)
 * those 3MB are worth ~12 more cache slots -- a 28% larger cache -- which is a
 * far better use of them. Overflow is not a failure: GpuNv2a_BatchAlloc draws
 * the pending run, drains, and recycles from 0, so a bigger frame just costs one
 * extra flush. [NV2A] logs the peak so this stays honest. */
#define MAX_BATCH_VERTS 16384

/* NV2A linear/NPOT textures need an aligned pitch (>= 64 bytes); a 1x1 (pitch 4)
 * triggers a GPU "invalid data error". Use 64x64 (pitch 256), proven-good. */
#define WHITE_TEX_DIM 64

static ShVertex* s_batch;     /* contiguous staging pool for vertex submission */

/* The write-combining win depends on the stride being exactly one cache line;
 * a silent change here would quietly restore the partial-line evictions (and a
 * wrong stride garbles geometry, since the NV2A reads attributes at
 * sizeof(ShVertex)). Fail the build instead of the hardware run. */
typedef char ShVertex_must_be_one_cache_line[(sizeof(ShVertex) == 64) ? 1 : -1];
static int       s_batchUsed; /* running offset into the pool (this frame) */
static int       s_runStart;  /* start of the pending un-drawn run [s_runStart,
                               * s_batchUsed): adjacent same-state prims accumulate
                               * here and draw as ONE run at the next state change,
                               * frame end, or pool wrap — the batching win. */
static unsigned  s_flushCount; /* draws issued this frame (batching effectiveness) */
static uint32_t* s_whiteTex;  /* opaque white, for untextured prims */

/* Most recently COMPLETED frame's surface (captured in FrameEnd just before the
 * present queues it and pbkit advances its back index). The pbkit ring is 3 deep,
 * so this buffer stays intact for two more frames — long enough for the gated
 * framebuffer->PSX-VRAM readback (gpu_xbox.c) to read it mid-walk. */
static const void* s_lastQueuedFb;
static int         s_freezePending;   /* freeze capture requested; runs at FrameBegin */
static unsigned    s_frameBeginMs;    /* ms at FrameBegin; splits game tick vs submission */
static void        FreezeRunPending(void);

static int s_frameW, s_frameH;

/* Frame counter for gpu_xbox.c's render census (frame-boundary detection). */
int g_Nv2aFrameCount = 0;

/* PGXP master flag (pgxp_stub.c, config-driven). Read only to gate the per-frame
 * shadow-table generation bump in FrameEnd; 0 (default) makes that a no-op. */
extern int g_PsxUsePgxp;

static void GpuNv2a_SetRenderState(void);
static void SetAttribPointer(unsigned index, unsigned size, const void* data);
static void GpuNv2a_FlushBatch(void);

static void GpuNv2a_InitShader(void)
{
    uint32_t* p;
    int       i;

    uint32_t vs_program[] = {
        #include "vs.inl"
    };

    p = pb_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, 0);
    p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
                 MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM)
                 | MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV));
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
    pb_end(p);

    p = pb_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0);
    pb_end(p);

    for (i = 0; i < (int)(sizeof(vs_program) / 16); i++) {
        p = pb_begin();
        pb_push(p++, NV097_SET_TRANSFORM_PROGRAM, 4);
        memcpy(p, &vs_program[i * 4], 4 * 4);
        p += 4;
        pb_end(p);
    }

    /* Fragment shader (register combiners): texture * diffuse */
    p = pb_begin();
    #include "ps.inl"
    pb_end(p);
}

/* Output geometry: the physical framebuffer and the centered 4:3 content rect
 * the game maps into. 640x480 = identity (content == fb, no pillars). 720p =
 * content 960x720 at x=160 with black pillars either side — the game (and its
 * menus) stays 4:3, PSX-faithful ("menus only"-style pillarboxing). Derived
 * from the video mode pbkit picked up; consumed by gpu_xbox.c's transform,
 * the scissor, the pillarbox clear, the FB readback, and the FMV quad. */
int g_Nv2aFbW      = 640;
int g_Nv2aFbH      = 480;
int g_Nv2aContentX = 0;
int g_Nv2aContentW = 640;
int g_Nv2aContentH = 480;

void GpuNv2a_Init(void)
{
    GpuNv2a_InitShader();

    g_Nv2aFbW      = pb_back_buffer_width();
    g_Nv2aFbH      = pb_back_buffer_height();
    g_Nv2aContentH = g_Nv2aFbH;
    g_Nv2aContentW = (g_Nv2aFbH * 4) / 3;
    if (g_Nv2aContentW > g_Nv2aFbW) g_Nv2aContentW = g_Nv2aFbW;
    g_Nv2aContentX = (g_Nv2aFbW - g_Nv2aContentW) / 2;
    SH_DBG("[SH-XBOX] NV2A fb %dx%d, content %dx%d at x=%d",
           g_Nv2aFbW, g_Nv2aFbH, g_Nv2aContentW, g_Nv2aContentH, g_Nv2aContentX);

    s_batch    = MmAllocateContiguousMemoryEx(MAX_BATCH_VERTS * sizeof(ShVertex), 0, MAXRAM, 0,
                                              PAGE_READWRITE | PAGE_WRITECOMBINE);
    s_whiteTex = MmAllocateContiguousMemoryEx(WHITE_TEX_DIM * WHITE_TEX_DIM * 4, 0, MAXRAM, 0,
                                              PAGE_READWRITE | PAGE_WRITECOMBINE);
    {
        int i;
        for (i = 0; i < WHITE_TEX_DIM * WHITE_TEX_DIM; i++)
            s_whiteTex[i] = 0xffffffff;
    }
    __asm__ __volatile__("sfence" ::: "memory");

    SH_DBG("[SH-XBOX] NV2A backend ready (batch=%d verts)", MAX_BATCH_VERTS);
}

/* Render state for flat 2D screen-space prims: no depth/cull, specular-enable
 * on (required for combiner output), alpha test on (kills fully-transparent
 * texels), blend off by default, z-clamp. */
static void GpuNv2a_SetRenderState(void)
{
    uint32_t* p = pb_begin();
    p = pb_push1(p, NV097_SET_SPECULAR_ENABLE, 1);
    p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
    p = pb_push1(p, NV097_SET_SKIN_MODE, NV097_SET_SKIN_MODE_OFF);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_DEPTH_MASK, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    /* Alpha test GEQUAL 1: PSX texel 0x0000 decodes to alpha 0 (fully
     * transparent) and previously painted BLACK cut-out boxes around fences /
     * foliage / decals; the combiner already outputs alpha = tex.a * col.a
     * (final combiner G = primary alpha), so only true transparent texels die. */
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 1);
    p = pb_push1(p, NV097_SET_ALPHA_FUNC, NV097_SET_ALPHA_FUNC_V_GEQUAL);
    p = pb_push1(p, NV097_SET_ALPHA_REF, 1);
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
    p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA);
    p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA);
    p = pb_push1(p, NV097_SET_BLEND_EQUATION, NV097_SET_BLEND_EQUATION_V_FUNC_ADD);
    p = pb_push1(p, NV097_SET_BLEND_COLOR, 0x40000000);   /* constant alpha = 0.25 (ABR3) */
    p = pb_push1(p, NV097_SET_ZMIN_MAX_CONTROL, NV097_SET_ZMIN_MAX_CONTROL_ZCLAMP_CLAMP);
    /* Final-combiner override (after ps.inl set it at init): out = C + D with
     * C = SECONDARY (the per-vertex fog term fogColor*f from ShVertex.spec) and
     * D = PRIMARY (stage0's tex*diffuse). A=B=ZERO so A*B+(1-A)*C+D = C+D.
     * Alpha (CW1 G) stays primary.a — fog does not affect alpha. */
    p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0,
                 MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_C_SOURCE, 0x5)   /* secondary */
                 | MASK(NV097_SET_COMBINER_SPECULAR_FOG_CW0_D_SOURCE, 0x4)); /* primary */
    pb_end(p);
}

/* Blend state, encoded: 0 = off; 1..4 = ABE on with PSX ABR mode 0..3.
 *   ABR0 (1): 0.5B+0.5F  -> SRC_ALPHA / ONE_MINUS_SRC_ALPHA (per-texel STP:
 *             STP=1 texels decode alpha 0x80 = 50/50, STP=0 alpha 0xFF = opaque
 *             even inside a semi-trans prim — PSX-exact; untextured semi-trans
 *             prims carry vertex alpha 0.5 from gpu_xbox.c).
 *   ABR1 (2): B+F   additive          -> ONE / ONE, ADD.
 *   ABR2 (3): B-F   subtractive       -> ONE / ONE, REVERSE_SUBTRACT.
 *   ABR3 (4): B+F/4 quarter-additive  -> CONSTANT_ALPHA(0.25) / ONE, ADD. */
/* Depth test toggle for the inventory/pickup item real-depth pass (gpu_xbox.c
 * PsyX_ForceItemDepthBegin/End). The zeta buffer is cleared every FrameBegin and
 * nothing else tests or writes it, so enabling here sees a fresh far plane.
 * LEQUAL matches painter's order for equal-depth prims (later draw wins). */
void GpuNv2a_SetDepthTest(int enable)
{
    uint32_t* p;
    GpuNv2a_FlushBatch();   /* pending run drew under the OLD depth state */
    p = pb_begin();
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, enable ? 1 : 0);
    p = pb_push1(p, NV097_SET_DEPTH_MASK,        enable ? 1 : 0);
    if (enable)
        p = pb_push1(p, NV097_SET_DEPTH_FUNC, NV097_SET_DEPTH_FUNC_V_LEQUAL);
    pb_end(p);
}

/* Depth WRITE alone (test stays as set above). The item pass flips this per
 * tagged/untagged run: untagged prims (UI sprites, 2D packets) keep z=0 =
 * nearest -> always PASS the LEQUAL test (painter-style, draw over everything)
 * but must not WRITE, or each stamps a z=0 wall that erases everything drawn
 * after it at those pixels (the "antenna disappears / worse see-through" bug). */
void GpuNv2a_SetDepthWrite(int enable)
{
    uint32_t* p;
    GpuNv2a_FlushBatch();
    p = pb_begin();
    p = pb_push1(p, NV097_SET_DEPTH_MASK, enable ? 1 : 0);
    pb_end(p);
}

void GpuNv2a_SetBlendMode(int mode)
{
    uint32_t* p;
    GpuNv2a_FlushBatch();   /* draw the pending run under the OLD blend first */
    p = pb_begin();
    if (mode <= 0) {
        p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
    } else {
        static const uint32_t sf[4] = {
            NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA,
            NV097_SET_BLEND_FUNC_SFACTOR_V_ONE,
            NV097_SET_BLEND_FUNC_SFACTOR_V_ONE,
            NV097_SET_BLEND_FUNC_SFACTOR_V_CONSTANT_ALPHA,
        };
        static const uint32_t df[4] = {
            NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA,
            NV097_SET_BLEND_FUNC_DFACTOR_V_ONE,
            NV097_SET_BLEND_FUNC_DFACTOR_V_ONE,
            NV097_SET_BLEND_FUNC_DFACTOR_V_ONE,
        };
        static const uint32_t eq[4] = {
            NV097_SET_BLEND_EQUATION_V_FUNC_ADD,
            NV097_SET_BLEND_EQUATION_V_FUNC_ADD,
            NV097_SET_BLEND_EQUATION_V_FUNC_REVERSE_SUBTRACT,
            NV097_SET_BLEND_EQUATION_V_FUNC_ADD,
        };
        const int abr = mode - 1;
        p = pb_push1(p, NV097_SET_BLEND_ENABLE, 1);
        p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, sf[abr]);
        p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, df[abr]);
        p = pb_push1(p, NV097_SET_BLEND_EQUATION, eq[abr]);
    }
    pb_end(p);
}

/* Draw-clip scissor (NV2A window clip, inclusive coords). w/h <= 0 resets to
 * the full surface. */
void GpuNv2a_SetScissor(int x, int y, int w, int h)
{
    uint32_t* p;
    int x1, y1;
    GpuNv2a_FlushBatch();   /* pending run drew under the OLD scissor */
    /* Reset = the CONTENT rect (not the full surface): nothing may draw into
     * the pillarbox bars. Identity at 640x480 where content == fb. */
    if (w <= 0 || h <= 0) { x = g_Nv2aContentX; y = 0; w = g_Nv2aContentW; h = g_Nv2aContentH; }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    x1 = x + w - 1; if (x1 > g_Nv2aFbW - 1) x1 = g_Nv2aFbW - 1;
    y1 = y + h - 1; if (y1 > g_Nv2aFbH - 1) y1 = g_Nv2aFbH - 1;
    if (x1 < x) x1 = x;
    if (y1 < y) y1 = y;
    p = pb_begin();
    p = pb_push1(p, NV097_SET_WINDOW_CLIP_TYPE, 0);              /* include */
    p = pb_push1(p, NV097_SET_WINDOW_CLIP_HORIZONTAL, ((uint32_t)x1 << 16) | (uint32_t)x);
    p = pb_push1(p, NV097_SET_WINDOW_CLIP_VERTICAL,   ((uint32_t)y1 << 16) | (uint32_t)y);
    pb_end(p);
}

void GpuNv2a_FrameBegin(void)
{
    /* No pb_wait_for_vbl() here: VSync() now drives the vblank waits (one present
     * per frame, held for the requested vblank count) via GpuNv2a_WaitVbl. */
    pb_reset();
    pb_target_back_buffer();

    g_Nv2aFrameCount++;
    s_frameBeginMs = (unsigned)KeTickCount;   /* splits game tick from submission in [FT] */

    s_frameW = pb_back_buffer_width();
    s_frameH = pb_back_buffer_height();

    /* First frame: claim the freeze-frame buffer while the heap is still empty.
     * The texture cache fills on the first textured prim (the boot logo, drawn
     * later THIS frame) and the map allocates after that, so this is the last
     * moment a multi-megabyte reservation is guaranteed to succeed. */
    {
        static int s_freezeReserved = 0;
        if (!s_freezeReserved) {
            extern void GpuNv2a_FreezeReserve(void);
            s_freezeReserved = 1;
            GpuNv2a_FreezeReserve();
        }
    }

    /* A capture requested since the last FrameBegin runs HERE: the pushbuffer has
     * just been reset and the back buffer is the target, which is the state the
     * retarget/restore pair expects. It also runs before the clear below, and
     * before the FreezeBlit that will read it later this frame. */
    if (s_freezePending)
        FreezeRunPending();

    pb_erase_depth_stencil_buffer(0, 0, s_frameW, s_frameH);
    /* Clear to the PSX draw-env isbg background — in-game GsSortClear sets it
     * to the FOG colour (game_main.c background2dColor = fog.color), so the
     * area beyond the far cull reads as a fog wall instead of a black abyss. */
    {
        uint32_t clear = GpuXbox_GetClearColor();
        static uint32_t s_lastClear = 0xFFFFFFFF;
        if (clear != s_lastClear) {
            SH_DBG("[FOGST-GPU] clear=0x%08x", (unsigned)clear);
            s_lastClear = clear;
        }
        if (g_Nv2aContentX > 0) {
            /* Pillarboxed mode: bars stay black; only the 4:3 content rect
             * gets the game's isbg/fog clear colour. */
            pb_fill(0, 0, s_frameW, s_frameH, 0xFF000000);
            pb_fill(g_Nv2aContentX, 0, g_Nv2aContentW, g_Nv2aContentH, clear);
        } else {
            pb_fill(0, 0, s_frameW, s_frameH, clear);
        }
    }
    pb_erase_text_screen();

    s_batchUsed = 0; /* recycle the vertex pool each frame */
    s_runStart  = 0; /* no pending run at frame start (the BindTexture in the
                      * setup below then flushes an empty run — a no-op) */

    /* Establish all draw state ONCE per frame (render state, texture, vertex-
     * program constant, attribute arrays). Per-draw EmitTris then only issues
     * BEGIN/DRAW_ARRAYS/END — re-doing vertex-program/attribute state between
     * draws corrupts the 2nd+ draw on NV2A (the mesh sample also sets up once,
     * draws many). Attribute base is fixed at s_batch[0]; draws select their
     * slice via DRAW_ARRAYS START_INDEX. */
    {
        uint32_t* p;
        int       i;

        GpuNv2a_SetRenderState();
        GpuNv2a_BindTexture(s_whiteTex, WHITE_TEX_DIM, WHITE_TEX_DIM);

        {
            static const float c0[4] = { 1.0f, 0.0f, 0.0f, 0.0f }; /* vs c[0] = pos.w */
            p = pb_begin();
            p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_VP_UPLOAD_CONST_ID, 96);
            pb_push(p++, NV20_TCL_PRIMITIVE_3D_VP_UPLOAD_CONST_X, 4);
            memcpy(p, c0, sizeof(c0));
            p += 4;
            pb_end(p);
        }

        p = pb_begin();
        pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16);
        for (i = 0; i < 16; i++)
            *(p++) = 2;
        pb_end(p);

        /* Position is size 4 (x,y,z,w): the vs passes I.pos.w to o.pos.w, which
         * the NV2A uses for perspective-correct varying interpolation. w==1 for
         * every non-PGXP vertex (PutVert/PutVertUV) = affine, identical to the old
         * size-3 default; PGXP writes the real per-vertex view depth into pos[3]. */
        SetAttribPointer(0, 4, &s_batch[0].pos);
        SetAttribPointer(3, 4, &s_batch[0].col);
        SetAttribPointer(4, 4, &s_batch[0].spec);  /* per-vertex fog term */
        SetAttribPointer(9, 2, &s_batch[0].tex);
    }
}

void GpuNv2a_FrameEnd(void)
{
    unsigned tFlush, tSwap, tNow;

    tFlush = (unsigned)KeTickCount;      /* ms clock (1ms resolution) */
    GpuNv2a_FlushBatch();   /* draw the frame's final run before we present */

    /* PGXP: advance the shadow-table generation once per frame. By here every
     * DrawOTag of the frame has resolved its precise vertices and the next
     * frame's GTE stores have not started, so this is the clean frame boundary
     * that stales last frame's entries (pgxp_xbox.c). No-op when PGXP is off. */
    if (g_PsxUsePgxp) {
        extern void Pgxp_FrameAdvance(void);
        Pgxp_FrameAdvance();
    }

    /* Remember this frame's surface for the framebuffer readback BEFORE
     * pb_finished() advances pb_back_index (pb_back_buffer() then points at the
     * NEXT frame's target). */
    s_lastQueuedFb = (const void*)pb_back_buffer();

    /* No pb_busy() drain: that forced strict CPU-then-GPU serialization every frame
     * (frame time = CPU_build + GPU_render + vblank). pb_finished() queues the swap
     * at vblank and only blocks when we're >2 frames ahead (the triple-buffer table
     * is full) — correct backpressure that lets the CPU build frame N+1 while the
     * GPU rasterizes frame N. */
    tSwap = (unsigned)KeTickCount;
    while (pb_finished()) { }
    tNow = (unsigned)KeTickCount;

    /* Frame-time breakdown (the 5-10fps town/combat is ~100ms/frame; static
     * analysis says ~10-20ms, so a big cost is hiding). frame = wall ms since
     * the previous FrameEnd (total frame time incl. the game's CPU work between
     * frames); swapWait = ms blocked in pb_finished (GPU behind = fill/draw
     * bound); a large swapWait vs frame => GPU-bound, small => CPU-bound. */
    {
        extern unsigned g_PsxDecodeMs, g_PsxDecodeCount;   /* psx_vram.c */
        static unsigned s_dbgFrame, s_lastEnd;
        static unsigned s_accMs, s_accDec, s_accDecMs, s_accDraws, s_worstMs;
        static unsigned s_accGame, s_accDraw;
        unsigned        dt = s_lastEnd ? (tNow - s_lastEnd) : 0;

        /* Split the frame at FrameBegin. gameMs = the game's own tick (AI,
         * animation, collision, OT build) which runs between frames; drawMs =
         * OT walk + prim build + submission. The cafe fight is 53-82ms/frame with
         * swapWait=0 (never GPU-bound) and negligible decode, so the cost is one
         * of these two and they need different fixes. */
        s_accGame += s_lastEnd ? (s_frameBeginMs - s_lastEnd) : 0;
        s_accDraw += (tNow - s_frameBeginMs);

        /* Accumulate over the reporting window so one line describes 120 frames
         * rather than whichever single frame happened to land on the modulo --
         * a spike scene is about the AVERAGE and the WORST, not a sample. */
        s_accMs    += dt;
        s_accDec   += g_PsxDecodeCount;
        s_accDecMs += g_PsxDecodeMs;
        s_accDraws += s_flushCount;
        if (dt > s_worstMs) s_worstMs = dt;

        if ((++s_dbgFrame % 120) == 0) {
            /* avgMs vs worstMs = steady cost vs hitches. decMs is CPU time inside
             * DecodePage; swapWait is time blocked waiting on the GPU. decMs high
             * => texture thrash (capacity); swapWait high => fill/draw bound (the
             * overdraw theory); neither => the cost is elsewhere in the game tick. */
            SH_DBG("[FT] avgMs=%u worstMs=%u gameMs=%u drawMs=%u swapWait=%ums draws=%u decodes=%u decMs=%u",
                   s_accMs / 120, s_worstMs, s_accGame / 120, s_accDraw / 120,
                   tNow - tSwap, s_accDraws / 120, s_accDec, s_accDecMs);
            s_accMs = s_accDec = s_accDecMs = s_accDraws = s_worstMs = 0;
            s_accGame = s_accDraw = 0;
        }
        g_PsxDecodeMs = g_PsxDecodeCount = 0;   /* per-frame counters */
        s_lastEnd = tNow;
    }
    s_flushCount = 0;
}

/* CPU-visible A8R8G8B8 surface for the gated framebuffer->PSX-VRAM readback
 * (gpu_xbox.c). fromLastQueued=0 returns the CURRENT back buffer — between the
 * end-of-tick GsDrawOt and the next VSync present that is the fully-composed,
 * pre-present frame (PSX DrawSync semantics). fromLastQueued=1 returns the last
 * COMPLETED (queued/presented) frame — the right source when called mid-OT-walk,
 * when the current back buffer only holds a partial frame. Drains the GPU first
 * so every submitted draw has landed; the caller is rare-frame gated, so the
 * stall (and the slow uncached read of write-combined memory) is acceptable. */
/* Submit everything pending and wait until the GPU has rasterized it. After this
 * returns, every texture slot bound earlier in the frame has been CONSUMED, so
 * its memory can be safely overwritten. The texture cache uses this as its
 * out-of-slots backstop: without it, a forced eviction rewrites 256KB the GPU
 * has not read yet and already-submitted prims sample the wrong page (the
 * garbled-text bug). */
void GpuNv2a_DrainGpu(void)
{
    GpuNv2a_FlushBatch();
    while (pb_busy()) { }
}

const void* GpuNv2a_ReadbackSurface(int fromLastQueued, int* w, int* h, int* pitchBytes)
{
    const void* fb;

    if (s_frameW <= 0 || s_frameH <= 0)
        return 0;               /* no frame targeted yet (early boot) */
    GpuNv2a_FlushBatch();       /* the back buffer must hold the pending run */
    fb = fromLastQueued ? s_lastQueuedFb : (const void*)pb_back_buffer();
    if (!fb)
        return 0;
    while (pb_busy()) { }       /* GPU idle => the frame is fully rasterized */
    *w = s_frameW;
    *h = s_frameH;
    *pitchBytes = (int)pb_back_buffer_pitch();
    return fb;
}

/* ---- Freeze-frame (g_PsxPresentLastFrame) --------------------------------
 * PSX never auto-cleared the framebuffer, so pausing simply left the last
 * gameplay render on screen. The shared code models that by arming
 * g_PsxPresentLastFrame on the ENTRY tick -- which still renders the world
 * normally -- and expects the backend to re-present that captured frame for as
 * long as the flag is held. Xbox had no consumer for it, so pause / item
 * pickup / map messages just showed the cleared background: the reported grey
 * screen. Capture once on the rising edge (the frame just presented IS the last
 * gameplay render), then blit it under each subsequent frame so only that
 * state's UI draws on top.
 *
 * Why capture-once rather than re-presenting the old buffer: the buffers rotate
 * and get cleared, and the UI (a rotating pickup model) must compose over a CLEAN
 * copy each frame or it smears.
 *
 * The copy is done BY THE GPU into a pbkit extra render target. It used to be a
 * CPU readback of the framebuffer, which measured 165ms at 720p -- CPU reads of
 * write-combined memory run about 20MB/s, so 3.6MB is a hard floor no amount of
 * loop tuning beats, and every pause/pickup/map-message paid it (long enough to
 * starve the VSync-driven audio pump, which is what made dialogue stutter). The
 * GPU reads the same memory at video-bandwidth: bind the last completed frame as
 * a texture, draw one 1:1 quad into the extra buffer. Point sampling and matching
 * size make it an exact copy, not a resample. */
static int   s_freezeW, s_freezeH;
static int   s_freezeValid;                    /* extra buffer holds a captured frame */
static int   s_freezeReady;                    /* pb_extra_buffers(1) succeeded */

/* Defined below this point; the freeze path is the first caller. */
static void BindTextureBpp(const void* addr, int w, int h, int bpp);
int   GpuNv2a_Ms(void);
void  GpuNv2a_BindWhite(void);
void  GpuNv2a_EmitTris(const ShVertex* verts, int count);
void* GpuNv2a_AllocTexMem(int bytes);

/* Draw a 1:1 fullscreen quad sampling `src` (A8R8G8B8, w x h) into the CURRENT
 * render target. Point sampling + matching size => an exact copy. Depth off:
 * both users are painter's-order background passes. */
static void FreezeCopyQuad(const void* src, int w, int h)
{
    ShVertex v[6];
    float    fw = (float)w, fh = (float)h;
    int      i;

    memset(v, 0, sizeof(v));
    for (i = 0; i < 6; i++) {
        v[i].col[0] = v[i].col[1] = v[i].col[2] = v[i].col[3] = 1.0f;
        v[i].pos[3] = 1.0f;                    /* affine W (0 => div-by-0 quad) */
    }
    v[0].pos[0] = 0.0f; v[0].pos[1] = 0.0f; v[0].tex[0] = 0.0f; v[0].tex[1] = 0.0f;
    v[1].pos[0] = fw;   v[1].pos[1] = 0.0f; v[1].tex[0] = fw;   v[1].tex[1] = 0.0f;
    v[2].pos[0] = 0.0f; v[2].pos[1] = fh;   v[2].tex[0] = 0.0f; v[2].tex[1] = fh;
    v[3] = v[1];
    v[4].pos[0] = fw;   v[4].pos[1] = fh;   v[4].tex[0] = fw;   v[4].tex[1] = fh;
    v[5] = v[2];

    GpuNv2a_SetDepthTest(0);
    GpuNv2a_SetDepthWrite(0);
    GpuNv2a_SetBlendMode(0);
    GpuNv2a_SetScissor(0, 0, 0, 0);            /* game's next PutDrawEnv restores it */
    BindTextureBpp(src, w, h, 4);
    GpuNv2a_EmitTris(v, 6);
    GpuNv2a_FlushBatch();                      /* must land before the target changes */
}

/* The extra buffer is allocated inside pb_init (see pb_extra_buffers(1) in
 * main_xbox.c), so there is nothing to reserve here -- just latch the geometry
 * and confirm the target exists. Kept as a function because FrameBegin calls it
 * once and the log line is the evidence that the feature is live. */
void GpuNv2a_FreezeReserve(void)
{
    int w = (int)pb_back_buffer_width();
    int h = (int)pb_back_buffer_height();

    if (s_freezeW || w <= 0 || h <= 0)
        return;
    s_freezeW     = w;
    s_freezeH     = h;
    s_freezeReady = (pb_extra_buffer(0) != NULL);
    SH_DBG("[FREEZE] target %s: %dx%d (GPU copy, no CPU readback)",
           s_freezeReady ? "ok" : "MISSING", w, h);
}

/* Perform a pending capture. Called from FrameBegin, AFTER pb_reset() and
 * pb_target_back_buffer() -- i.e. inside a normal frame's pushbuffer state.
 * FreezeCapture itself runs between FrameEnd and FrameBegin, where the
 * pushbuffer has not been reset yet, so it only records the request. */
static void FreezeRunPending(void)
{
    const void* src = s_lastQueuedFb;          /* the last COMPLETED frame */

    s_freezePending = 0;
    if (!src)
        return;
    /* Retarget the colour buffer to the extra surface, let the GPU copy the
     * frame into it, then restore. No pb_busy() spin and no CPU read of video
     * memory -- that is the entire 165ms. set_draw_buffer moves only the colour
     * target, so the depth buffer and all other state stay as they are. */
    {   /* The 165ms CPU readback this replaced was the largest single stall in
         * the game; keep it measured so the comparison stays honest. */
        int t0 = GpuNv2a_Ms();
        pb_target_extra_buffer(0);
        FreezeCopyQuad(src, s_freezeW, s_freezeH);
        pb_target_back_buffer();
        GpuNv2a_BindWhite();
        SH_DBG("[FREEZE] capture %dx%d in %dms (gpu, was 165ms cpu)",
               s_freezeW, s_freezeH, GpuNv2a_Ms() - t0);
    }
    s_freezeValid = 1;
}

int GpuNv2a_FreezeCapture(void)
{
    if (!s_freezeReady || !s_lastQueuedFb || s_freezeW <= 0 || s_freezeH <= 0)
        return 0;
    s_freezePending = 1;                       /* runs at the next FrameBegin */
    return 1;
}

/* Re-present the frozen frame under this frame's UI. One quad; the GPU does it
 * in parallel with the CPU instead of a per-frame multi-megabyte memcpy. */
void GpuNv2a_FreezeBlit(void)
{
    const void* src;

    if (!s_freezeReady || !s_freezeValid || s_freezeW <= 0 || s_freezeH <= 0)
        return;
    src = (const void*)pb_extra_buffer(0);
    if (!src)
        return;
    FreezeCopyQuad(src, s_freezeW, s_freezeH);
    GpuNv2a_BindWhite();
}

void GpuNv2a_FreezeRelease(void)
{
    s_freezeValid = 0;                         /* keep the buffer: see FreezeReserve */
}

/* Millisecond clock for the readback's took= probe (KeTickCount is the kernel's
 * 1ms tick counter; integer only — nxdk printf drops %f). */
int GpuNv2a_Ms(void)
{
    return (int)KeTickCount;
}

void GpuNv2a_WaitVbl(void)
{
    pb_wait_for_vbl();
}

static void SetAttribPointer(unsigned index, unsigned size, const void* data)
{
    uint32_t* p = pb_begin();
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + index * 4,
                 MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F)
                 | MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, size)
                 | MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, sizeof(ShVertex)));
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + index * 4, (uint32_t)data & 0x03ffffff);
    pb_end(p);
}

/* Bind a linear texture (texel coords, clamp, point-sampled) to stage 0.
 * bpp = 4 -> A8R8G8B8 (the decoded-page format), bpp = 2 -> R5G6B5 (the
 * freeze-frame buffer: half the memory, and the NV2A samples it natively so no
 * CPU conversion is needed). The format dword's COLOR field (bits 8..15) is the
 * only difference: 0x12 = LU_IMAGE_A8R8G8B8, 0x11 = LU_IMAGE_R5G6B5. */
static void BindTextureBpp(const void* addr, int w, int h, int bpp)
{
    uint32_t* p;
    uint32_t  fmt = (bpp == 2) ? 0x0001112a : 0x0001122a;
    GpuNv2a_FlushBatch();   /* pending run drew with the OLD texture bound */
    p = pb_begin();
    p = pb_push2(p, NV20_TCL_PRIMITIVE_3D_TX_OFFSET(0), (DWORD)addr & 0x03ffffff, fmt);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_PITCH(0), (w * bpp) << 16);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_SIZE(0), (w << 16) | h);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_WRAP(0), 0x00030303);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(0), 0x4003ffc0);
    /* PSX rasterization is nearest-neighbour (no bilinear). Point sampling is
     * correct for both 3D world textures and 2D sprites; bilinear bled adjacent
     * texels across the unpadded, page-split FONT16 atlas -> garbled text.
     * 0x01014000 = MIN nearest(1) | MAG nearest(1) | enable. (was 0x04074000 linear) */
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_FILTER(0), 0x01014000);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(1), 0x0003ffc0);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(2), 0x0003ffc0);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(3), 0x0003ffc0);
    pb_end(p);
}

void GpuNv2a_BindTexture(const void* addr, int w, int h)
{
    BindTextureBpp(addr, w, h, 4);
}

/* Restore the 1x1-white default texture (untextured prims -> colour passes through). */
void GpuNv2a_BindWhite(void)
{
    GpuNv2a_BindTexture(s_whiteTex, WHITE_TEX_DIM, WHITE_TEX_DIM);
}

/* GPU-DMA-able contiguous, write-combined memory for decoded textures. */
void* GpuNv2a_AllocTexMem(int bytes)
{
    return MmAllocateContiguousMemoryEx(bytes, 0, MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
}

/* Draw the pending run [s_runStart, s_batchUsed) as NV2A triangle lists, then
 * mark it drawn. Called at every state-change entry point (texture / blend /
 * scissor), at frame end, and before the readback drain — so the run always
 * flushes in exact OT order right before the state that would change it.
 *
 * Depth test is OFF (painter's order) and we only ever batch ADJACENT same-state
 * prims, so the framebuffer result is byte-identical to per-prim draws — just a
 * fraction of the draw calls (town was ~1000-1460 draws/frame, one BEGIN/END +
 * vertex-cache break per PSX prim).
 *
 * CRITICAL: each pb_begin/pb_end block is a COMPLETE, SELF-CONTAINED draw
 * (BREAK + BEGIN(TRIANGLES) + INDEX_DATA + END). A run larger than one block's
 * budget is split into MULTIPLE complete sub-draws — NEVER one BEGIN..END
 * spanning pb blocks, which hangs the NV2A (that was the first attempt's bug).
 * MAX_DRAW_VERTS=216 keeps a block at 7 + (216/2) = 115 dwords, well under
 * pbkit's 128-dword-per-block hard limit; 216 is a multiple of 3 so every chunk
 * boundary lands on a triangle boundary (the run is a concatenation of 3/6-vert
 * prims, so its total and every chunk are multiples of 3 — no split triangle). */
#define MAX_DRAW_VERTS 216
static void GpuNv2a_FlushBatch(void)
{
    int start = s_runStart;
    int total = s_batchUsed - s_runStart;
    int done  = 0;
    unsigned long long _cyc0;

    if (total <= 0)
        return;
    s_flushCount++;
    _cyc0 = sh_rdtsc();

    /* One store fence per draw (not per appended prim): flushes the WC pool
     * writes for every vertex in this run to global visibility before the GPU
     * DMA-reads them. ~1 fence per draw vs ~1 per prim (~958/frame) — the
     * store-buffer serialization was a real per-prim CPU cost. */
    __asm__ __volatile__("sfence" ::: "memory");

    while (done < total) {
        int       chunk = total - done;
        int       base, ndwords, i;
        uint32_t* p;

        if (chunk > MAX_DRAW_VERTS)
            chunk = MAX_DRAW_VERTS;
        base    = start + done;
        ndwords = (chunk + 1) / 2;   /* 2 indices/dword */

        p = pb_begin();
        /* Fresh vertex cache per draw (Star Fox does this before every batch);
         * indices within a draw are unique+sequential, so no intra-draw alias. */
        p = pb_push1(p, NV097_BREAK_VERTEX_BUFFER_CACHE, 0);
        p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLES);
        pb_push(p++, 0x40000000 | NV20_TCL_PRIMITIVE_3D_INDEX_DATA, ndwords);
        for (i = 0; i < ndwords; i++) {
            int a = base + i * 2;
            int b = a + 1;
            if (b >= base + chunk)
                b = base + chunk - 1;   /* odd tail: pad with last (dangling, ignored) */
            *(p++) = (uint32_t)(a & 0xFFFF) | ((uint32_t)(b & 0xFFFF) << 16);
        }
        p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
        pb_end(p);

        done += chunk;
    }

    g_Nv2aDrawCycles += sh_rdtsc() - _cyc0;
    s_runStart = s_batchUsed;
}

/* Append `count` vertices to the frame's pool. The run stays OPEN and draws as
 * one batch at the next state change / frame end (GpuNv2a_FlushBatch). Verts are
 * contiguous, so the run is a plain sequential triangle list. Per-frame draw
 * state (render state, vertex program, attribute arrays) was set once in
 * FrameBegin; texture/blend/scissor are set between runs by gpu_xbox.c. */
/* Reserve `count` vertices in the pool and hand back a pointer to write them
 * IN PLACE. Callers used to fill a local array and have EmitTris memcpy it in,
 * so every vertex crossed the CPU->write-combined boundary twice. Writing
 * directly halves that traffic; with the 64-byte stride each vertex is now a
 * single aligned line. Returns 0 if the request cannot be satisfied. */
ShVertex* GpuNv2a_BatchAlloc(int count)
{
    ShVertex* p;

    if (count <= 0 || count > MAX_BATCH_VERTS)
        return 0;
    if (s_batchUsed + count > MAX_BATCH_VERTS) {
        static int s_allocFlushLogged = 0;
        if (!s_allocFlushLogged) {
            SH_DBG("[NV2A] vertex pool flush (> %d verts/frame)", MAX_BATCH_VERTS);
            s_allocFlushLogged = 1;
        }
        GpuNv2a_FlushBatch();
        while (pb_busy()) { }      /* attribute base is fixed at s_batch[0] */
        s_batchUsed = 0;
        s_runStart  = 0;
    }
    p = s_batch + s_batchUsed;
    s_batchUsed += count;          /* sfence deferred to FlushBatch (per draw) */
    {   /* Peak watermark: proves the pool is still comfortably oversized after
         * being cut, or says plainly that it is not. */
        static int s_peak = 0;
        if (s_batchUsed > s_peak) {
            s_peak = s_batchUsed;
            if ((s_peak & 1023) == 0 || s_peak > MAX_BATCH_VERTS - 2048)
                SH_DBG("[NV2A] vertex pool peak %d/%d verts", s_peak, MAX_BATCH_VERTS);
        }
    }
    return p;
}

void GpuNv2a_EmitTris(const ShVertex* verts, int count)
{
    int start;

    if (count <= 0 || count > MAX_BATCH_VERTS)
        return;
    if (s_batchUsed + count > MAX_BATCH_VERTS) {
        /* Pool full mid-frame: draw the pending run FIRST (its verts are about to
         * be overwritten), drain the GPU (attribute base is fixed at s_batch[0]),
         * then recycle from 0 — already-drawn runs are in the framebuffer, so the
         * scene accumulates. Rare at this pool size. */
        static int s_flushLogged = 0;
        if (!s_flushLogged) { SH_DBG("[NV2A] vertex pool flush (> %d verts/frame)", MAX_BATCH_VERTS); s_flushLogged = 1; }
        GpuNv2a_FlushBatch();
        while (pb_busy()) { }
        s_batchUsed = 0;
        s_runStart  = 0;
    }

    start = s_batchUsed;
    memcpy(s_batch + start, verts, count * sizeof(ShVertex));
    s_batchUsed += count;   /* sfence deferred to GpuNv2a_FlushBatch (per draw) */
}
