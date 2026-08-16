/*
 * gpu.h - PC port override for the decomp's gpu.h
 *
 * This file includes the original gpu.h content but replaces
 * MIPS inline assembly GTE macros with C implementations.
 */
#ifndef _GPU_H
#define _GPU_H

#include "common.h"

#include <psyq/libgte.h>
#include <psyq/libgpu.h>
#include <psyq/libgs.h>
#include "psx_pack.h"

#define LINE_VERT_COUNT 2
#define RECT_VERT_COUNT 4
#define BOX_VERT_COUNT  8

/* PSX_OT_OFS is defined in include/gpu.h (the decomp's header) */

typedef enum _MaterialFlags
{
    MaterialFlag_None = 0,
    MaterialFlag_0    = 1 << 0,
    MaterialFlag_1    = 1 << 1,
    MaterialFlag_2    = 1 << 2
} e_MaterialFlags;

typedef enum _BlendMode
{
    BlendMode_Average     = 0,
    BlendMode_Additive    = 1,
    BlendMode_Subtractive = 2
} e_BlendMode;

enum PrimType
{
    PRIM_POLY = 0x20,
    PRIM_LINE = 0x40,
    PRIM_RECT = 0x60
};

enum PrimRectFlags
{
    RECT_SIZE_16  = (1 << 3) | (1 << 4),
    RECT_SIZE_8   = 1 << 4,
    RECT_SIZE_1   = 1 << 3,
    RECT_TEXTURE  = 1 << 2,
    RECT_BLEND    = 1 << 1,
    RECT_MODULATE = 1 << 0
};

typedef struct _Line2d
{
    DVECTOR vertex0_0;
    DVECTOR vertex1_4;
} s_Line2d;
STATIC_ASSERT_SIZEOF(s_Line2d, 8);

typedef struct _Triangle2d
{
    DVECTOR vertex0_0;
    DVECTOR vertex1_4;
    DVECTOR vertex2_8;
} s_Triangle2d;
STATIC_ASSERT_SIZEOF(s_Triangle2d, 12);

typedef struct _Quad2d
{
    DVECTOR vertex0_0;
    DVECTOR vertex1_4;
    DVECTOR vertex2_8;
    DVECTOR vertex3_C;
} s_Quad2d;
STATIC_ASSERT_SIZEOF(s_Quad2d, 16);

typedef struct _ColoredLine2d
{
    s_Line2d line_0;
    u16      r_8;
    u16      g_A;
    u16      b_C;
    u16      __pad_E;
} s_ColoredLine2d;
STATIC_ASSERT_SIZEOF(s_ColoredLine2d, 16);

typedef struct _LineBorder
{
    s_Line2d lines_0[RECT_VERT_COUNT];
} s_LineBorder;
STATIC_ASSERT_SIZEOF(s_LineBorder, 32);

typedef struct _QuadBorder
{
    s_Quad2d quads_0[RECT_VERT_COUNT];
} s_QuadBorder;
STATIC_ASSERT_SIZEOF(s_QuadBorder, 64);

typedef struct
{
    u8 r;
    u8 g;
    u8 b;
    u8 p;
} s_PrimColor;

#define getTPageN(tp, abr, xn, yn) \
    ((((tp) & 0x3) << 7) | (((abr) & 0x3) << 5) | (((yn) & 0x1) << 4) | ((xn) & 0xF))

/* ---------------------------------------------------------------------------
 * "Fast" primitive setters: one wide store covering several narrow fields.
 *
 * Each of these composes a word in LITTLE-ENDIAN FIELD ORDER (first field in the
 * low bits) and stores it over adjacent shorts or bytes. That is only the same
 * thing as writing the fields on a little-endian host. On big-endian the first
 * field lands in the HIGH bits, so every one of them silently swaps its fields.
 *
 * Xbox 360, first boot with a working GTE: 1320 primitives parsed per frame and
 * exactly ONE emitted. setXY*Fast had swapped x with y in every 2D primitive, so
 * the spans came out enormous (census bounding box 0..1280 on a 640-wide screen)
 * and PolyOversized -- correctly -- rejected nearly all of them.
 *
 * The big-endian variants below write the fields directly, which is what the
 * wide store was always shorthand for. The little-endian definitions are left
 * byte-for-byte unchanged so PC and Xbox codegen does not move at all.
 * ------------------------------------------------------------------------- */
