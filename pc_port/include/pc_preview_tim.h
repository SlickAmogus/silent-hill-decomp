/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_PREVIEW_TIM_H
#define PC_PREVIEW_TIM_H
#include <stddef.h>
/* Decode a single palette of a raw 4/8-bit TIM into owned RGBA pixels, without
 * touching the game's VRAM or TIM parser. CLUT uses original file coordinates. */
unsigned char* Pc_PreviewTim_Decode(const unsigned char* data, size_t size,
                                  unsigned clut, int* width, int* height);
#endif
