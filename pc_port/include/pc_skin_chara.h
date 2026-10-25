/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_SKIN_CHARA_H
#define PC_SKIN_CHARA_H

/* Skinned glTF character replacements.
 *
 * A character whose model file has a sibling gamedata/load/<folder>/<NAME>.glb
 * (or .gltf) is drawn from that skinned mesh instead of its rigid ILM parts:
 * every vertex follows the game's own bone matrices through its glTF skin
 * weights, so joints bend smoothly and the model needs no cutting into parts.
 * The skin's joints are matched to the game's bones by name (Mixamo, Blender,
 * XPS, Unreal, Biped or VRoid conventions, with Sketchfab "_NN" suffixes), and
 * the model's bind pose -- T-pose, A-pose, anything -- is retargeted onto the
 * character's rest pose at load. Textures stay full resolution, one per
 * material. */

#include "game.h"

typedef struct _Skeleton s_Skeleton;
typedef struct PcModernDrawBinding PcModernDrawBinding;

#ifdef __cplusplus
extern "C" {
#endif

/* Draw handles carry this bit so the modern-mesh renderer seam can route them. */
#define PC_SKIN_HANDLE_FLAG 0x80000000u

/* Returns the replacement for the character that owns `skel`, or NULL to draw
 * it the stock way. */
void* Pc_SkinChara_Begin(s_Skeleton* skel, const void* boneCoords);
/* Every visible part of the skeleton, with the matrices the stock drawer would
 * have used. Also runs that bone's lighting. */
void  Pc_SkinChara_Bone(void* chara, int boneIdx, MATRIX* worldMat, MATRIX* viewMat);
void  Pc_SkinChara_End(void* chara, void* ot, int shift, int hideHead);
/* Nonzero for a stock part still drawn beside the replacement: a prop the
 * ILM welds into a body part (Cybil's pistol is her 06LGUN/10RGUN hands). */
int   Pc_SkinChara_KeepsStockPart(const char partName[8]);
/* Transforms and lights a replaced part into the scratch slots without drawing
 * it, for a kept part that reads its seam vertices from there
 * (bodyprog_80055028.c). */
typedef struct _ModelInfo s_ModelInfo;
void  Pc_PartFillScratch(s_ModelInfo* modelInfo, MATRIX* viewMat);

int   Pc_SkinChara_PrepareDraw(u32 handle, PcModernDrawBinding* binding);

/* Sets the per-pixel lighting uniforms (and the normal map) on the bound
 * modern program for a skin handle; any other handle switches them off. */
void  Pc_SkinChara_ApplyLighting(u32 handle, unsigned int program);

/* Which file HERO's replacement loads: a play-as swap's base name (HERO_CJ),
 * or NULL for HERO itself. A swap whose file is gone falls back to HERO. */
void  Pc_SkinChara_SetHeroFile(const char* file);

/* HERO's replacement says female=1 in its HERO.ini (drives the voice pitch). */
int   Pc_SkinChara_HeroFemale(void);

/* HERO's replacement moves the flashlight (flashlight_x/y/z, cm): the shift in
 * torso space, Q12 metres. Returns 0 when there is none. */
int   Pc_SkinChara_HeroLightOffset(s32 offQ12[3]);

/* HERO's replacement fist sits elsewhere in the right hand than Harry's, which
 * the held weapons are modelled round: the transform (Q12 rotation, model-unit
 * shift) from the weapon's frame into that bone's, measured at load plus the
 * ini's gun_raise/gun_out/gun_twist. Returns 0 when it is the identity. */
int   Pc_SkinChara_HeroGunAdjust(MATRIX* adj);

#ifdef __cplusplus
}
#endif

#endif
