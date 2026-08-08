#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "PsyX/PsyX_render.h"
#include "pc_modern_depth.h"

float PsyX_GetItemDepthSzMax(void)
{
    return 100.0f;
}

static int Near(float actual, float expected)
{
    return fabsf(actual - expected) < 1.0e-6f;
}

int main(void)
{
    GrModernVertex vertices[6] = { 0 };
    const unsigned short sz[6] = { 10, 25, 40, 60, 75, 90 };
    const float expected[6] = { 0.8f, 0.5f, 0.2f, -0.2f, -0.5f, -0.8f };
    size_t i;

    assert(Pc_ModernDepth_Apply(vertices, sz, 6));
    for (i = 0; i < 6; i++)
        assert(Near(vertices[i].z, expected[i]));

    /* Two triangles sharing one modern packet retain six independent depths. */
    assert(!Near(vertices[0].z, vertices[1].z));
    assert(!Near(vertices[1].z, vertices[2].z));
    assert(!Near(vertices[3].z, vertices[4].z));
    assert(!Near(vertices[4].z, vertices[5].z));
    assert(!Near(vertices[0].z, vertices[3].z));

    /* A triangle average, OT bucket, or one-depth-per-mesh collapse cannot pass. */
    assert(!Near(vertices[0].z, 0.5f));
    assert(!Near(vertices[2].z, 0.5f));
    assert(!Near(vertices[3].z, -0.5f));
    assert(!Near(vertices[5].z, -0.5f));

    assert(!Pc_ModernDepth_Apply(NULL, sz, 6));
    assert(!Pc_ModernDepth_Apply(vertices, NULL, 6));
    assert(!Pc_ModernDepth_Apply(vertices, sz, 0));
    puts("modern_depth_test=PASS distinct=6 triangles=2 collapse_rejected=PASS");
    return 0;
}
