#ifndef _GPU_H
#define _GPU_H

#include "common.h"

#include <psyq/libgte.h>
#include <psyq/libgpu.h>
#include <psyq/libgs.h>

#include "psx_pack.h"

#ifdef SH_PC_PORT
/* PsyCross uses dfe (draw-to-framebuffer-enable) to decide on-screen vs off-screen
 * rendering. On real PSX hardware, dfe only controls display during the draw phase
 * (for interlace flicker reduction) — primitives always render to the framebuffer.
 * Force dfe=1 so all primitives render on-screen. */
#undef _get_mode
#define _get_mode(dfe, dtd, tpage) \
    ((0xe1000000)|((dtd)?0x0200:0)|0x0400|((tpage)&0x9ff))

#undef setDrawTPage
#define setDrawTPage(p, dfe, dtd, tpage) \
    setlen(p, 1), \
    ((p)->code[0] = _get_mode(1, dtd, tpage))

#undef setDrawMode
#define setDrawMode(p, dfe, dtd, tpage, tw) \
    setlen(p, 3), \
    ((p)->code[0] = _get_mode(1, dtd, tpage)), \
    ((p)->code[1] = _get_tw((RECT16 *)tw))
#endif

#ifdef SH_PC_PORT
/*
 * PSX OT byte offset conversion.
 * On PSX, OT entries (GsOT_TAG) are 4 bytes. Game code uses raw byte offsets
 * like (u8*)ot->org + 24 to reach entry 6. On 64-bit PC, entries are 12 bytes,
 * so byte offsets must be scaled.
 */
#define PSX_OT_OFS(n) (((n) / 4) * (int)sizeof(GsOT_TAG))
#endif

#define LINE_VERT_COUNT 2
#define RECT_VERT_COUNT 4
#define BOX_VERT_COUNT  8

/** @brief Model primitive material flags. */
typedef enum _MaterialFlags
{
    MaterialFlag_None = 0,
    MaterialFlag_0    = 1 << 0,
    MaterialFlag_1    = 1 << 1,
    MaterialFlag_2    = 1 << 2
} e_MaterialFlags;

/** @brief Semi-transparency blend modes. */
typedef enum _BlendMode
{
    BlendMode_Average     = 0,
    BlendMode_Additive    = 1,
    BlendMode_Subtractive = 2
} e_BlendMode;

/** @brief Primitive flags.
 * TODO: Could split these into 3 enums and create a `PRIM_CODE` macro for convenient packing.
 */
typedef enum _PrimitiveFlags
{
    RECT_MODULATE = 1 << 0, /** Use primitive color to modulate texture. */
    RECT_BLEND    = 1 << 1, /** Semi-transparency flag. */
    RECT_TEXTURE  = 1 << 2, /** Rectangle is textured (`SPRT`). */

    RECT_SIZE_1   = 1 << 3, /** Rectangle is 1x1 (`TILE_1`). */
    RECT_SIZE_8   = 2 << 3, /** Rectangle is 8x8 (`TILE_8` or `SPRT_8`). */
    RECT_SIZE_16  = 3 << 3, /** Rectangle is 16x16 (`TILE_16` or `SPRT_16`). */

    PRIM_POLY     = 1 << 5, /** Polygon (`POLY`). */
    PRIM_LINE     = 2 << 5, /** Line (`LINE`). */
    PRIM_RECT     = 3 << 5  /** Rectangle (`TILE` or `SPRT`). */
} e_PrimitiveFlags;

/** @brief 2D screen-space line. */
typedef struct _Line2d
{
    /* 0x0 */ DVECTOR vertex0;
    /* 0x4 */ DVECTOR vertex1;
} s_Line2d;
STATIC_ASSERT_SIZEOF(s_Line2d, 8);

/** @brief 2D screen-space triangle. */
typedef struct _Triangle2d
{
    /* 0x0 */ DVECTOR vertex0;
    /* 0x4 */ DVECTOR vertex1;
    /* 0x8 */ DVECTOR vertex2;
} s_Triangle2d;
STATIC_ASSERT_SIZEOF(s_Triangle2d, 12);

