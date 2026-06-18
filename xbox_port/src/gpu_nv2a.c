/*
 * gpu_nv2a.c - NV2A rendering backend (pbkit) for the Silent Hill Xbox port.
 *
 * Milestone 2a: prove the texture + register-combiner pipeline by drawing one
 * textured triangle in 2D screen space (the form PSX libgpu primitives take
 * after the software GTE projects them). The vertex shader (vs.vs.cg) maps
 * screen pixels -> clip space; the pixel shader (ps.ps.cg) does texture *
 * diffuse. This infrastructure (2D-ortho VS, modulate combiner, texture upload,
 * vertex submission) is the foundation of the real DrawOTag (milestone 3).
 *
 * pbkit sequences mirror nxdk's `mesh` sample (proven register values).
 */
#include <pbkit/pbkit.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdint.h>
#include <string.h>

#include "sh_log.h"

#define MAXRAM 0x03FFAFFF
#define MASK(mask, val) (((val) << (__builtin_ffs(mask) - 1)) & (mask))

/* Screen-space vertex: position in pixels, diffuse colour, texcoord. Matches
 * the vs.vs.cg input layout (attribs POSITION=0, DIFFUSE=3, TEXCOORD0=9). */
#pragma pack(1)
typedef struct {
    float pos[3];
    float col[4];
    float tex[2];
} ShVertex;
#pragma pack()

#define TEX_DIM 64

static uint32_t* s_vertices;
static uint32_t* s_indices;
static unsigned  s_numIndices;

static struct {
    uint16_t width;
    uint16_t height;
    uint16_t pitch;
    void*    addr;
} s_texture;

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

    /* Fragment shader (register combiners) */
    p = pb_begin();
    #include "ps.inl"
    pb_end(p);
}

static void GpuNv2a_InitTexture(void)
{
    uint32_t* tex;
    int x, y;

    s_texture.width  = TEX_DIM;
    s_texture.height = TEX_DIM;
    s_texture.pitch  = TEX_DIM * 4;
    s_texture.addr   = MmAllocateContiguousMemoryEx(s_texture.pitch * s_texture.height, 0, MAXRAM, 0,
                                                    PAGE_READWRITE | PAGE_WRITECOMBINE);
    tex = (uint32_t*)s_texture.addr;

    /* 8x8 magenta / cyan checkerboard so a correct render is unmistakable. */
    for (y = 0; y < TEX_DIM; y++) {
        for (x = 0; x < TEX_DIM; x++) {
            int on = ((x >> 3) ^ (y >> 3)) & 1;
            tex[y * TEX_DIM + x] = on ? 0xffff00ff : 0xff00ffff; /* ARGB */
        }
    }
}

void GpuNv2a_Init(void)
{
    static const ShVertex verts[3] = {
        /* pos(px)            col(rgba)            tex      */
        { { 320.0f,  80.0f, 0.0f }, { 1, 1, 1, 1 }, { 0.5f, 0.0f } },
        { { 120.0f, 400.0f, 0.0f }, { 1, 1, 1, 1 }, { 0.0f, 1.0f } },
        { { 520.0f, 400.0f, 0.0f }, { 1, 1, 1, 1 }, { 1.0f, 1.0f } },
    };
    static const uint32_t indices[3] = { 0, 1, 2 };

    GpuNv2a_InitShader();
    GpuNv2a_InitTexture();

    s_vertices = MmAllocateContiguousMemoryEx(sizeof(verts), 0, MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    memcpy(s_vertices, verts, sizeof(verts));
    s_indices = MmAllocateContiguousMemoryEx(sizeof(indices), 0, MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    memcpy(s_indices, indices, sizeof(indices));
    s_numIndices = sizeof(indices) / sizeof(indices[0]);

    SH_DBG("[SH-XBOX] NV2A backend init: shaders loaded, %dx%d checker texture, %u-index test tri",
           TEX_DIM, TEX_DIM, s_numIndices);
}

static void SetAttribPointer(unsigned index, unsigned format, unsigned size, unsigned stride, const void* data)
{
    uint32_t* p = pb_begin();
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + index * 4,
                 MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE, format)
                 | MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, size)
                 | MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, stride));
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + index * 4, (uint32_t)data & 0x03ffffff);
    pb_end(p);
}

/* Render state for a 2D blit path. NV2A defaults (and what the 3D mesh sample
 * relied on) are not all right for flat 2D screen-space prims, so set them
 * explicitly: no depth test (z is flat), no culling (winding-independent),
 * z-clamp (never drop a prim on near/far), specular-enable on (required for
 * register-combiner / final-combiner output), no alpha test / blend. */
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

void GpuNv2a_DrawTestTriangle(void)
{
    uint32_t* p;
    int       i;

    GpuNv2a_SetRenderState();

    /* Upload the vertex-program literal constants. vp20 does NOT embed float
     * literals in the program — the compiler emits them as c[] constants that
     * must be uploaded to the hardware constant bank (program c[0] maps to
     * hardware slot 96). Check the `// const c[0] = ...` line in vs.inl after
     * editing the shader; without this upload the transform reads garbage. */
    {
        static const float c0[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        p = pb_begin();
        p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_VP_UPLOAD_CONST_ID, 96);
        pb_push(p++, NV20_TCL_PRIMITIVE_3D_VP_UPLOAD_CONST_X, 4);
        memcpy(p, c0, sizeof(c0));
        p += 4;
        pb_end(p);
    }

    /* Texture stage 0: linear A8R8G8B8 (0x12), clamp, bilinear. */
    p = pb_begin();
    p = pb_push2(p, NV20_TCL_PRIMITIVE_3D_TX_OFFSET(0), (DWORD)s_texture.addr & 0x03ffffff, 0x0001122a);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_PITCH(0), s_texture.pitch << 16);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_NPOT_SIZE(0), (s_texture.width << 16) | s_texture.height);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_WRAP(0), 0x00030303);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(0), 0x4003ffc0);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_FILTER(0), 0x02022000);
    pb_end(p);

    /* Disable texture stages 1-3. */
    p = pb_begin();
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(1), 0x0003ffc0);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(2), 0x0003ffc0);
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_TX_ENABLE(3), 0x0003ffc0);
    pb_end(p);

    /* Clear all 16 vertex attribute arrays (format 2 = none). */
    p = pb_begin();
    pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16);
    for (i = 0; i < 16; i++) {
        *(p++) = 2;
    }
    pb_end(p);

    SetAttribPointer(0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, sizeof(ShVertex), &s_vertices[0]);
    SetAttribPointer(3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4, sizeof(ShVertex), &((char*)s_vertices)[12]);
    SetAttribPointer(9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof(ShVertex), &((char*)s_vertices)[28]);

    p = pb_begin();
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLES);
    pb_push(p++, 0x40000000 | NV20_TCL_PRIMITIVE_3D_INDEX_DATA, s_numIndices);
    memcpy(p, &s_indices[0], s_numIndices * sizeof(uint32_t));
    p += s_numIndices;
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    pb_end(p);
}
