/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_PPLIT_H
#define PC_PPLIT_H

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/formats/model.h"

/* Optional per-pixel lighting for the stock world and characters (config key
 * per_pixel_lighting). Each lit mesh records its normals in view space, each
 * lit prim hands its corners' normals to PsyCross, and the shader re-runs the
 * GTE light sum per pixel. Everything here is a no-op while it is off. */

extern int g_PsyX_PpLitActive;
extern float g_PsyX_HeroLight[4];

void Pc_PpLit_Apply(void);
void Pc_PpLit_Light(const MATRIX* mat);
void Pc_PpLit_WorldMesh(const s_MeshHeader* meshHdr, s32 normalOffset, const MATRIX* mat);
void Pc_PpLit_CharaMesh(const s_MeshHeader* meshHdr, s32 normalOffset, const MATRIX* mat);
void Pc_PpLit_WorldPrim(const void* prim, s32 s0, s32 s1, s32 s2, s32 s3);
void Pc_PpLit_CharaPrim(const void* prim, s32 n0, s32 n1, s32 n2, s32 n3);

#endif
