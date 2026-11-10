/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_ILM_PREVIEW_H
#define PC_ILM_PREVIEW_H

#ifdef __cplusplus
extern "C" {
#endif

#define IP_MAX_MAT 16

/* A stock character posed on its ANM's rest frame, as one triangle stream in
 * game space (8 floats per corner: x y z, normal, u v), grouped by palette
 * row, with each row's sheet decoded to RGBA (texW x texH). */
typedef struct
{
    float*         stream;
    int            corners;
    int            matCount;
    struct { int first, count; } mats[IP_MAX_MAT];
    unsigned char* rgba[IP_MAX_MAT];
    int            texW, texH;
    float          bounds[6];
} IlmPreview;

int  Pc_IlmPreview_Build(int charaId, IlmPreview* out);
void Pc_IlmPreview_Free(IlmPreview* p);
/* A whole game file by its file-table index (a loose copy first, else the
 * disc), malloc'd; NULL when it cannot be read. */
unsigned char* Pc_IlmPreview_ReadFile(int idx, int* outSize);

#ifdef __cplusplus
}
#endif

#endif
