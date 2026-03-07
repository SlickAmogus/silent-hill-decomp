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

#endif
