#ifndef _COMMON_H
#define _COMMON_H

#include "include_asm.h"
#include "types.h"
#include "version.h"

#define PAD_RODATA()

#ifdef SH_PC_PORT
#include <stdint.h>
extern uint8_t g_PsxScratchpad[4096];
#define PSX_SCRATCH ((void*)g_PsxScratchpad)
#define PSX_SCRATCH_ADDR(offset) ((void*)(g_PsxScratchpad + (offset)))
/* Redirect PSX kernel functions that conflict with Windows API names */
extern int PsxEnterCriticalSection(void);
extern void PsxExitCriticalSection(void);
#define EnterCriticalSection() PsxEnterCriticalSection()
#define ExitCriticalSection() PsxExitCriticalSection()
#else
#define PSX_SCRATCH ((void*)0x1F800000)
#define PSX_SCRATCH_ADDR(offset) ((void*)(((u8*)PSX_SCRATCH) + (offset)))
#endif

/** @brief Computes the size of an array.
 *
 * @param arr Array.
 * @return Element count.
 */
#define ARRAY_SIZE(arr) \
    (s32)(sizeof(arr) / sizeof((arr)[0]))

#ifdef SH_PC_PORT
#define ALIGN(x, a) \
    (((uintptr_t)(x) + ((a) - 1)) & ~((uintptr_t)(a) - 1))
#else
#define ALIGN(x, a) \
    (((u32)(x) + ((a) - 1)) & ~((a) - 1))
#endif

#ifdef SH_PC_PORT
#define SECTION(x)
#else
#define SECTION(x) \
    __attribute__((section(x)))
#endif

#ifdef SH_PC_PORT
/* Struct sizes differ on 64-bit due to pointer size changes */
#define STATIC_ASSERT(cond, msg)
#define STATIC_ASSERT_SIZEOF(type, size)
#else
#define STATIC_ASSERT(cond, msg) \
    typedef char static_assertion_##msg[(cond) ? 1 : -1]

#define STATIC_ASSERT_SIZEOF(type, size) \
    typedef char static_assertion_sizeof_##type[(sizeof(type) == (size)) ? 1 : -1]
#endif

#endif
