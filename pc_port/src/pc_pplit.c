/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_pplit.h"

#include <math.h>
#include <string.h>

#include "bodyprog/gfx/world.h"
#include "PsyX/PsyX_public.h"
#include "pc_config.h"

extern s_WorldEnvWork g_WorldEnvWork;

/* Normals are indexed exactly like the colours they stand in for: per lighting
 * slot for the world (field_2B8, one normal per run of `count` slots) and per
 * normal for characters (field_21C). */
static float s_worldN[GTE_SCRATCH_SHADE_CAP][3];
static float s_charaN[GTE_SCRATCH_NORMAL_CAP][3];

void Pc_PpLit_Apply(void)
{
    static const float strength[4] = { 0.0f, 0.35f, 0.65f, 1.0f };
    /* Up and to the left of the camera, so the fill reads as a key light
     * rather than a flat front flash. View space: +y is down, -z faces the
     * camera. */
    static const float dir[3] = { -0.35f, -0.6f, -0.72f };
    float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    int level = g_PcConfig.heroLight;

    if (level < 0) level = 0;
    if (level > 3) level = 3;
    g_PsyX_PpLitActive = g_PcConfig.perPixelLighting ? 1 : 0;
    g_PsyX_HeroLight[0] = dir[0] / len;
    g_PsyX_HeroLight[1] = dir[1] / len;
    g_PsyX_HeroLight[2] = dir[2] / len;
    g_PsyX_HeroLight[3] = strength[level];
}

static void Rotate(const MATRIX* m, s32 x, s32 y, s32 z, float* out)
{
    out[0] = (m->m[0][0] * (float)x + m->m[0][1] * (float)y + m->m[0][2] * (float)z) / 4096.0f;
    out[1] = (m->m[1][0] * (float)x + m->m[1][1] * (float)y + m->m[1][2] * (float)z) / 4096.0f;
    out[2] = (m->m[2][0] * (float)x + m->m[2][1] * (float)y + m->m[2][2] * (float)z) / 4096.0f;
}

/* field_74/field_7C are the light row and position in the space of the model
 * func_80057228 last prepared, so this model's view matrix carries them into
 * view space, where they are the same for every model in the frame. */
void Pc_PpLit_Light(const MATRIX* mat)
{
    PsyX_PpLight l;
    int i;

    if (!g_PsyX_PpLitActive)
        return;
    memset(&l, 0, sizeof(l));
    l.mode = g_WorldEnvWork.field_0;
    if (l.mode != 0)
    {
        Rotate(mat, g_WorldEnvWork.field_7C.vx, g_WorldEnvWork.field_7C.vy, g_WorldEnvWork.field_7C.vz, l.lightPos);
        for (i = 0; i < 3; i++)
            l.lightPos[i] += (float)mat->t[i];
        Rotate(mat, g_WorldEnvWork.field_74.vx, g_WorldEnvWork.field_74.vy, g_WorldEnvWork.field_74.vz, l.lightDir);
        l.tint[0] = g_WorldEnvWork.worldTintColor.r;
        l.tint[1] = g_WorldEnvWork.worldTintColor.g;
        l.tint[2] = g_WorldEnvWork.worldTintColor.b;
        for (i = 0; i < 3; i++)
            l.lc[i] = (float)g_WorldEnvWork.field_2C.m[i][0];
        l.bk[0] = g_WorldEnvWork.field_24;
        l.bk[1] = g_WorldEnvWork.field_25;
        l.bk[2] = g_WorldEnvWork.field_26;
        l.ambient = (float)g_WorldEnvWork.field_20;
        l.cap = g_WorldEnvWork.field_3;
        l.dqa = (float)g_WorldEnvWork.field_4C;
    }
    PsyX_PpLitSetLight(&l);
}

void Pc_PpLit_WorldMesh(const s_MeshHeader* meshHdr, s32 normalOffset, const MATRIX* mat)
{
    const s_Normal* n;
    s32 slot = normalOffset;

    if (!g_PsyX_PpLitActive)
        return;
    for (n = meshHdr->normals; n < &meshHdr->normals[meshHdr->normalCount]; n++)
    {
        float v[3];
        s32 k;

        Rotate(mat, n->nx << 5, n->ny << 5, n->nz << 5, v);
        for (k = 0; k < n->count && slot < GTE_SCRATCH_SHADE_CAP; k++, slot++)
            memcpy(s_worldN[slot], v, sizeof(v));
    }
}

void Pc_PpLit_CharaMesh(const s_MeshHeader* meshHdr, s32 normalOffset, const MATRIX* mat)
{
    s32 i;

    if (!g_PsyX_PpLitActive)
        return;
    for (i = 0; i < meshHdr->normalCount && normalOffset + i < GTE_SCRATCH_NORMAL_CAP; i++)
    {
        const s_Normal* n = &meshHdr->normals[i];
        Rotate(mat, n->nx << 5, n->ny << 5, n->nz << 5, s_charaN[normalOffset + i]);
    }
}

static void Record(const void* prim, int kind, float (*table)[3], s32 cap, const s32* idx)
{
    float n[12];
    int c;

    for (c = 0; c < 4; c++)
    {
        if (idx[c] < 0 || idx[c] >= cap)
            return;
        memcpy(&n[c * 3], table[idx[c]], sizeof(float) * 3);
    }
    PsyX_PpLitPrim(prim, kind, n);
}

void Pc_PpLit_WorldPrim(const void* prim, s32 s0, s32 s1, s32 s2, s32 s3)
{
    const s32 idx[4] = { s0, s1, s2, s3 };
    Record(prim, 1, s_worldN, GTE_SCRATCH_SHADE_CAP, idx);
}

void Pc_PpLit_CharaPrim(const void* prim, s32 n0, s32 n1, s32 n2, s32 n3)
{
    const s32 idx[4] = { n0, n1, n2, n3 };
    Record(prim, 2, s_charaN, GTE_SCRATCH_NORMAL_CAP, idx);
}