/** @brief 2D screen-space quad. */
typedef struct _Quad2d
{
    /* 0x0 */ DVECTOR vertex0;
    /* 0x4 */ DVECTOR vertex1;
    /* 0x8 */ DVECTOR vertex2;
    /* 0xC */ DVECTOR vertex3;
} s_Quad2d;
STATIC_ASSERT_SIZEOF(s_Quad2d, 16);

/** @brief Colored 2D screen-space line. */
typedef struct _ColoredLine2d
{
    /* 0x0 */ s_Line2d line;
    /* 0x8 */ u16      r;
    /* 0xA */ u16      g;
    /* 0xC */ u16      b;
    /* 0xE */ u16      __pad_E; // Maybe 4th component of an RGB+code struct?
} s_ColoredLine2d;
STATIC_ASSERT_SIZEOF(s_ColoredLine2d, 16);

/** @brief 2D screen-space line border. */
typedef struct _LineBorder
{
    /* 0x0 */ s_Line2d lines[RECT_VERT_COUNT];
} s_LineBorder;
STATIC_ASSERT_SIZEOF(s_LineBorder, 32);

/** @brief 2D screen-space quad border. */
typedef struct _QuadBorder
{
    /* 0x0 */ s_Quad2d quads[RECT_VERT_COUNT];
} s_QuadBorder;
STATIC_ASSERT_SIZEOF(s_QuadBorder, 64);

/** @brief Primitive color. */
typedef struct _PrimColor
{
    u8 r;
    u8 g;
    u8 b;
    u8 p;
} s_PrimColor;

/** @brief Same as `getTPage`, but `xn` and `yn` are indices instead of VRAM coordinates. */
#define getTPageN(tp, abr, xn, yn) \
    ((((tp) & 0x3) << 7) | (((abr) & 0x3) << 5) | (((yn) & 0x1) << 4) | ((xn) & 0xF))

/* ---------------------------------------------------------------------------
 * SH_PRIM_FIELDWISE
 *
 * The "Fast" macros below pack two or four fields into one wide store. That is
 * only ever valid on a little-endian target whose primitive structs have the
 * PSX's exact layout, and it is wrong TWICE over anywhere else:
 *
 *   - Byte order. `(x & 0xFFFF) + (y << 16)` puts y in the HIGH half, which on
 *     a big-endian machine is where x0 lives. The Xbox 360 port hit exactly
 *     this: 1320 primitives parsed per frame and one emitted, because every 2D
 *     primitive had x swapped with y and the oversize check rejected them.
 *
 *   - Alignment. This port's primitives are larger than the PSX's, so a field
 *     the PSX had 4-aligned may not be. &SPRT.r0 lands at offset 11 here, and
 *     MIPS does not fix up a misaligned sw -- it traps. On N64 that is an
 *     immediate CPU exception in the Konami logo, which is how this was found.
 *
 * pc_port/include/gpu.h already carried this fix, but it is a DIFFERENT header:
 * include/game.h does #include "gpu.h", and a quoted include searches the
 * includer's own directory first, so all of src/ gets THIS file. Every game TU
 * was therefore still using the packed versions.
 *
 * The little-endian definitions are left byte-for-byte unchanged so PC codegen
 * does not move at all.
 * ------------------------------------------------------------------------- */
#if !defined(SH_PRIM_FIELDWISE) && \
    (defined(__BIG_ENDIAN__) || \
     (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__))
#define SH_PRIM_FIELDWISE 1
#endif

/** @brief Same as `setRECT`, but uses 2x 32-bit stores instead of 4x 16-bit stores. */
#ifdef SH_PRIM_FIELDWISE
/* Parameters are _x/_y/_w/_h: named x/y/w/h they get substituted into the field
 * accesses (r)->x ... and turn every call into (r)-><whatever was passed>. */
#define setRECTFast(r, _x, _y, _w, _h) \
    ((r)->x = (s16)(_x), (r)->y = (s16)(_y), (r)->w = (s16)(_w), (r)->h = (s16)(_h))
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

#define setUV0AndClut(p, u, v, cx, cy) \
    ((p)->u0 = (u8)(u), (p)->v0 = (u8)(v), \
     (p)->clut = (u16)((((cy) << 6) | (((cx) >> 4) & 0x3F))))

/* _clut / _tpage for the same reason: a parameter named clut turns the field
 * access (p)->clut into (p)->getClut(...) at every call site that passes one. */
