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

#endif