#if defined(__BIG_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define SH_PRIM_FIELDWISE 1
#endif

#ifdef SH_PRIM_FIELDWISE

#define setRECTFast(r, x, y, w, h) \
    ((r)->x = (s16)(x), (r)->y = (s16)(y), (r)->w = (s16)(w), (r)->h = (s16)(h))

#else

#define setRECTFast(r, x, y, w, h)        \
    ((u32*)(r))[0] = ((x) | ((y) << 16)), \
    ((u32*)(r))[1] = ((w) | ((h) << 16))

#endif

#ifdef SH_PRIM_FIELDWISE

#define setXY0Fast(p, x, y)  ((p)->x0 = (s16)(x), (p)->y0 = (s16)(y))
#define setXY1Fast(p, x, y)  ((p)->x1 = (s16)(x), (p)->y1 = (s16)(y))
#define setXY2Fast(p, x, y)  ((p)->x2 = (s16)(x), (p)->y2 = (s16)(y))
#define setXY3Fast(p, x, y)  ((p)->x3 = (s16)(x), (p)->y3 = (s16)(y))

#define setWHFast(p, _w, _h) ((p)->w = (s16)(_w), (p)->h = (s16)(_h))

#define setUV0AndClut(p, u, v, cx, cy)     ((p)->u0 = (u8)(u), (p)->v0 = (u8)(v),      (p)->clut = (u16)((((cy) << 6) | (((cx) >> 4) & 0x3F))))

#define setUV0AndClutSum(p, u, v, clut)     ((p)->u0 = (u8)(u), (p)->v0 = (u8)(v), (p)->clut = (u16)(clut))

#define setUV1AndTPageSum(p, u, v, tpage)     ((p)->u1 = (u8)(u), (p)->v1 = (u8)(v), (p)->tpage = (u16)(tpage))

#define setUV2Sum(p, u, v)   ((p)->u2 = (u8)(u), (p)->v2 = (u8)(v))
#define setUV3Sum(p, u, v)   ((p)->u3 = (u8)(u), (p)->v3 = (u8)(v))

#define setCodeWord(p, code, rgb24)     ((p)->r0 = (u8)((rgb24) & 0xFF), (p)->g0 = (u8)(((rgb24) >> 8) & 0xFF),      (p)->b0 = (u8)(((rgb24) >> 16) & 0xFF), (p)->code = (u8)(code))

#define setRGBC0(prim, r, g, b, code)     ((prim)->r0 = (u8)(r), (prim)->g0 = (u8)(g), (prim)->b0 = (u8)(b), (prim)->code = (u8)(code))
#define setRGBC1(prim, r, g, b, code)     ((prim)->r1 = (u8)(r), (prim)->g1 = (u8)(g), (prim)->b1 = (u8)(b), (prim)->pad1 = (u8)(code))
#define setRGBC2(prim, r, g, b, code)     ((prim)->r2 = (u8)(r), (prim)->g2 = (u8)(g), (prim)->b2 = (u8)(b), (prim)->pad2 = (u8)(code))
#define setRGBC3(prim, r, g, b, code)     ((prim)->r3 = (u8)(r), (prim)->g3 = (u8)(g), (prim)->b3 = (u8)(b), (prim)->pad3 = (u8)(code))

#define setRGB0Fast(p, r, g, b) ((p)->r0 = (u8)(r), (p)->g0 = (u8)(g), (p)->b0 = (u8)(b))
#define setRGB1Fast(p, r, g, b) ((p)->r1 = (u8)(r), (p)->g1 = (u8)(g), (p)->b1 = (u8)(b))
#define setRGB2Fast(p, r, g, b) ((p)->r2 = (u8)(r), (p)->g2 = (u8)(g), (p)->b2 = (u8)(b))
#define setRGB3Fast(p, r, g, b) ((p)->r3 = (u8)(r), (p)->g3 = (u8)(g), (p)->b3 = (u8)(b))