#define setUV0AndClutSum(p, u, v, _clut) \
    ((p)->u0 = (u8)(u), (p)->v0 = (u8)(v), (p)->clut = (u16)(_clut))

#define setUV1AndTPageSum(p, u, v, _tpage) \
    ((p)->u1 = (u8)(u), (p)->v1 = (u8)(v), (p)->tpage = (u16)(_tpage))

#define setUV2Sum(p, u, v)   ((p)->u2 = (u8)(u), (p)->v2 = (u8)(v))
#define setUV3Sum(p, u, v)   ((p)->u3 = (u8)(u), (p)->v3 = (u8)(v))

/* The parameter is _code, NOT code: a macro parameter is substituted even after
 * `->`, so a parameter named `code` turns the field access `(p)->code` into
 * `(p)->PRIM_RECT|RECT_TEXTURE` at every call site. pc_port/include/gpu.h has
 * the same latent bug in its copy; it just never gets exercised, because these
 * callers all resolve to THIS header. */
#define setCodeWord(p, _code, rgb24)                      \
    ((p)->r0 = (u8)((rgb24) & 0xFF),                      \
     (p)->g0 = (u8)(((rgb24) >> 8) & 0xFF),               \
     (p)->b0 = (u8)(((rgb24) >> 16) & 0xFF),              \
     (p)->code = (u8)(_code))

#define setRGBC0(prim, r, g, b, _code) \
    ((prim)->r0 = (u8)(r), (prim)->g0 = (u8)(g), (prim)->b0 = (u8)(b), (prim)->code = (u8)(_code))
/* The fourth byte of each colour quad is written POSITIONALLY, not by name. It
 * is padding, and the primitives disagree on what to call it: POLY_G4 says
 * pad1/pad2/pad3, LINE_G2 and POLY_GT4 say p1/p2/p3. A byte store also cannot
 * trap, and on this target address order is field order, which is precisely
 * what the PSX's little-endian packed store produced. */
#define setRGBC1(prim, r, g, b, _code) \
    ((prim)->r1 = (u8)(r), (prim)->g1 = (u8)(g), (prim)->b1 = (u8)(b), \
     ((u8*)&(prim)->r1)[3] = (u8)(_code))
#define setRGBC2(prim, r, g, b, _code) \
    ((prim)->r2 = (u8)(r), (prim)->g2 = (u8)(g), (prim)->b2 = (u8)(b), \
     ((u8*)&(prim)->r2)[3] = (u8)(_code))
#define setRGBC3(prim, r, g, b, _code) \
    ((prim)->r3 = (u8)(r), (prim)->g3 = (u8)(g), (prim)->b3 = (u8)(b), \
     ((u8*)&(prim)->r3)[3] = (u8)(_code))

#define setRGB0Fast(p, r, g, b) \
    ((p)->r0 = (u8)(r), (p)->g0 = (u8)(g), (p)->b0 = (u8)(b))

#else /* !SH_PRIM_FIELDWISE */

/** @brief Same as `setXY0`, but uses 1x 32-bit store instead of 2x 16-bit stores. */
#define setXY0Fast(p, x, y) \
    *(u32*)(&(p)->x0) = (((x) & 0xFFFF) + ((y) << 16))

#define setXY1Fast(p, x, y) \
    *(u32*)(&(p)->x1) = (((x) & 0xFFFF) + ((y) << 16))

#define setXY2Fast(p, x, y) \
    *(u32*)(&(p)->x2) = (((x) & 0xFFFF) + ((y) << 16))

#define setXY3Fast(p, x, y) \
    *(u32*)(&(p)->x3) = (((x) & 0xFFFF) + ((y) << 16))

/** @brief Same as `setWH`, but uses 1x 32-bit store instead of 2x 16-bit stores. */
#define setWHFast(p, _w, _h) \
    *(u32*)(&(p)->w) = (((_w) & 0xFFFF) + ((_h) << 16))

/** @brief Combines `setUV0` and `setClut` into a single 32-bit stores. Also doesn't call `GetClut`. */
#define setUV0AndClut(p, u, v, cx, cy) \
    *(u32*)(&(p)->u0) = (((((cy) << 6) | (((cx) >> 4) & 0x3F)) << 16) | ((v) << 8) | (u))

