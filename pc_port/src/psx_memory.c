/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * psx_memory.c - PSX memory layout emulation for PC port
 *
 * The PSX has 2MB of main RAM at 0x80000000-0x801FFFFF.
 * Game code uses hardcoded addresses throughout (buffer pointers, etc).
 *
 * We allocate a full 2MB block. PSX_ADDR() in psx_memory.h maps
 * an address offset (0x00000000-0x001FFFFF) into this buffer.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* Full PSX RAM emulation - 2MB + 1MB overflow guard.
 * Some file reads target buffers near the end of PSX RAM (e.g. FS_BUFFER_16
 * at 0x1EBE00) and the loaded file may exceed the remaining space.
 * On PSX this wraps around; on PC it would corrupt adjacent memory.
 * Extra 1MB prevents stack/heap corruption from these overflows. */
#define PSX_RAM_SIZE (3 * 1024 * 1024)

/* Game structs live inside both buffers at their PSX offsets, and the
 * compiler accesses them in pairs of words (LDRD/STRD, VLDR). A plain byte
 * array is only byte-aligned; x86 and arm64 tolerate that, 32-bit ARM
 * (Fire TV) raises SIGBUS. Same fix as STUB_ALIGN in data_stubs.c. */
#if defined(__GNUC__) || defined(__clang__)
#define PSX_MEM_ALIGN __attribute__((aligned(16)))
#else
#define PSX_MEM_ALIGN
#endif

uint8_t g_PsxRam[PSX_RAM_SIZE] PSX_MEM_ALIGN;

/* PSX Scratchpad RAM emulation.
 * Real PSX has 1KB; the PC port widens s_GteScratchData/s_GteScratchData2 to
 * ~2964 bytes (256-slot pools for oversized models) -- 4KB holds it, and the
 * bodyprog.h _Static_asserts pin sizeof <= 4096. */
uint8_t g_PsxScratchpad[4096] PSX_MEM_ALIGN;

void PsxMemory_Init(void)
{
    memset(g_PsxRam, 0, sizeof(g_PsxRam));
    memset(g_PsxScratchpad, 0, sizeof(g_PsxScratchpad));
    printf("[PSX_MEM] Initialized %d KB RAM emulation at %p\n",
           PSX_RAM_SIZE / 1024, (void*)g_PsxRam);
}
