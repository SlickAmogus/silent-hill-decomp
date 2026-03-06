/*
 * psx_memory.h - PSX memory emulation for PC port
 *
 * Maps PSX hardcoded addresses (0x80XXXXXX) to offsets within
 * a 2MB RAM buffer allocated on the heap.
 */
#ifndef PSX_MEMORY_H
#define PSX_MEMORY_H

#include <stdint.h>

/* 2MB PSX RAM buffer */
extern uint8_t g_PsxRam[];

/* 1KB Scratchpad */
extern uint8_t g_PsxScratchpad[];

/* Convert a PSX address (0x80XXXXXX) to a PC pointer into g_PsxRam */
#define PSX_ADDR(addr) ((void*)(g_PsxRam + ((addr) & 0x1FFFFF)))

void PsxMemory_Init(void);

#endif /* PSX_MEMORY_H */
