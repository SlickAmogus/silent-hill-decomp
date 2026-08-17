/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * dat_endian.h - byte-swap the attract-demo .DAT files after they are read.
 *
 * MISC/DEMO****.DAT is overlaid directly on s_DemoWork and MISC/PLAY****.DAT on
 * an array of s_DemoFrameData, with no reformat walker in between, so on a
 * big-endian host every multi-byte field is read reversed. Same shape as the
 * TIM and ANM bugs and the same remedy. No-ops on little-endian.
 */
#ifndef DAT_ENDIAN_H
#define DAT_ENDIAN_H

/** Convert one s_DemoWork (2048 bytes) in place. Safe to call twice: it detects
 *  already-native data from frameCount's magnitude and returns. */
void Demo_SwapWorkForBigEndian(void* demoWork);

/** Convert `frameCount` entries of s_DemoFrameData (16 bytes each) in place. */
void Demo_SwapPlayForBigEndian(void* playBuf, unsigned frameCount);

#endif
