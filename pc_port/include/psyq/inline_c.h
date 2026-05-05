/* PSY-Q to PsyCross compatibility shim */
#ifndef _PSYQ_COMPAT_INLINE_C_H
#define _PSYQ_COMPAT_INLINE_C_H
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

#undef gte_ldv3c
#define gte_ldv3c( r0 ) do { \
    uint *_p = (uint*)((char*)(r0)); \
    MTC2(_p[0], 0); MTC2(_p[1], 1); \
    MTC2(_p[2], 2); MTC2(_p[3], 3); \
    MTC2(_p[4], 4); MTC2(_p[5], 5); \
} while(0)

#undef gte_stsxy3c
#define gte_stsxy3c( r0 ) do { \
    uint *_p = (uint*)((char*)(r0)); \
    _p[0] = MFC2(12); _p[1] = MFC2(13); _p[2] = MFC2(14); \
} while(0)

/* gte_stsxy3_g3: store SXY0/1/2 (GTE C12-14) into the X/Y slots of a
 * POLY_G3 / POLY_FT4 / POLY_GT4 packet. PSX layout has XYs at 8,16,24
 * (DECLARE_P_ADDR=4 B); PC with USE_EXTENDED_PRIM_POINTERS has them
 * at 16,24,32 (DECLARE_P_ADDR=12 B). Using PSX offsets on PC clobbers
 * the prim header (len/pgxp_index) AND shifts vertex data by one
 * slot, leaving x2,y2 uninitialized — see crash dump from
 * 2026-05-01 21:37 (ParsePrimitivesLinkedList +0xa5, bad next-pointer
 * with the muzzle-flash tpage byte 0x2B in upper bytes). */
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

/* gte_stsz3c: store SZ1/SZ2/SZ3 (GTE C17-19) with PSX `swc2` stride (4 bytes).
 * Mirrors the canonical fix in pc_port/include/inline_no_dmpsx.h. Kept here
 * defensively for any TU that includes this shim without inline_no_dmpsx.h.
 *
 * PSX uses `swc2 $N,K($p)` — a 32-bit store at offsets 0, 4, 8. Earlier the
 * macro here used `short*` stride 2 (6 bytes total), leaving the upper half
 * of each destination s32 stale → particle code that averages four
 * consecutive s32 fields after gte_stsz3c (e.g. func_80063A50 muzzle-flash
 * field_1BC..field_1C8) produces garbage Z, lands in OT bucket 0, and the
 * quads render as a one-frame full-screen flash. */
#undef gte_stsz3c
#define gte_stsz3c( p ) do { \
    int *_w = (int*)(p); \
    _w[0] = (int)(MFC2(17) & 0xFFFF); \
    _w[1] = (int)(MFC2(18) & 0xFFFF); \
    _w[2] = (int)(MFC2(19) & 0xFFFF); \
} while(0)

#endif
