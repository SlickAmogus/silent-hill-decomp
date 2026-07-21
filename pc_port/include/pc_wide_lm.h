#ifndef PC_WIDE_LM_H
#define PC_WIDE_LM_H

/* v7 high-poly ILM support — detection + parse + registry + lifetime.
 *
 * A v7 file is a NARROW v6-shaped spine (a real s_LmHeader: materials,
 * modelHdrs with names + 0xB draw-chain bits, modelOrder — but meshCount=0 on
 * every part, so the stock walkers draw nothing) plus a PC-only u32 GEOMETRY
 * blob keyed by modelFileIdx that a separate wide drawer consumes. This TU
 * detects v7 in LmHeader_FixOffsets_PC, reformats the spine exactly as the v6
 * path does, and registers the wide blob. On ANY failure the spine alone is
 * left standing (meshCount=0 -> invisible, non-corrupting) — always fallback.
 *
 * See docs and the Stage v7 loader spec. Wide-blob record layout mirrors the
 * converter's _emit_wide_ilm in pc_port/tools/ilm_obj.py. */

#include <stddef.h>

#include "game.h"
#include "bodyprog/formats/lm.h"
#include "bodyprog/formats/model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One primitive of a wide mesh. Vertex and normal corners are SEPARATE u32
 * arrays indexing this mesh's own verts[]/normals[] (0xFFFFFFFF in corner[3] is
 * the triangle sentinel). Material is already baked into clut/flags by the
 * converter — clut == s_Primitive.field_2, flags == s_Primitive.field_6. */
typedef struct
{
    u32 corner[4];
    u32 normal[4];
    u16 uv[4]; /* packed u | (v << 8) */
    u16 clut;
    u16 flags;
} s_WidePrim;

typedef struct
{
    u32        vertexCount;
    u32        normalCount;
    u32        primCount;
    SVECTOR*   verts;   /* bone-local, same space as stock geometry */
    SVECTOR*   normals;
    s_WidePrim* prims;
} s_WideMesh;

typedef struct
{
    u8          boneIdx;
    u32         meshCount;
    s_WideMesh* meshes;
} s_WideLmPart;

/* Detect a v7 spine at lmHdr, reformat it in place (calloc'd 64-bit structs,
 * like LmHeader_FixOffsets_PC), then parse + register the wide geometry blob.
 * size = destination buffer capacity (Pc_BigLm_DestCapacity, 0 if unknown);
 * modelFileIdx = the fileIdx registry key (Pc_BigLm_FileIdxOf) or -1. */
void Pc_WideLm_Parse(s_LmHeader* lmHdr, size_t size, s32 modelFileIdx);

/* True when modelHdr lies inside a registered v7 spine's modelHdrs[] range —
 * the predicate the draw hook branches on. Stock v6 models are never
 * registered, so this is always false for them. */
int Pc_WideLm_IsWide(const s_ModelHeader* modelHdr);

/* The registered wide part for a spine modelHdr, or NULL if modelHdr is not a
 * live v7 model. modelHdr - spineModels is the part ordinal (== model index). */
s_WideLmPart* Pc_WideLm_PartOf(const s_ModelHeader* modelHdr);

/* The wide drawer: clones the stock lit chain (func_8005A21C transform/shade/
 * emit) with the u8 GTE pool removed, so a dense modded part renders at full
 * authored density. Called from the func_80057090 draw hook for v7 models. */
void Pc_WideLm_DrawPart(s_ModelInfo* modelInfo, GsOT_TAG* otTag, s32 arg2,
                        MATRIX* viewMat, MATRIX* worldMat, u16 arg5);

/* Bake material state (runtime tpage, CLUT/UV rebase) into a wide part's prims,
 * cloning Model_MaterialFlagsApply — which skips wide parts because their spine
 * has meshCount==0. Called from Lm_MaterialFlagsApply at the stock bake site so
 * wide prims get the tpage (field_6_0) the stock prims do; without it every wide
 * prim samples VRAM page 0 (invisible body / garbage texture). */
void Pc_WideLm_ApplyMaterial(const s_ModelHeader* modelHdr, s32 matIdx, const s_Material* mat, s32 matFlags);

/* Free a registered entry by fileIdx (lifetime parity with pc_big_lm.c). */
void Pc_WideLm_Free(s32 fileIdx);

#ifdef __cplusplus
}
#endif

#endif /* PC_WIDE_LM_H */
