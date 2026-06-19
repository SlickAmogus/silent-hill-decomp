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

#define MAX_BATCH_VERTS 1024

/* NV2A linear/NPOT textures need an aligned pitch (>= 64 bytes); a 1x1 (pitch 4)
 * triggers a GPU "invalid data error". Use 64x64 (pitch 256), proven-good. */
#define WHITE_TEX_DIM 64

static ShVertex* s_batch;     /* contiguous staging pool for vertex submission */
static int       s_batchUsed; /* running offset: each draw gets its own slice so
                               * a later draw never overwrites verts the GPU is
                               * still DMA-reading from an earlier draw */
static uint32_t* s_whiteTex;  /* opaque white, for untextured prims */

static int s_frameW, s_frameH;

static void GpuNv2a_SetRenderState(void);
static void GpuNv2a_BindTexture(const void* addr, int w, int h);
static void SetAttribPointer(unsigned index, unsigned size, const void* data);

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

void GpuNv2a_Init(void)
{
    GpuNv2a_InitShader();

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
 * on (required for combiner output), no alpha-test/blend, z-clamp. */
static void GpuNv2a_SetRenderState(void)
{
    uint32_t* p = pb_begin();
    p = pb_push1(p, NV097_SET_SPECULAR_ENABLE, 1);
    p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
    p = pb_push1(p, NV097_SET_SKIN_MODE, NV097_SET_SKIN_MODE_OFF);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_DEPTH_MASK, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
    p = pb_push1(p, NV097_SET_ZMIN_MAX_CONTROL, NV097_SET_ZMIN_MAX_CONTROL_ZCLAMP_CLAMP);
    pb_end(p);
}

void GpuNv2a_FrameBegin(void)
{
    pb_wait_for_vbl();
    pb_reset();
    pb_target_back_buffer();

    s_frameW = pb_back_buffer_width();
    s_frameH = pb_back_buffer_height();

    pb_erase_depth_stencil_buffer(0, 0, s_frameW, s_frameH);
    pb_fill(0, 0, s_frameW, s_frameH, 0xff000000);
    pb_erase_text_screen();

    s_batchUsed = 0; /* recycle the vertex pool each frame */

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

        SetAttribPointer(0, 3, &s_batch[0].pos);
        SetAttribPointer(3, 4, &s_batch[0].col);
        SetAttribPointer(9, 2, &s_batch[0].tex);
    }
}

void GpuNv2a_FrameEnd(void)
{
    while (pb_busy()) { }
    while (pb_finished()) { }
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

/* Bind a linear A8R8G8B8 texture (texel coords, clamp, bilinear) to stage 0. */
static void GpuNv2a_BindTexture(const void* addr, int w, int h)
{
    uint32_t* p = pb_begin();
    p = pb_push2(p, NV20_TCL_PRIMITIVE_3D_TX_OFFSET(0), (DWORD)addr & 0x03ffffff, 0x0001122a);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_PITCH(0), (w * 4) << 16);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_SIZE(0), (w << 16) | h);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_WRAP(0), 0x00030303);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(0), 0x4003ffc0);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_FILTER(0), 0x04074000);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(1), 0x0003ffc0);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(2), 0x0003ffc0);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(3), 0x0003ffc0);
    pb_end(p);
}

/* Append `count` vertices to the frame's pool and draw them as a triangle list.
 * Per-frame draw state (render state, texture, vertex program, attribute arrays)
 * was set once in FrameBegin; here we ONLY copy the verts and issue the draw,
 * selecting this draw's slice via DRAW_ARRAYS START_INDEX. Untextured (white
 * texture bound), so the per-vertex colour shows through unchanged. */
void GpuNv2a_EmitTris(const ShVertex* verts, int count)
{
    uint32_t* p;
    int       start;

    if (count <= 0 || count > MAX_BATCH_VERTS)
        return;
    if (s_batchUsed + count > MAX_BATCH_VERTS)
        return; /* pool full this frame — drop (don't corrupt in-flight verts) */

    start = s_batchUsed;
    memcpy(s_batch + start, verts, count * sizeof(ShVertex));
    __asm__ __volatile__("sfence" ::: "memory");
    s_batchUsed += count;

    /* Draw via the INDEX_DATA (ARRAY_ELEMENT16) method — two 16-bit vertex
     * indices per dword — referencing this draw's slice [start, start+count).
     * (DRAW_ARRAYS with a non-zero START_INDEX misrenders on this NV2A; the
     * mesh sample uses INDEX_DATA for multi-batch draws and it works.) */
    {
        int ndwords = (count + 1) / 2; /* 2 indices/dword, round up */
        int i;
        p = pb_begin();
        /* Invalidate the NV2A vertex cache so this draw fetches fresh verts
         * instead of reusing a previous draw's cached (stale) vertices — the
         * cause of the 2nd-draw-per-frame corruption. (Star Fox does this before
         * every batch.) */
        p = pb_push1(p, NV097_BREAK_VERTEX_BUFFER_CACHE, 0);
        p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLES);
        pb_push(p++, 0x40000000 | NV20_TCL_PRIMITIVE_3D_INDEX_DATA, ndwords);
        for (i = 0; i < ndwords; i++) {
            int a = start + i * 2;
            int b = start + i * 2 + 1;
            if (b >= start + count)
                b = start + count - 1; /* odd count: pad with last (dangling, ignored) */
            *(p++) = (uint32_t)(a & 0xFFFF) | ((uint32_t)(b & 0xFFFF) << 16);
        }
        p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
        pb_end(p);
    }
}