/** @brief Combines `setUV0` and `setClut` into a single 32-bit store, using ADD to combine `u` and `v`. */
#define setUV0AndClutSum(p, u, v, clut) \
    *(u32*)(&(p)->u0) = ((u) + ((v) << 8) + ((clut) << 16))

/** @brief Combines `setUV1` and `setClut` into a single 32-bit store, using ADD to combine `u` and `v`. */
#define setUV1AndTPageSum(p, u, v, tpage) \
    *(u32*)(&(p)->u1) = ((u) + ((v) << 8) + ((tpage) << 16))

/** @brief Same as `setUV2`, using ADD to combine `u` and `v`. */
#define setUV2Sum(p, u, v) \
    *(u16*)(&(p)->u2) = ((u) + ((v) << 8))

/** @brief Same as `setUV3`, using ADD to combine `u` and `v`. */
#define setUV3Sum(p, u, v) \
    *(u16*)(&(p)->u3) = ((u) + ((v) << 8))

/** @brief Combines `setcode` and `setRGB0`. */
#ifdef SH_PC_PORT
/* PsyCross uses 12-byte P_TAG header, so offset +4 is wrong (writes into addr).
 * Use struct field access instead. */
#define setCodeWord(p, code, rgb24) \
    *(u32*)(&(p)->r0) = (((code) << 24) | ((rgb24) & 0xFFFFFF))
#else
#define setCodeWord(p, code, rgb24) \
    *(u32*)(((u8*)(p)) + 4) = (((code) << 24) | ((rgb24) & 0xFFFFFF))
#endif

// TODO: Perhaps `setRGBC0`, `setRGBC1`, `setRGBC2`, and `setRGBC3` were one macro. Incidental value set to padding fields suggests it?
/** @brief Combines `setRGB0` and `setcode`. */
#define setRGBC0(prim, r, g, b, code) \
    *(u32*)(&(prim)->r0) = ((((r) + ((g) << 8)) + ((b) << 16)) + ((code) << 24))

/** @brief Combines `setRGB1` and incidentally applies code to the padding component. */
#define setRGBC1(prim, r, g, b, code) \
    *(u32*)(&(prim)->r1) = ((((r) + ((g) << 8)) + ((b) << 16)) + ((code) << 24))

/** @brief Combines `setRGB2` and incidentally applies code to the padding component. */
#define setRGBC2(prim, r, g, b, code) \
    *(u32*)(&(prim)->r2) = ((((r) + ((g) << 8)) + ((b) << 16)) + ((code) << 24))

/** @brief Combines `setRGB3` and incidentally applies code to the padding component. */
#define setRGBC3(prim, r, g, b, code) \
    *(u32*)(&(prim)->r3) = ((((r) + ((g) << 8)) + ((b) << 16)) + ((code) << 24))

/** @brief Slightly faster `setRGB0`. */
#define setRGB0Fast(p, r, g, b) \
    (*(u16*)&(p)->r0 = (r) + ((g) << 8), (p)->b0 = (b))

#endif /* SH_PRIM_FIELDWISE */

/* ---------------------------------------------------------------------------
 * PACKED-WORD writers.
 *
 * The decomp writes primitive field GROUPS as one 32-bit store in ~50 places,
 * e.g. `*(u32*)&poly->r0 = color` or `*(u32*)&sprt->u0 = u | (v<<8) | (clut<<16)`.
 * That is the PSX's own idiom and it is exact on a little-endian machine whose
 * primitives have the PSX's layout. It is wrong TWICE anywhere else:
 *
 *   - Byte order. On big-endian the low byte of the word lands at the HIGHEST
 *     address, so a colour word puts `code` where `r0` lives and a UV word puts
 *     the CLUT where `u0` lives. Every texture coordinate comes out garbage,
 *     which is what "the logos draw white and untextured" actually was.
 *
 *   - Alignment. This port's primitives are larger than the PSX's, so a field
 *     the PSX had 4-aligned may not be. &POLY_G4.r0 sits at offset 11 here, and
 *     MIPS traps a misaligned sw rather than fixing it up -- an immediate CPU
 *     exception in the title screen's fog.
 *
 * These decompose the packed word into the same bytes the little-endian store
 * would have produced, then write them as fields. Use them instead of the cast.
 *
 * The word argument is evaluated more than once, which is safe for every call
 * site in the tree today -- they are all side-effect-free arithmetic on locals.
 * Check that before using one on anything with a side effect.
 * ------------------------------------------------------------------------- */

