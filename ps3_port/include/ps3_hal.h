/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * ps3_hal.h - the PS3 HAL surface, declared WITHOUT PSL1GHT types.
 *
 * THE RULE THIS HEADER EXISTS TO ENFORCE: PSL1GHT headers and decomp headers
 * may never meet in one translation unit.
 *
 * PSL1GHT's <ppu-types.h> -- pulled in transitively by essentially every lv2/
 * sys/rsx header via ppu-lv2.h -- typedefs u8..u64 and s8..s64. The decomp's
 * include/decomp/types.h defines those same names. They are NOT compatible on
 * the PPU: LP64 makes uint64_t `unsigned long`, while the decomp's u64 is
 * `unsigned long long`. Same width, different type, so the TU dies with
 * "conflicting types for 'u64'". Widths matching is what makes this nasty --
 * it is a pure type-identity clash, so it cannot be papered over with a cast.
 *
 * The seam: shared game code and pc_port stubs include THIS header, which uses
 * plain C types only. Only sources under ps3_port/src that include NO decomp
 * headers may include PSL1GHT. Every HAL entry point therefore crosses the
 * boundary as plain C.
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

#ifdef __cplusplus
}
#endif

#endif /* PS3_HAL_H */
