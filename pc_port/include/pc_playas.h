/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_PLAYAS_H
#define PC_PLAYAS_H

/* Play as another character (config: player_character, console: PLAYAS,
 * K-view - / = cycling). Swaps the player's rendered ILM+TIM by retargeting
 * CHARA_FILE_INFOS[Chara_Harry]'s model/texture rows — the same mechanism the
 * PAL Mumbler censorship and the chara pool's beta retargets use — while
 * keeping HB_BASE.ANM, playerBoneCoords and all gameplay logic untouched, so
 * every Harry animation drives the substituted body directly. */

#ifdef __cplusplus
extern "C" {
#endif

/* Apply the config-selected character before the boot-time
 * WorldGfx_HarryCharaLoad. Only retargets table rows; the boot load and
 * WorldGfx_PlayerModelProcessLoad do the rest. */
void Pc_PlayAs_Init(void);

int         Pc_PlayAs_Count(void);
int         Pc_PlayAs_Current(void);
const char* Pc_PlayAs_Label(int idx);   /* uppercase display name */
const char* Pc_PlayAs_Name(int idx);    /* lowercase config/console token */
int         Pc_PlayAs_SkinCharaId(void);/* Chara_Harry when not swapped */
int         Pc_PlayAs_IsFemale(void);   /* gates the optional voice-pitch shift */

/* Select a character. Mid-game this synchronously reloads the player model
 * (held item freed + re-equipped around the disc reads). save != 0 persists
 * the choice to config.cfg. Returns 1 on success. */
int Pc_PlayAs_SetByIndex(int idx, int save);
int Pc_PlayAs_SetByName(const char* name, int save); /* case-insensitive */
int Pc_PlayAs_Cycle(int step);                       /* wraps; returns new index */

/* world_draw.c hooks: */

/* After Pc_BigLm_Redirect in WorldGfx_HarryCharaLoad. A non-HERO player ILM
 * must never land in the HERO-sized PSX slab: disc reads are sector-granular
 * (16 KiB) and overrun HELD_ITEM_LM_BUFFER, so hand out a PC-owned buffer.
 * Loose/v7 redirects (already-owned pointers) pass through untouched. */
void* Pc_PlayAs_PlayerLmRedirect(int modelFileIdx, void* lmHdr);

/* Same place, on the image desc: a skin TIM is larger than the parcel
 * Chara_FsImageCalc sizes for HERO.TIM, and the overrun lands on the chara CLUT
 * shelf below it (rainbow enemies map-wide). Route non-HERO player textures to a
 * virtual pool slot so they never touch VRAM. Must run BEFORE the queued read —
 * the same desc feeds both the upload and harryModel.texture's material bake. */
void Pc_PlayAs_PlayerImageDesc(void* imageDesc /* s_FsImageDesc* */);

/* After WorldGfx_CharaModelProcessLoad(&harryModel): hide the skin's embedded
 * prop meshes (guns/bag/key/extra hands), set up Lisa's hair coords, and
 * register HERO.TIM in a virtual texture slot for the HERO-textured weapons. */
void Pc_PlayAs_OnPlayerModelLoaded(void);

/* Repoint the held-weapon image desc (knife/hammer/axe/handgun/rifle/shotgun
 * PLMs are textured from "HERO" = Harry's VRAM parcel, which now holds the
 * skin's sheet) at the virtual-slot copy of HERO.TIM. */
void Pc_PlayAs_HeldItemImageDesc(void* imageDesc /* s_FsImageDesc* */);

/* True when the Harry hand-variant part toggles (func_8003DE60) must not run:
 * its hard-coded part indices target HERO's part order and scramble any other
 * ILM. The caller re-applies the skin visibility table instead. */
int Pc_PlayAs_SuppressHarryHandVariants(void);
void Pc_PlayAs_ApplySkinVisibility(void);

/* Per-frame from Player_Update: keep Lisa's rigid hair coords composed (their
 * GsCOORDINATE2 flg caches must clear every frame) and re-init them after a
 * warm reset's SysWork_Clear. */
void Pc_PlayAs_PlayerAnimTick(void);

#ifdef __cplusplus
}
#endif

#endif
