/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * tim_endian_xbox360.c - byte-swap a TIM in place after it is read off the disc.
 *
 * TIM files are PSX data: little-endian throughout. ReadTIM does not copy them,
 * it points prect/paddr straight INTO the file buffer, so on a big-endian host
 * every field is read byte-swapped. The first 360 boot showed it plainly:
 *   mode  = 134217728 (0x08000000) where 8 was meant
 *   prect = 4096x256  (0x1000)     where 16 was meant
 * and a "16 wide" texture claiming 4096 is why the VRAM uploads were nonsense.
 *
 * Swapping in place at load is the right boundary: everything downstream --
 * ReadTIM, LoadImage, the VRAM cache -- then sees native values with no further
 * changes, and the pixel words are swapped once instead of on every sample.
 *
 * Idempotent by construction: the magic word reads 0x10 once converted and
 * 0x10000000 while still foreign, so a second call is a no-op. That matters
 * because a TIM buffer can be re-parsed without being re-read.
 */
#include <stdint.h>

#include "sh_log.h"

static uint32_t Swap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}

static uint16_t Swap16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

static void SwapWords16(uint16_t* p, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        p[i] = Swap16(p[i]);
}

/* TIM layout:
 *   u32 magic (0x10), u32 mode
 *   if (mode & 8): u32 clutSize, u16 x,y,w,h, u16 clut[w*h]
 *   u32 imgSize, u16 x,y,w,h, u16 pixels[w*h]
 * Both size words count themselves, so they bound each block exactly. */
void Tim_SwapForBigEndian(void* addr)
{
    uint32_t* hdr = (uint32_t*)addr;
    uint32_t  mode;

    if (!hdr)
        return;
    if (hdr[0] == 0x10)
        return;                       /* already native -- nothing to do */
    if (Swap32(hdr[0]) != 0x10) {
        SH_DBG("[TIMSWAP] not a TIM: magic=0x%08x", (unsigned)hdr[0]);
        return;
    }

    hdr[0] = Swap32(hdr[0]);
    hdr[1] = Swap32(hdr[1]);
    mode   = hdr[1];

    {
        uint32_t* blk = hdr + 2;
        int       pass;

        /* CLUT block first when present, then the pixel block. Identical shape,
         * so one loop covers both rather than duplicating the arithmetic. */
        for (pass = 0; pass < 2; pass++) {
            uint16_t* rect;
            uint32_t  words;

            if (pass == 0 && !(mode & 0x8))
                continue;             /* no CLUT in this TIM */

            blk[0] = Swap32(blk[0]);  /* block size, includes this word */
            rect   = (uint16_t*)&blk[1];
            SwapWords16(rect, 4);     /* x, y, w, h */

            /* Trust the size word over w*h: a truncated or padded rip would
             * otherwise walk off the end of the buffer. */
            words = (blk[0] > 12) ? (blk[0] - 12) / 2 : 0;
            SwapWords16((uint16_t*)&blk[3], words);

            blk = (uint32_t*)((uint8_t*)blk + blk[0]);
        }
    }
}
