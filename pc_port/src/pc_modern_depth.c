/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "PsyX/PsyX_render.h"
#include "psx/libgpu.h"

#include "pc_modern_depth.h"

int Pc_ModernDepth_Apply(struct GrModernVertex* vertices,
                         const unsigned short* sz,
                         size_t count)
{
    const float szMax = PsyX_GetItemDepthSzMax();
    const float scale = 2.0f / szMax;
    size_t i;

    if (vertices == NULL || sz == NULL || count == 0 || szMax < 1.0f)
        return 0;
    for (i = 0; i < count; i++)
    {
        float z = 1.0f - (float)sz[i] * scale;
        if (z < -1.0f)
            z = -1.0f;
        if (z > 1.0f)
            z = 1.0f;
        vertices[i].z = z;
    }
    return 1;
}
