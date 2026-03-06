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

#define LINE_VERT_COUNT 2
#define RECT_VERT_COUNT 4
#define BOX_VERT_COUNT  8

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

#define setRECTFast(r, x, y, w, h)        \
    ((u32*)(r))[0] = ((x) | ((y) << 16)), \
    ((u32*)(r))[1] = ((w) | ((h) << 16))

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
