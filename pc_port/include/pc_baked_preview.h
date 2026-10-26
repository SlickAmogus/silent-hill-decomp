/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_BAKED_PREVIEW_H
#define PC_BAKED_PREVIEW_H

typedef struct {
    unsigned texture;
    float value;
    int width,height,animated,approximate;
} PcBakedPreview;

/* Fixed-parameter examples, not the current world or a simulated renderer.
 * Missing entries return zero; never substitute an unrelated image. */
int Pc_BakedPreview_Get(const char* key,float value,PcBakedPreview* result);
void Pc_BakedPreview_Close(void);
#endif