/** @brief `*(u32*)&p->r0 = w`, portably: r0,g0,b0,code. */
#define setRGBCWord0(p, w)     setRGBC0(p, (w) & 0xFF, ((w) >> 8) & 0xFF, ((w) >> 16) & 0xFF, ((w) >> 24) & 0xFF)
#define setRGBCWord1(p, w)     setRGBC1(p, (w) & 0xFF, ((w) >> 8) & 0xFF, ((w) >> 16) & 0xFF, ((w) >> 24) & 0xFF)
#define setRGBCWord2(p, w)     setRGBC2(p, (w) & 0xFF, ((w) >> 8) & 0xFF, ((w) >> 16) & 0xFF, ((w) >> 24) & 0xFF)
#define setRGBCWord3(p, w)     setRGBC3(p, (w) & 0xFF, ((w) >> 8) & 0xFF, ((w) >> 16) & 0xFF, ((w) >> 24) & 0xFF)

/** @brief `*(u32*)&p->u0 = w`, portably: u0,v0,clut (clut is the upper 16). */
#define setUV0ClutWord(p, w)     setUV0AndClutSum(p, (w) & 0xFF, ((w) >> 8) & 0xFF, ((w) >> 16) & 0xFFFF)

/** @brief `*(u32*)&p->u1 = w`, portably: u1,v1,tpage (tpage is the upper 16). */
#define setUV1TPageWord(p, w)     setUV1AndTPageSum(p, (w) & 0xFF, ((w) >> 8) & 0xFF, ((w) >> 16) & 0xFFFF)

/** @brief `*(u32*)&p->w = w`, portably: w = low half, h = high half. */
#define setWHWord(p, _w) setWHFast(p, (_w) & 0xFFFF, ((_w) >> 16) & 0xFFFF)

/** @brief `*(u32*)&p->x0 = w`, portably: x0 = low half, y0 = high half. */
#define setXY0Word(p, _w) setXY0Fast(p, (s16)((_w) & 0xFFFF), (s16)(((_w) >> 16) & 0xFFFF))

/** @brief `*(u16*)&p->u2 = w` / `u3`, portably. */
#define setUV2Word(p, w) setUV2Sum(p, (w) & 0xFF, ((w) >> 8) & 0xFF)
#define setUV3Word(p, w) setUV3Sum(p, (w) & 0xFF, ((w) >> 8) & 0xFF)

#define setRGB1Fast(p, r, g, b) \
    (*(u16*)&(p)->r1 = (r) + ((g) << 8), (p)->b1 = (b))

#define setRGB2Fast(p, r, g, b) \
    (*(u16*)&(p)->r2 = (r) + ((g) << 8), (p)->b2 = (b))

#define setRGB3Fast(p, r, g, b) \
    (*(u16*)&(p)->r3 = (r) + ((g) << 8), (p)->b3 = (b))

/** @brief Combines `addPrim` and `setlen`. */
#ifdef SH_PC_PORT
#define addPrimFast(ot, p, _len) \
    (setlen(p, _len), addPrim(ot, p))
#else
#define addPrimFast(ot, p, _len) \
    (((p)->tag = getaddr(ot) | ((_len) << 24)), setaddr(ot, p))
#endif

/** @brief Combines `setPolyFT4` with `tpage` setter.
  * @hack Needed to allow `tpage` and POLY_FT4 code `0x2C` to be merged in some cases.
*/
#define setPolyFT4TPage(poly, tp) \
({ \
    s32 tpage = (tp); \
    setPolyFT4((poly)); \
    (poly)->tpage = tpage; \
})

extern _GsFCALL GsFCALL4;

void GsTMDfastG3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, u_long* scratch);
void GsTMDfastTG3LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, u_long* scratch);
void GsTMDfastG4LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, u_long* scratch);
void GsTMDfastTG4LFG(void* op, VERT* vp, VERT* np, PACKET* pk, int n, int shift, GsOT* ot, u_long* scratch);
void SetPriority(PACKET*, s32, s32);

