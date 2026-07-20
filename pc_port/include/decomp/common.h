/*
 * common.h - PC port override for decomp/common.h
 *
 * Replaces PSX-specific constructs with PC equivalents:
 * - PSX scratchpad memory → heap-allocated buffer
 * - MIPS section attributes → no-ops
 * - INCLUDE_ASM → disabled (SKIP_ASM defined)
 */
#ifndef _COMMON_H
#define _COMMON_H

#include "include_asm.h"
#include "types.h"
#include "version.h"

#define PAD_RODATA()

/*
 * PSX Scratchpad Memory Emulation
 *
 * The PSX has 1KB of fast scratchpad RAM at 0x1F800000.
 * On PC, we allocate a static buffer and use its address.
 */
#ifdef SH_PC_PORT

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t g_PsxScratchpad[4096];
#ifdef __cplusplus
}
#endif

#define PSX_SCRATCH ((void*)g_PsxScratchpad)
#define PSX_SCRATCH_ADDR(offset) ((void*)(g_PsxScratchpad + (offset)))

#else
/* Original PSX definitions */
#define PSX_SCRATCH ((void*)0x1F800000)
#define PSX_SCRATCH_ADDR(offset) ((void*)(((u8*)PSX_SCRATCH) + (offset)))
#endif

#define ARRAY_SIZE(arr) \
    (s32)(sizeof(arr) / sizeof((arr)[0]))

#define ALIGN(x, a) \
    (((u32)(x) + ((a) - 1)) & ~((a) - 1))

/* Section attribute - no-op on PC */
#ifdef SH_PC_PORT
#define SECTION(x)
#else
#define SECTION(x) __attribute__((section(x)))
#endif

#define STATIC_ASSERT(cond, msg) \
    typedef char static_assertion_##msg[(cond) ? 1 : -1]

#define STATIC_ASSERT_SIZEOF(type, size) \
    typedef char static_assertion_sizeof_##type[(sizeof(type) == (size)) ? 1 : -1]

#endif
