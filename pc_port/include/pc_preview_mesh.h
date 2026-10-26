/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_PREVIEW_MESH_H
#define PC_PREVIEW_MESH_H
#include <stddef.h>

/* Owned, untextured diagnostic geometry. This is NOT a complete scene loader:
 * only a self-contained static v6 LM model is accepted (no skeleton/shared pool). */
typedef struct {
    float* positions;
    /* u, v, material index, raw PSX CLUT word for each emitted vertex. */
    float* textureCoordinates;
    size_t vertexCount;
    float minimum[3], maximum[3];
} PcPreviewMesh;
int Pc_PreviewMesh_Decode(const unsigned char* lm, size_t size, unsigned model,
                          PcPreviewMesh* out);
void Pc_PreviewMesh_Free(PcPreviewMesh* mesh);
#endif