#ifdef SH_PC_PORT
/* PC port: use PsyCross C-based GTE register access instead of MIPS asm */
#include "gpu_gte_pc.h"
#else

/** @brief Sets the `DQA` register in the GTE. Not part of Psy-Q for some reason. */
#define gte_lddqa(r0) __asm__ volatile( \
    "ctc2  %0, $27;"                    \
    :                                   \
    : "r"(r0))

/** @brief Zeroes the `DQB` register in the GTE. */
#define gte_lddqb_0() __asm__ volatile( \
    "ctc2  $zero, $28;")

/** @brief Zeroes transfer vector in the GTE. */
#define gte_ldtr_0() __asm__ volatile( \
    "ctc2  $zero, $5;"                 \
    "ctc2  $zero, $6;"                 \
    "ctc2  $zero, $7;")

/** @brief Loads `SVECTOR` into light matrix, similar to `gte_SetLightMatrix`. */
#define gte_SetLightSVector(p) __asm__ volatile( \
    "lw    $12, 0(%0);"                          \
    "lhu   $13, 4(%0);"                          \
    "ctc2  $12, $8;"                             \
    "ctc2  $13, $9;"                             \
    "ctc2  $zero, $10;"                          \
    "ctc2  $zero, $11;"                          \
    "ctc2  $zero, $12;"                          \
    :                                            \
    : "r"(p)                                     \
    : "$12", "$13", "memory");

/** @brief Broadcasts a single value into the GTE universal vector, similar to `gte_ldsv`. */
#define gte_ldsv_(val) __asm__ volatile( \
    "mtc2  %0, $9;"                      \
    "mtc2  %0, $10;"                     \
    "mtc2  %0, $11;"                     \
    :                                    \
    : "r"(val)                           \
    : "memory");

/** @brief Returns the value of the GTE `IR1` register. */
#define gte_stIR1()                                        \
    ({                                                     \
        u32 __r;                                           \
        __asm__ volatile("mfc2 %0, $9; nop;" : "=r"(__r)); \
        __r;                                               \
    })

/** @brief Loads `SVECTOR` into GTE Vector 0. */
#define gte_SetVector0(p) __asm__ volatile( \
    "lw    $12, 0(%0);"                     \
    "lhu   $13, 4(%0);"                     \
    "mtc2  $12, $0;"                        \
    "mtc2  $13, $1;"                        \
    :                                       \
    : "r"(p)                                \
    : "$12", "$13", "memory")

/** @brief Loads GTE light source vector `x` and `y`. */
#define gte_SetLightSourceXY(x, y) __asm__ volatile( \
    "sll  %0, %0, 16;"                               \
    "srl  %0, %0, 16;"                               \
    "sll  %1, %1, 16;"                               \
    "or   %0, %0, %1;"                               \
    "ctc2 %0, $8;"                                   \
    :                                                \
    : "r"(x), "r"(y))

/** @brief Loads GTE light source vector `z`. */
#define gte_SetLightSourceZ(z) __asm__ volatile( \
    "ctc2  %0, $9;"                              \
    "ctc2  $zero, $10;"                          \
    "ctc2  $zero, $11;"                          \
    "ctc2  $zero, $12;"                          \
    :                                            \
    : "r"((z) & 0xFFFF))

/** @brief Loads row 0 and row 1 from `MATRIX` into GTE rotation matrix. */
#define gte_SetRotMatrix_Row0_1(r0) __asm__ volatile( \
    "lw   $12, 0( %0 );"                              \
    "lw   $13, 4( %0 );"                              \
    "ctc2 $12, $0;"                                   \
    "lw   $12, 8( %0 );"                              \
    "ctc2 $13, $1;"                                   \
    "ctc2 $12, $2"                                    \
    :                                                 \
    : "r"(r0)                                         \
    : "$12", "$13")

/** @brief Loads row 2 from `MATRIX` into GTE rotation matrix. */
#define gte_SetRotMatrix_Row2(r0) __asm__ volatile( \
    "lw   $12, 12( %0 );"                           \
    "lw   $13, 16( %0 );"                           \
    "ctc2 $12, $3;"                                 \
    "ctc2 $13, $4;"                                 \
    :                                               \
    : "r"(r0)                                       \
    : "$12", "$13")