#else

#define setXY0Fast(p, x, y) \
    *(u32*)(&(p)->x0) = (((x) & 0xFFFF) + ((y) << 16))

#define setXY1Fast(p, x, y) \
    *(u32*)(&(p)->x1) = (((x) & 0xFFFF) + ((y) << 16))

#define setXY2Fast(p, x, y) \
    *(u32*)(&(p)->x2) = (((x) & 0xFFFF) + ((y) << 16))

#define setXY3Fast(p, x, y) \
    *(u32*)(&(p)->x3) = (((x) & 0xFFFF) + ((y) << 16))

#define setWHFast(p, _w, _h) \
    *(u32*)(&(p)->w) = (((_w) & 0xFFFF) + ((_h) << 16))

#define setUV0AndClut(p, u, v, cx, cy) \
    *(u32*)(&(p)->u0) = (((((cy) << 6) | (((cx) >> 4) & 0x3F)) << 16) | ((v) << 8) | (u))

#define setUV0AndClutSum(p, u, v, clut) \
    *(u32*)(&(p)->u0) = ((u) + ((v) << 8) + ((clut) << 16))

#define setUV1AndTPageSum(p, u, v, tpage) \
    *(u32*)(&(p)->u1) = ((u) + ((v) << 8) + ((tpage) << 16))

#define setUV2Sum(p, u, v) \
    *(u16*)(&(p)->u2) = ((u) + ((v) << 8))

#define setUV3Sum(p, u, v) \
    *(u16*)(&(p)->u3) = ((u) + ((v) << 8))

#define setCodeWord(p, code, rgb24) \
    *(u32*)(&(p)->r0) = (((code) << 24) | ((rgb24) & 0xFFFFFF))

#define setRGBC0(prim, r, g, b, code) \
    *(u32*)(&(prim)->r0) = ((((r) + ((g) << 8)) + ((b) << 16)) + ((code) << 24))

#define setRGBC1(prim, r, g, b, code) \
    *(u32*)(&(prim)->r1) = ((((r) + ((g) << 8)) + ((b) << 16)) + ((code) << 24))

#define setRGBC2(prim, r, g, b, code) \
    *(u32*)(&(prim)->r2) = ((((r) + ((g) << 8)) + ((b) << 16)) + ((code) << 24))

#define setRGBC3(prim, r, g, b, code) \
    *(u32*)(&(prim)->r3) = ((((r) + ((g) << 8)) + ((b) << 16)) + ((code) << 24))

#define setRGB0Fast(p, r, g, b) \
    (*(u16*)&(p)->r0 = (r) + ((g) << 8), (p)->b0 = (b))

#define setRGB1Fast(p, r, g, b) \
    (*(u16*)&(p)->r1 = (r) + ((g) << 8), (p)->b1 = (b))

#define setRGB2Fast(p, r, g, b) \
    (*(u16*)&(p)->r2 = (r) + ((g) << 8), (p)->b2 = (b))

#define setRGB3Fast(p, r, g, b) \
    (*(u16*)&(p)->r3 = (r) + ((g) << 8), (p)->b3 = (b))

#endif /* SH_PRIM_FIELDWISE */

#define addPrimFast(ot, p, _len) \
    (setlen(p, _len), addPrim(ot, p))

#define setPolyFT4TPage(poly, tp) \
    do { \
        int tpage = (tp); \
        setPolyFT4((poly)); \
        (poly)->tpage = tpage; \
    } while(0)

extern _GsFCALL GsFCALL4;

void GsTMDfastG3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, u_long* scratch);
void GsTMDfastTG3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, u_long* scratch);
void GsTMDfastG4LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, u_long* scratch);
void GsTMDfastTG4LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, u_long* scratch);
void SetPriority(PACKET*, s32, s32);

/* Include PC GTE macro replacements */
#include "gpu_gte_pc.h"

#endif
