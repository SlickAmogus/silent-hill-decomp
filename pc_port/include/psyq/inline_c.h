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
 * POLY_G3 layout. POLY_G3 has XYs at offsets 8, 16, 24 — POLY_FT4 has
 * its first three vertex XYs at the same offsets, so this macro is
 * also valid for the first 3 vertices of POLY_FT4 (the muzzle-flash
 * particle code in bodyprog_8005E0DC.c uses it that way). */
#undef gte_stsxy3_g3
#define gte_stsxy3_g3( p ) do { \
    char *_b = (char*)(p); \
    *(uint*)(_b + 8)  = MFC2(12); \
    *(uint*)(_b + 16) = MFC2(13); \
    *(uint*)(_b + 24) = MFC2(14); \
} while(0)

/* gte_stsz3c: store SZ1/SZ2/SZ3 (GTE C17-19) into 3 consecutive shorts
 * at p. Used by particle math after gte_rtpt() to grab the depths of
 * three transformed vertices. */
#undef gte_stsz3c
#define gte_stsz3c( p ) do { \
    short *_s = (short*)(p); \
    _s[0] = (short)MFC2(17); \
    _s[1] = (short)MFC2(18); \
    _s[2] = (short)MFC2(19); \
} while(0)

#endif
