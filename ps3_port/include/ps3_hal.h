/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ps3_hal.h - the PS3 HAL surface, declared WITHOUT PSL1GHT types.
 *
 * THE RULE THIS HEADER EXISTS TO ENFORCE: PSL1GHT headers and the decomp's
 * include/decomp/types.h may never meet in one translation unit.
 *
 * PSL1GHT's <ppu-types.h> -- pulled in transitively by essentially every lv2/
 * sys/rsx header via ppu-lv2.h -- typedefs u8..u64 and s8..s64. types.h defines
 * those same names. They are NOT compatible on the PPU: LP64 makes uint64_t
 * `unsigned long`, while the decomp's u64 is `unsigned long long`. Same width,
 * different type, so the TU dies with "conflicting types for 'u64'" (plus a
 * `bool` clash). Widths matching is what makes this nasty -- it is a pure
 * type-identity clash, so it cannot be papered over with a cast.
 *
 * The boundary was measured, not assumed, because guessing it too wide costs
 * real capability. What actually conflicts is types.h and therefore anything
 * pulling it: common.h, game.h, and the decomp game headers generally.
 * What does NOT conflict, and is free to sit beside PSL1GHT in a HAL file:
 *
 *     sh_log.h        (only needs <stdio.h>)   -> HAL files CAN use SH_DBG
 *     psx_memory.h
 *     gpu_nv2a.h      (plain float/unsigned)
 *
 * So the seam is: HAL sources may include PSL1GHT and may log, but must not
 * reach for the decomp's game types. Anything needing both is split, with the
 * PSL1GHT half behind a declaration in this header, which uses plain C types
 * only. Ps3_TimebaseFreq() is the pattern.
 */
#ifndef PS3_HAL_H
#define PS3_HAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Ticks of the Cell PPU time base per second, queried from lv2 rather than
 * hardcoded: the nominal 79.8 MHz is a divided core clock and lv2 owns the
 * divisor. Pairs with SH_CYCLES() in sh_hwperf.h, which reads the base with a
 * bare mftb and so needs no PSL1GHT header of its own. */
unsigned long long Ps3_TimebaseFreq(void);

/* Yields to lv2 for approximately `ms`. Unlike the 360, which busy-waits on the
 * time base because a bare-metal libXenon app has no scheduler, the PS3 runs
 * under GameOS and can actually sleep -- spinning here would starve the audio
 * and pad service threads lv2 runs on our behalf. */
void Ps3_SleepMs(unsigned ms);

/* ---- RSX display layer (rsx_video.c) -------------------------------------
 * Everything that touches libgcm lives behind these, because gpu_rsx.c needs
 * the decomp's ShVertex and therefore cannot include PSL1GHT at all. */

/* Brings up the RSX, picks up the console's CURRENT video mode rather than
 * forcing one, and allocates the double-buffered colour + Z24S8 depth
 * surfaces. Returns non-zero on success; everything else no-ops if it failed,
 * so a display that cannot be configured degrades to "runs, draws nothing"
 * instead of hanging. */
int  Ps3Rsx_Init(void);
int  Ps3Rsx_Ready(void);
int  Ps3Rsx_Width(void);
int  Ps3Rsx_Height(void);

/* Bind the back buffer and clear it to `clearArgb` (0xAARRGGBB). */
void Ps3Rsx_FrameBegin(unsigned int clearArgb);
/* Queue the flip and swap buffers. */
void Ps3Rsx_FrameEnd(void);
/* Block until the queued flip has retired. */
void Ps3Rsx_WaitFlip(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3_HAL_H */
