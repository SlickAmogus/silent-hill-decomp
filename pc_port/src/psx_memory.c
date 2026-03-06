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

/* Full PSX RAM emulation - 2MB */
#define PSX_RAM_SIZE (2 * 1024 * 1024)
uint8_t g_PsxRam[PSX_RAM_SIZE];

/* PSX Scratchpad RAM emulation (1KB) */
uint8_t g_PsxScratchpad[1024];

void PsxMemory_Init(void)
{
    memset(g_PsxRam, 0, sizeof(g_PsxRam));
    memset(g_PsxScratchpad, 0, sizeof(g_PsxScratchpad));
    printf("[PSX_MEM] Initialized %d KB RAM emulation at %p\n",
           PSX_RAM_SIZE / 1024, (void*)g_PsxRam);
}
