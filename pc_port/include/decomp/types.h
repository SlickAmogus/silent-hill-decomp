/*
 * types.h - PC port override for decomp/types.h
 *
 * Key difference: PSX uses 32-bit `long`, but on 64-bit PC `long` may be
 * 64-bit (on Linux/macOS). We ensure consistent 32-bit sizes using stdint.
 */
#ifndef _TYPES_H
#define _TYPES_H

#include "psyq/sys/types.h"

#include <stdint.h>

#ifndef NULL
#define NULL 0
#endif

typedef int8_t         byte;
typedef int8_t         s8;
typedef int16_t        s16;
typedef int32_t        s32;
typedef int64_t        s64;
typedef uint8_t        u8;
typedef uint16_t       u16;
typedef uint32_t       u32;
typedef uint64_t       u64;

/* Fixed-point types */
typedef int8_t         q0_7;
typedef int16_t        q11_4;
typedef int16_t        q7_8;
typedef int16_t        q3_12;
typedef int32_t        q27_4;
typedef int32_t        q25_6;
typedef int32_t        q23_8;
typedef int32_t        q21_10;
typedef int32_t        q19_12;
typedef int64_t        q51_12;
typedef uint8_t        q0_8;
typedef uint16_t       q8_8;
typedef uint16_t       q4_12;
typedef uint32_t       q24_8;
typedef uint32_t       q20_12;
typedef uint64_t       q52_12;

#ifndef __cplusplus
#ifndef bool
typedef enum { false, true } bool;
#endif
#endif

#define NO_VALUE -1

/*
 * Vector types - use int32_t instead of long to ensure 32-bit on all platforms.
 * PSX `long` is always 32-bit, but on 64-bit Linux `long` is 64-bit.
 */
typedef struct
{
    int32_t vx;
    int32_t vy;
    int32_t vz;
} VECTOR3;

typedef struct
{
    int16_t vx;
    int16_t vy;
    int16_t vz;
} SVECTOR3;

typedef struct
{
    int16_t vx;
    int16_t vz;
} DVECTOR_XZ;

#endif
