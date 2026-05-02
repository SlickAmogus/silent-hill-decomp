/*
 * inline_no_dmpsx.h - PC port override
 *
 * On PSX, this replaces GTE macros with raw coprocessor opcodes (.word).
 * On PC, PsyCross inline_c.h already provides C implementations of all
 * GTE operations, so this file is a no-op.
 */
#ifndef _INLINE_NO_DMPSX_H_
#define _INLINE_NO_DMPSX_H_

#include <inline_c.h>

/* PsyCross GTE macros dereference args as pointers (*(uint*)(r0)),
 * but real PSX inline_c passes them as values (mtc2 r0, $reg).
 * Override all affected macros to match PSX value-passing behavior. */

#undef gte_lddp
#define gte_lddp( r0 ) { uint _v = (uint)(r0); MTC2(_v, 8); }

#undef gte_ldsxy0
#define gte_ldsxy0( r0 ) { MTC2((uint)(r0), 12); }

#undef gte_ldsxy3
#define gte_ldsxy3( r0, r1, r2 ) \
    { MTC2((uint)(r0), 12); MTC2((uint)(r2), 14); MTC2((uint)(r1), 13); }

/* gte_ldv3c - Load 3 vertices (6 regs: VXY0,VZ0, VXY1,VZ1, VXY2,VZ2)
 * from a contiguous array of 3 SVECTORs (24 bytes).
 * PSX: lwc2 $0-$5 from r0. PsyCross doesn't define this. */
#undef gte_ldv3c
#define gte_ldv3c( r0 ) do { \
    uint *_p = (uint*)((char*)(r0)); \
    MTC2(_p[0], 0); MTC2(_p[1], 1); \
    MTC2(_p[2], 2); MTC2(_p[3], 3); \
    MTC2(_p[4], 4); MTC2(_p[5], 5); \
} while(0)

/* gte_stsxy3c - Store 3 screen XY results to contiguous DVECTORs.
 * PSX: swc2 $12,$13,$14 to r0+0,+4,+8. PsyCross doesn't define this. */
#undef gte_stsxy3c
#define gte_stsxy3c( r0 ) do { \
    uint *_p = (uint*)((char*)(r0)); \
    _p[0] = MFC2(12); _p[1] = MFC2(13); _p[2] = MFC2(14); \
} while(0)

/* gte_stsxy3_g3 - Store SXY0/SXY1/SXY2 (GTE C12-14) into the X/Y slots
 * of POLY_G3 / POLY_FT4 / POLY_GT4 packets.
 *
 * PSX offsets are 8, 16, 24 because DECLARE_P_ADDR is 4 bytes there.
 * On PC with USE_EXTENDED_PRIM_POINTERS, DECLARE_P_ADDR is 12 bytes,
 * so the same logical vertex slots live at 16, 24, 32.
 *
 * Earlier this macro used the PSX offsets verbatim. The result:
 *   * offset 8 (PC) overlaps the prim's `len`/`pgxp_index` header,
 *     so each muzzle-flash POLY_FT4 had its header trashed by a
 *     32-bit screen-coord write — corrupting both setlen() and the
 *     PGXP index.
 *   * x0,y0 received vertex-1 data, x1,y1 received vertex-2 data,
 *     and x2,y2 was never written (left uninitialized).
 *
 * Symptom was a crash inside ParsePrimitivesLinkedList walking past
 * a corrupted next-pointer (high garbage like 0x2B00..., the tpage
 * byte from the next muzzle-flash field write bleeding into bytes
 * that should have been part of an addr field).
 *
 * On PSX (32-bit, non-extended) compiles, fall back to the original
 * offsets so this header stays valid for both targets. */
#undef gte_stsxy3_g3
#if defined(_M_X64) || defined(__amd64__) || defined(SH_PC_PORT)
#define gte_stsxy3_g3( p ) do { \
    char *_b = (char*)(p); \
    *(uint*)(_b + 16) = MFC2(12); \
    *(uint*)(_b + 24) = MFC2(13); \
    *(uint*)(_b + 32) = MFC2(14); \
} while(0)
#else
#define gte_stsxy3_g3( p ) do { \
    char *_b = (char*)(p); \
    *(uint*)(_b + 8)  = MFC2(12); \
    *(uint*)(_b + 16) = MFC2(13); \
    *(uint*)(_b + 24) = MFC2(14); \
} while(0)
#endif

/* gte_stsz3c - Store SZ1/SZ2/SZ3 (GTE C17-19) into 3 consecutive
 * shorts at p. The particle code uses this after gte_rtpt() to grab
 * the 3 transformed Z depths for OT depth-sorting. PsyCross doesn't
 * define it. */
#undef gte_stsz3c
#define gte_stsz3c( p ) do { \
    short *_s = (short*)(p); \
    _s[0] = (short)MFC2(17); \
    _s[1] = (short)MFC2(18); \
    _s[2] = (short)MFC2(19); \
} while(0)

#endif
