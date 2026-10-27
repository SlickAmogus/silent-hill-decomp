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

#ifdef SH_XBOX_PORT
/* Split screen (xbox_port/src/split_xbox.c): Player 2 wears a character of its
 * own. Everything play-as keeps per player -- the character, the skin's bind
 * offsets and extra bones -- is this one block, which split screen exchanges with
 * the live state for Player 2's turn (the root height byte in HB_BASE's header is
 * exchanged by split screen itself). Same element types as the statics. */
typedef struct
{
    int           cur;
    int           skinBindT[18][3];
    unsigned char skinBindHas[18];
    short         skinBindFileIdx;
    int           skinBindValid;
    int           extraT[5][3];
    int           extraR[5][9];
    unsigned char extraParent[5];
    int           extraCount;
} PcPlayAsCtx;

void Pc_PlayAs_CtxExchange(PcPlayAsCtx* ctx);

/* The context of character `idx` worn on Harry's rig, built without touching the
 * live player: the skin's binds come from its ANM like a swap would read them.
 * *outRootY is the HB_BASE root height that character needs. Returns 0 when the
 * skin's rig could not be read (the character then wears Harry's proportions). */
int Pc_PlayAs_BuildCtx(int idx, PcPlayAsCtx* out, unsigned char* outRootY);

int Pc_PlayAs_CharaIdOf(int idx);

/* HERO.TIM's virtual texture slot, registered while a skin owns Harry's VRAM
 * parcel. Returns 0 (and leaves *imageDesc alone) when there is none. */
int Pc_PlayAs_HeroTimDesc(void* imageDesc /* s_FsImageDesc* */);
#endif

#ifdef __cplusplus
}
#endif

#endif