#define gte_LoadVector0_1_2_XYZ(xy, z) __asm__ volatile( \
    /* Load vector 0. */ \
    "lhu   $12, 0(%1);" /* CPU $12   = Z0 (unsigned 16-bit from `z[0]`) */ \
    "lwc2  $0, 0(%0);"  /* COP2 VXY0 = X0,Y0 (packed 2x16-bit from `xy[0]`) */ \
    "mtc2  $12, $1;"    /* COP2 VZ0  = Z0 (low 16 bits used) */ \
    /* Load vector 1. */ \
    "lhu   $12, 2(%1);" /* CPU $12   = Z1 */ \
    "lwc2  $2, 4(%0);"  /* COP2 VXY1 = X1,Y1 */ \
    "mtc2  $12, $3;"    /* COP2 VZ1  = Z1 */ \
    /* Load vector 2. */ \
    "lhu   $12, 4(%1);" /* CPU $12   = Z2 */ \
    "lwc2  $4, 8(%0);"  /* COP2 VXY2 = X2,Y2 */ \
    "mtc2  $12, $5;"    /* COP2 VZ2  = Z2 */ \
    : \
    : "r"(xy), "r"(z) \
    : "$12", "memory")

#define gte_FetchScreen0_1_2_XYZ(xy, z) __asm__ volatile( \
    /* Vertex 0. */ \
    "mfc2  $12, $17;"   /* CPU $12 = SZ1 (depth for vertex 0) */ \
    "swc2  $12, 0(%0);" /* COP2 SXY0 (reg 12) -> xy[0] (packed X0, Y0) */ \
    "sh    $12, 0(%1);" /* CPU $12 (SZ1) -> `z[0]` */ \
    /* Vertex 1. */ \
    "mfc2  $12, $18;"   /* CPU $12 = SZ2 */ \
    "swc2  $13, 4(%0);" /* COP2 SXY1 (reg 13) -> `xy[1]` */ \
    "sh    $12, 2(%1);" /* CPU $12 (SZ2) -> `z[1]` */ \
    /* Vertex 2. */ \
    "mfc2  $12, $19;"   /* CPU $12 = SZ3 */ \
    "swc2  $14, 8(%0);" /* COP2 SXY2 (reg 14) -> `xy[2]` */ \
    "sh    $12, 4(%1);" /* CPU $12 (SZ3) -> `z[2]` */ \
    : \
    : "r"(xy), "r"(z) \
    : "$12", "memory")

/** @brief Less efficient version of `gte_SetRotMatrix` from PsyQ?
* PsyQ `gte_SetRotMatrix` loads 32-bit words from the `MATRIX` straight into GTE,
* while this macro reads 16-bit words and combines them into a 32-bit word before loading.
* Not sure of reason why, wonder if it's from some older PsyQ SDK.
*/
#define gte_SetRotMatrix_custom(mat) __asm__ volatile( \
    "lhu $12, 16(%0);"                                 \
    "nop;"                                             \
    "ctc2 $12, $4;"                                    \
    "lhu $12, 10(%0);"                                 \
    "lhu $13, 4(%0);"                                  \
    "sll $12, $12, 16;"                                \
    "addu $12, $12, $13;"                              \
    "ctc2 $12, $3;"                                    \
    "lhu $12, 14(%0);"                                 \
    "lhu $13, 8(%0);"                                  \
    "sll $12, $12, 16;"                                \
    "addu $12, $12, $13;"                              \
    "ctc2 $12, $2;"                                    \
    "lhu $12, 2(%0);"                                  \
    "lhu $13, 12(%0);"                                 \
    "sll $12, $12, 16;"                                \
    "addu $12, $12, $13;"                              \
    "ctc2 $12, $1;"                                    \
    "lhu $12, 6(%0);"                                  \
    "lhu $13, 0(%0);"                                  \
    "sll $12, $12, 16;"                                \
    "addu $12, $12, $13;"                              \
    "ctc2 $12, $0;"                                    \
    :                                                  \
    : "r"(mat)                                         \
    : "$12", "$13", "memory")

/** @brief Loads `x`/`y`/`z` into GTE Vector 0 / `VZY0`/`VZ0`. */
#define gte_LoadVector0_XYZ(x, y, z) __asm__ volatile( \
    "sll %0, %0, 16;"                                  \
    "srl %0, %0, 16;"                                  \
    "sll %1, %1, 16;"                                  \
    "or  %0, %0, %1;"                                  \
    "mtc2 %0, $0;"                                     \
    "mtc2 %2, $1;"                                     \
    :                                                  \
    : "r"(x), "r"(y), "r"(z))

/** @brief Sets the GTE's `vxy0` register to 0. */
#define gte_ldvxy0_Zero() __asm__ volatile( \
    "mtc2  $zero, $0;")

#define gte_ldvxy0(val) __asm__ volatile( \
    "mtc2  %0, $0;"                       \
    :                                     \
    : "r"(val)                            \
    : "memory")

/** @brief Sets the GTE's `vz0` register to 0. */
#define gte_ldvz0() __asm__ volatile( \
    "mtc2  $zero, $1;")

/** @brief Retrieves the value from the GTE's `SZ3` register. */
#define gte_stSZ3()                                   \
    ({                                                \
        u32 __r;                                      \
        __asm__ volatile("mfc2 %0, $19" : "=r"(__r)); \
        __r;                                          \
    })

#define gte_stMAC0()                                       \
    ({                                                     \
        s32 __r;                                           \
        __asm__ volatile("mfc2 %0, $24; nop" : "=r"(__r)); \
        __r;                                               \
    })

#define gte_stMAC1()                                        \
    ({                                                      \
        u32 __r;                                            \
        __asm__ volatile("mfc2 %0, $25; nop;" : "=r"(__r)); \
        __r;                                                \
    })

#define gte_stMAC2()                                        \
    ({                                                      \
        u32 __r;                                            \
        __asm__ volatile("mfc2 %0, $26; nop;" : "=r"(__r)); \
        __r;                                                \
    })

#define gte_stMAC12(r0) __asm__ volatile( \
    "mfc2    $12, $25;"                   \
    "nop;"                                \
    "sh    $12, 0( %0 );"                 \
    "mfc2    $13, $26;"                   \
    "nop;"                                \
    "sh    $13, 2( %0 );"                 \
    :                                     \
    : "r"(r0)                             \
    : "$12", "$13", "memory")

#define gte_ldsv3_(x, y, z) __asm__ volatile( \
    "mtc2  %0, $9;"                           \
    "mtc2  %1, $10;"                          \
    "mtc2  %2, $11;"                          \
    :                                         \
    : "r"(x), "r"(y), "r"(z)                  \
    : "memory")

#define gte_ldR13R21(val) __asm__ volatile( \
    "or    $12, %0, $0;"                    \
    "negu  $12, $12;"                       \
    "ctc2  $12, $1;"                        \
    :                                       \
    : "r"(val)                              \
    : "$12", "memory")

#define gte_ldR11R12(r0) __asm__ volatile( \
    "ctc2 %0, $0;"                         \
    "ctc2 %0, $2;"                         \
    :                                      \
    : "r"(r0)                              \
    : "memory")

#define gte_IsDisabled()                                \
    ({                                                  \
        u32 __r;                                        \
        __asm__ volatile("move %0, $zero" : "=r"(__r)); \
        __r;                                            \
    })

#define gte_stlzcr(dst) __asm__ volatile( \
    "mfc2  %0, $31"                       \
    : "=r"(dst))

#define gte_ldir_stbk() __asm__ volatile ( \
    "cfc2    $12, $13;"                    \
    "nop;"                                 \
    "mtc2    $12, $9;"                     \
    "cfc2    $12, $14;"                    \
    "nop;"                                 \
    "mtc2    $12, $10;"                    \
    "cfc2    $12, $15;"                    \
    "nop;"                                 \
    "mtc2    $12, $11;"                    \
    : : : "$12")

#define gte_ldmac_stir() __asm__ volatile ( \
    "mfc2    $12, $25;"                     \
    "nop;"                                  \
    "ctc2    $12, $13;"                     \
    "mfc2    $12, $26;"                     \
    "nop;"                                  \
    "ctc2    $12, $14;"                     \
    "mfc2    $12, $27;"                     \
    "nop;"                                  \
    "ctc2    $12, $15;"                     \
    : : : "$12")

#endif /* !SH_PC_PORT */

#endif
