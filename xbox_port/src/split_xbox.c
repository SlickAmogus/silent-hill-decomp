/*
 * split_xbox.c - split screen: the session, Player 2, and Player 2's turn.
 *
 * One game, two players. The stock game only knows "the player": the g_SysWork
 * player slots (work, combat, bones, ...) plus a few hundred bytes of per-player
 * globals in player_control.c, item_screens and the camera. Player 2 is a second
 * copy of exactly that state, kept in one static block (s_side) that always holds
 * whichever player is NOT in the slots. Sx_Exchange swaps the block with the live
 * state in place, so the stock player, camera and event code runs for either
 * player unchanged; s_owner says who is in the slots.
 *
 * Each gameplay frame Player 1 runs as stock, then Split_SidePass gives Player 2
 * a turn: pads swapped, context swapped, Player 2's camera, events, update,
 * draws (into both views) and attacks, then everything swapped back. A turn that
 * starts something modal -- a door, a menu, pause, a pickup, a game over -- ends
 * with Player 2 left in the slots as the owner, so that state plays out for
 * Player 2 exactly as it would for one player; Split_AfterDispatch hands the
 * slots back to Player 1 once the game is back in gameplay.
 *
 * Session: OFF -> MENU (title's Multiplayer menu) -> BOOTING (the stock New Game /
 * Continue / Load runs) -> ARMED (GameBoot step 0: exterior streaming widens for
 * two players) -> ACTIVE (first gameplay frame: Player 2 joins beside Player 1).
 * A game over keeps the session and the title reopens the menu on Continue;
 * every other way back to the title ends it. All of it lives in statics here,
 * never in g_SysWork / g_GameWork (warm boot clears one, a memcard load the other).
 *
 * Deaths: a player who dies while the partner is up holds in the death pose,
 * blinks, and gets back up behind the partner (the online branch's respawn); when
 * both are down the stock game over runs.
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/player.h"
#include "bodyprog/items.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/collision/collision.h"
#include "bodyprog/gfx/world.h"
#include "bodyprog/math/math.h"
#include "bodyprog/model.h"
#include "bodyprog/screen/cutscene_border.h"
#include "bodyprog/sys/joy.h"
#include "bodyprog/view/vw_system.h"
#include "bodyprog/view/vc_main.h"
#include "bodyprog/view/vc_util.h"
#include "main/fsqueue.h"
#include "main/fileinfo.h"
#include "gpu_nv2a.h"
#include "hires_override.h"
#include "pc_config.h"
#include "pc_playas.h"
#include "xbox_respool.h"
#include "split_xbox.h"
#include "split_internal_xbox.h"
#include "sh_log.h"

#include <stdlib.h>
#include <string.h>

/* ---- outside this file ---- */

/* pad_xbox.c */
extern int  Pad_XboxBindP2(int port);
extern void Pad_XboxUnbindP2(void);
extern void Pad_XboxLatchP2(void);
extern int  Pad_XboxPlayerPort(int p);
extern int  Pad_XboxP2Connected(void);
extern void Pad_XboxP2Pulse(unsigned level, unsigned ms);
/* psx_libgpu_xbox.c */
extern int  VSync_Paced(int n);
extern int  VSync(int mode);
/* gpu_nv2a.c / gpu_xbox.c */
extern void GpuNv2a_SplitPool(int on);
extern int  GpuNv2a_Ms(void);
extern void GpuXbox_ViewEnd(void);
/* game_main.c */
extern int  Pc_ScriptOwnsShot(void);
extern void Pc_Pick_CharaPreDraw(struct _SubCharacter* charaPtr, int slot, void* boneCoordsPtr);
extern void Xbox_ApplyControlStyle(int s);
extern int  g_ControlStyle;
/* world_draw.c / bodyprog_80040B74.c */
extern void   Ipd_SplitStreamPointsGet(q19_12 out[4]);
extern int    g_XbSplitStreamValid;
extern q19_12 g_XbSplitStreamPos[4];
/* player_control.c */
extern void*  Player_XbSplitLocals(unsigned* size);
extern void   Player_XbSplitLocalsReset(void);
extern void   func_8007E9C4(void);
extern void   Anim_BoneInit(s_AnmHeader* anmHdr, GsCOORDINATE2* boneCoords);
extern void   Player_CombatUpdate(s_SubCharacter* player, GsCOORDINATE2* coord);
extern void   func_8008A3AC(s_SubCharacter* chara);
extern void   Chara_Flag8Clear(s_SubCharacter* chara);
extern q19_12 D_800C4550;
extern s8     D_800C4560;
extern s16    D_800C4556;
extern s32    g_Player_HasActionInput;
extern u8     g_Player_DisableDamage;
extern u8     D_800C4588;
extern u16    g_Player_IsAiming, g_Player_IsSteppingLeftTap, g_Player_IsSteppingRightTap,
              g_Player_IsTurningLeft, g_Player_IsTurningRight, g_Player_IsSteppingLeftHold,
              g_Player_IsSteppingRightHold, g_Player_IsHoldAttack, g_Player_IsAttacking,
              g_Player_IsShooting, g_Player_IsMovingForward, g_Player_IsMovingBackward,
              g_Player_IsRunning;
extern int    g_PcNativeDistAccrued;
extern s32    g_Player_LastMoveStep;
extern q3_12  g_Player_FlexRotationX;
/* xbox_extra_actions.c */
extern int g_PcReloadRequest, g_PcQuickTurnRequest;
/* game_sys_states.c (its state table is static there) */
extern void SysState_Gameplay_Update(void);
/* world_draw.c */
extern void   func_8003DA9C(e_CharaId charaId, GsCOORDINATE2* boneCoords, s32 arg2, q3_12 timer, s32 arg4);
extern void   func_8003D01C(void);
extern s32    WorldGfx_PlayerPrevHeldItem(s_PlayerCombat* combat);
extern void   Gfx_PlayerHeldItemAttach(u8 weaponAttack);
/* map_effects.c (Player 2's light) */
extern q19_12 func_8003F4DC(GsCOORDINATE2** coords, SVECTOR* rot, q19_12 alpha, s32 arg3, u32 arg4, s_SysWork* sysWork);
extern void   func_800554C4(s32 arg0, s16 arg1, GsCOORDINATE2* coord0, GsCOORDINATE2* coord1, SVECTOR* rot,
                            q19_12 x, q19_12 y, q19_12 z, s_WaterZone* waterZones);
extern void   func_80055814(s32 arg0);
extern s32    Vc_LensFlareTypeGet(void);
extern s_WorldEnvWork g_WorldEnvWork;
extern VECTOR3        g_PcFlashlightShadowWorld;
extern int            g_PsyX_FlashlightFpsMode;
/* split_audio_xbox.c / split_npc_xbox.c */
void SplitAudio_Reset(void);
void SplitAudio_FrameTick(void);
void SplitAudio_ListenerCapture(int k);

/* ---- session ---- */

int g_XbSplitActive;
int g_XbSplitTwoViews;
int g_XbSplitSidePass;

enum
{
    SS_OFF = 0,
    SS_MENU,
    SS_BOOTING,
    SS_ARMED,
    SS_ACTIVE
};

static int s_state;
static int s_resume;          /* a split game over: the title reopens the menu on Continue */
static int s_bootEntry = -1;  /* the stock title entry to run for a split start */
static int s_placePending;    /* a load passed step 0: place the side player at the next InGame entry */
static int s_owner;           /* the player in the g_SysWork slots */
static int s_viewOwner;       /* whose camera OT0 is built from this frame */
static int s_npcSwapped;      /* the NPC subset of the side block is exchanged */
static int s_padsSwapped;     /* controllers[0] and [1] are exchanged */
static int s_sideInWorld;     /* the side player has been placed in this world */
static int s_sideParked;      /* ...but sits out a scene (hidden, frozen) */
static int s_paceN = 2;       /* split pacing: vblanks per frame (2 = 30fps, 3 = 20fps) */
static int s_pubSwapPending;  /* the slots changed hands mid-frame: the cameras follow next frame */
static int s_handBack;        /* the slots came back to Player 1 this frame */
static int s_lastKind;        /* the menu row that started the session */

/* What split screen forces for its duration (single-instance state the second
 * player cannot share), and puts back. Never persisted. */
static int s_savedStyle, s_savedControl2d, s_savedMovementOriginal, s_savedDarkHarry, s_forced;

/* ---- the side block ---- */

typedef struct
{
    void*    p;
    unsigned n;
    unsigned off;
    u8       npc; /* part of the subset monsters see (Split_SwapNpcSet) */
} SxEnt;

#define SX_MAX_ENT   112
#define SX_SIDE_SIZE 8192

static SxEnt         s_ent[SX_MAX_ENT];
static int           s_entCount;
static unsigned      s_sideBytes;
static int           s_tableBad;
static unsigned char s_side[SX_SIDE_SIZE] __attribute__((aligned(16)));

static unsigned s_offCombat, s_offWork, s_offE30, s_offLatch, s_offTargetNpc, s_offNpcIdxs,
                s_offLastWeapon, s_offHeld, s_offModelPtr, s_offMapEvent, s_offItemEv,
                s_offItemIds;

/* The savegame fields that belong to a player, the HB_BASE root height and the
 * play-as context, exchanged alongside the table. */
typedef struct
{
    s8     mapRoomIdx;
    u8     equippedWeapon;
    q19_12 healthSaturation;
} SxSave;

static SxSave      s_sideSave;
static u8          s_sideRootY;
static PcPlayAsCtx s_sidePlayAs;

/* Player 2's own assets. The bone mirror holds Player 2's posed bones after its
 * draws, for the weapon effects that anchor to the bone array they are given
 * (static: COORD_PTR_OK refuses heap coords). */
static s_CharaModel  s_p2Model;
static GsCOORDINATE2 s_sideBoneMirror[HarryBone_Count + 5];
static u8*           s_p2Ilm;
static int           s_p2IlmCap;
static u8*           s_p2Plm;
static u8*           s_anmImg;      /* the weapon keyframe window of the player whose tag is s_imgTag */
static int           s_imgTag = -1;
static int           s_p2ModelOk;   /* s_p2Model is loaded and baked */
static int           s_p2ReqIdx = -1;   /* the character asked for (Split_P2CharIndex) */
static int           s_p2ModelIdx = -1; /* the character actually worn, -1 = Player 1's model */
static int           s_p2SlotTex;   /* the skin's texture is in slot 301 */
static int           s_weaponSlotOk;/* Player 2's own-texture weapons use slot 302 */
static u8            s_equipIn[2];  /* each player's weapon when it last came into the slots */

#define P2_SKIN_SLOT   (HIRES_POOL_PLAYAS_SLOT + 1)
#define P2_WEAPON_SLOT (HIRES_POOL_PLAYAS_SLOT + 2)
#define ANM_WINDOW     ((unsigned)((u8*)FS_BUFFER_4 - (u8*)FS_BUFFER_12))
#define PLM_CAP        (2048 + 16)

static unsigned Sx_Add(void* p, unsigned n, int npc)
{
    unsigned off = (s_sideBytes + 3u) & ~3u;

    if (s_entCount >= SX_MAX_ENT || off + n > SX_SIDE_SIZE)
    {
        s_tableBad = 1;
        return 0;
    }
    s_ent[s_entCount].p   = p;
    s_ent[s_entCount].n   = n;
    s_ent[s_entCount].off = off;
    s_ent[s_entCount].npc = (u8)npc;
    s_entCount++;
    s_sideBytes = off + n;
    return off;
}

#define SX_ADD(v, npc) Sx_Add(&(v), (unsigned)sizeof(v), (npc))

/* Everything that follows a player, per player-state.md. Never in here: the
 * vibration engine lists (field_2510/2514/254C, self-referential), the light and
 * lens-flare bone pointers (they point at the slots and follow whoever is there),
 * the published view (per view, split_render_xbox.c), harryModel itself. */
static void Sx_TableInit(void)
{
    unsigned locSize = 0;
    void*    loc;

    if (s_entCount)
        return;

    s_offCombat = SX_ADD(g_SysWork.playerCombat, 1);
    s_offWork   = SX_ADD(g_SysWork.playerWork, 1);
    SX_ADD(g_SysWork.playerBoneCoords, 0);
    s_offE30 = SX_ADD(g_SysWork.unkCoords_E30, 0);
    s_offLatch     = SX_ADD(g_SysWork.charaGroupFlags[3], 1);
    s_offTargetNpc = SX_ADD(g_SysWork.targetNpcIdx, 0);
    s_offNpcIdxs   = SX_ADD(g_SysWork.npcIdxs, 1);
    SX_ADD(g_SysWork.enablePlayerMatchAnim, 0);
    SX_ADD(g_SysWork.playerStopFlags, 0);
    SX_ADD(g_SysWork.cameraAngleY, 0);
    SX_ADD(g_SysWork.field_275C, 0);
    SX_ADD(g_SysWork.field_2760, 0);
    SX_ADD(g_SysWork.field_2764, 0);

    SX_ADD(D_800C44F0, 0);
    SX_ADD(g_TargetEnemyPosition, 0);
    SX_ADD(D_800C454C, 0);
    SX_ADD(D_800C4550, 0);
    SX_ADD(D_800C4554, 0);
    SX_ADD(D_800C4556, 0);
    SX_ADD(g_Player_HasMoveInput, 0);
    SX_ADD(g_Player_HasActionInput, 0);
    SX_ADD(D_800C4560, 0);
    SX_ADD(g_Player_IsDead, 1);
    SX_ADD(g_Player_DisableDamage, 1);
    SX_ADD(g_Player_EquippedWeaponInfo, 0);
    SX_ADD(D_800C457C, 0);
    SX_ADD(D_800C4588, 0);
    SX_ADD(g_Player_IsAiming, 1);
    SX_ADD(g_Player_IsSteppingLeftTap, 0);
    SX_ADD(g_Player_IsSteppingRightTap, 0);
    SX_ADD(g_Player_IsTurningLeft, 0);
    SX_ADD(g_Player_IsTurningRight, 0);
    SX_ADD(g_Player_IsSteppingLeftHold, 0);
    SX_ADD(g_Player_IsSteppingRightHold, 0);
    SX_ADD(g_Player_IsHoldAttack, 0);
    SX_ADD(g_Player_IsAttacking, 0);
    SX_ADD(g_Player_IsShooting, 0);
    SX_ADD(g_Player_IsMovingForward, 0);
    SX_ADD(g_Player_IsMovingBackward, 0);
    SX_ADD(g_Player_IsRunning, 0);
    SX_ADD(D_800C4590, 0);
    SX_ADD(D_800C45B0, 0);
    SX_ADD(D_800C45C8, 0);
    SX_ADD(g_Player_PrevPosition, 0);
    SX_ADD(g_PcNativeDistAccrued, 0);
    SX_ADD(g_Player_HeadingAngle, 0);
    SX_ADD(D_800C4610, 0);
    SX_ADD(g_Player_LastMoveStep, 0);
    loc = Player_XbSplitLocals(&locSize);
    Sx_Add(loc, locSize, 0);

    SX_ADD(g_Player_GrabReleaseInputTimer, 0);
    SX_ADD(g_Player_FlexRotationY, 0);
    SX_ADD(g_Player_FlexRotationX, 0);
    SX_ADD(g_Player_IsInWalkToRunTransition, 0);
    SX_ADD(g_Player_DisableControl, 0);
    SX_ADD(D_800AF216, 0);
    SX_ADD(g_Player_RockDrill_DirectionAttack, 0);
    SX_ADD(g_Player_TargetNpcIdx, 0);
    SX_ADD(D_800AF220, 0);
    SX_ADD(g_Player_AttackAnimIdx, 0);
    SX_ADD(g_Player_MeleeAttackType, 0);
    SX_ADD(g_Player_IsMultiTapAttack, 0);
    SX_ADD(g_Inventory_EquippedItem, 0);
    Sx_Add(&HARRY_BASE_ANIM_INFOS[0], (unsigned)sizeof(HARRY_BASE_ANIM_INFOS[0]), 0);
    Sx_Add(&HARRY_BASE_ANIM_INFOS[56], (unsigned)sizeof(HARRY_BASE_ANIM_INFOS[0]) * 20, 0);
    SX_ADD(D_800AF624, 0);
    SX_ADD(D_800AF626, 0);
    s_offLastWeapon = SX_ADD(g_Player_LastWeaponSelected, 0);
    SX_ADD(g_Player_WeaponAttack, 0);
    SX_ADD(D_800C3960, 0);
    SX_ADD(D_800C3961, 0);
    SX_ADD(D_800C3962, 0);
    SX_ADD(D_800C3963, 0);
    SX_ADD(g_PcReloadRequest, 0);
    SX_ADD(g_PcQuickTurnRequest, 0);

    s_offHeld     = SX_ADD(g_WorldGfxWork.heldItem, 0);
    s_offModelPtr = SX_ADD(g_WorldGfxWork.registeredCharaModels[Chara_Harry], 0);

    SX_ADD(vcWork, 0);
    SX_ADD(vcSelfViewTimer, 0);
    SX_ADD(g_WorldGfxWork.vcCameraInternalInfo.mv_smooth, 0);

    s_offMapEvent = SX_ADD(g_MapEventData, 0);
    s_offItemEv   = Sx_Add(g_ItemTriggerEvents, (unsigned)(sizeof(s_EventData*) * 5), 0);
    s_offItemIds  = SX_ADD(g_ItemTriggerItemIds, 0);

    SH_DBG("[SPLIT] player context: %d fields, %u bytes%s", s_entCount, s_sideBytes,
           s_tableBad ? " -- TOO BIG, split screen disabled" : "");
}

static void Sx_MemSwap(void* a, void* b, unsigned n)
{
    unsigned char  t[64];
    unsigned char* pa = (unsigned char*)a;
    unsigned char* pb = (unsigned char*)b;

    while (n)
    {
        unsigned k = n > sizeof(t) ? (unsigned)sizeof(t) : n;

        memcpy(t, pa, k);
        memcpy(pa, pb, k);
        memcpy(pb, t, k);
        pa += k;
        pb += k;
        n  -= k;
    }
}

#define SIDE(type, off) ((type*)(s_side + (off)))

/* ---- weapons: keyframe window and ammo ---- */

/* The HB_WEPx.ANM GameFs_WeaponInfoUpdate reads into FS_BUFFER_12 for a weapon,
 * or -1. */
static int Sx_WeaponAnm(s32 weaponAttack)
{
    switch (weaponAttack)
    {
        case EquippedWeaponId_KitchenKnife: return FILE_ANIM_HB_WEP3_ANM;
        case EquippedWeaponId_Axe:          return FILE_ANIM_HB_WEP1_ANM;
        case EquippedWeaponId_SteelPipe:
        case EquippedWeaponId_Hammer:       return FILE_ANIM_HB_WEP2_ANM;
        case EquippedWeaponId_Chainsaw:     return FILE_ANIM_HB_WEP6_ANM;
        case EquippedWeaponId_RockDrill:    return FILE_ANIM_HB_WEP8_ANM;
        case EquippedWeaponId_Katana:       return FILE_ANIM_HB_WEP9_ANM;
        case EquippedWeaponId_Handgun:      return FILE_ANIM_HB_WEP4_ANM;
        case EquippedWeaponId_HuntingRifle: return FILE_ANIM_HB_WEP51_ANM;
        case EquippedWeaponId_Shotgun:      return FILE_ANIM_HB_WEP52_ANM;
        case EquippedWeaponId_HyperBlaster: return FILE_ANIM_HB_WEP53_ANM;
        default:                            return -1;
    }
}

/* FS_BUFFER_12 holds the slot player's weapon keyframes (the window the weapon's
 * animations sample, inside HB_BASE's frames). One spare image holds the other
 * player's, swapped in only when the two players' weapons animate differently. A
 * player with no weapon loaded needs nothing; its partner's window is kept. */
static void Sx_WeaponWindow(int teardown)
{
    const int out = Sx_WeaponAnm(g_Player_LastWeaponSelected);
    const int in  = Sx_WeaponAnm(*SIDE(s32, s_offLastWeapon));

    if (s_anmImg == NULL)
        return;
    if (in < 0 || in == out)
    {
        if (out >= 0 && s_imgTag != out)
        {
            memcpy(s_anmImg, FS_BUFFER_12, ANM_WINDOW);
            s_imgTag = out;
        }
        return;
    }
    if (s_imgTag == in)
    {
        Sx_MemSwap(FS_BUFFER_12, s_anmImg, ANM_WINDOW);
        s_imgTag = out;
        return;
    }
    if (out >= 0)
    {
        memcpy(s_anmImg, FS_BUFFER_12, ANM_WINDOW);
        s_imgTag = out;
    }
    if (!teardown)
    {
        Fs_QueueStartRead(in, FS_BUFFER_12);
        Fs_QueueWaitForEmpty();
        SH_DBG("[SPLIT] weapon keyframes reloaded for the incoming player (file %d)", in);
    }
}

static s32 Sx_ItemSlot(u8 id)
{
    s32 i;

    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
        if (g_SavegamePtr->items[i].id_0 == id)
            return i;
    return NO_VALUE;
}

static int Sx_IsGun(u8 id)
{
    return id != InvItemId_Unequipped && INV_ITEM_GROUP(id) == InvItemGroup_GunWeapons;
}

/* The inventory and the ammo reserve are shared; each player keeps the rounds in
 * its own magazine (playerCombat). The weapon item's count mirrors the magazine
 * of whoever is in the slots, as func_8004F190 would derive it for one player. */
static void Sx_AmmoFixIncoming(void)
{
    const u8 eq = g_SavegamePtr->equippedWeapon;
    s32      i, a;

    if (eq == InvItemId_Unequipped)
        return;
    i = Sx_ItemSlot(eq);
    if (i == NO_VALUE)
        return;
    g_SysWork.playerCombat.weaponInventoryIdx = (s8)i;
    if (Sx_IsGun(eq))
    {
        a = Sx_ItemSlot((u8)INV_WEAPON_AMMO_ID(eq));
        g_SysWork.playerCombat.totalWeaponAmmo = (a == NO_VALUE) ? 0 : g_SavegamePtr->items[a].count_1;
        g_SavegamePtr->items[i].count_1        = g_SysWork.playerCombat.currentWeaponAmmo;
    }
}

/* A player who picked up the gun the partner is holding starts with an empty
 * magazine: the rounds the item showed were the partner's. The stock empty-pull
 * reload then fills it from the shared reserve. */
static void Sx_SameGunRule(int outP)
{
    const u8 eq = g_SavegamePtr->equippedWeapon;

    if (eq != s_equipIn[outP] && eq == s_sideSave.equippedWeapon && Sx_IsGun(eq))
        g_SysWork.playerCombat.currentWeaponAmmo = 0;
}

/* ---- the exchange ---- */

static void Sx_ExchangeEx(int teardown)
{
    int i;

    if (s_tableBad || !s_entCount)
        return;
    if (s_npcSwapped)
        Split_SwapNpcSet();

    Sx_SameGunRule(s_owner);
    Sx_WeaponWindow(teardown);

    for (i = 0; i < s_entCount; i++)
        Sx_MemSwap(s_ent[i].p, s_side + s_ent[i].off, s_ent[i].n);

    {
        s8     r = g_SavegamePtr->mapRoomIdx;
        u8     e = g_SavegamePtr->equippedWeapon;
        q19_12 h = g_SavegamePtr->healthSaturation;

        g_SavegamePtr->mapRoomIdx       = s_sideSave.mapRoomIdx;
        g_SavegamePtr->equippedWeapon   = s_sideSave.equippedWeapon;
        g_SavegamePtr->healthSaturation = s_sideSave.healthSaturation;
        s_sideSave.mapRoomIdx           = r;
        s_sideSave.equippedWeapon       = e;
        s_sideSave.healthSaturation     = h;
    }
    {
        u8 y = ((u8*)FS_BUFFER_0)[0x13];

        ((u8*)FS_BUFFER_0)[0x13] = s_sideRootY;
        s_sideRootY              = y;
    }
    Pc_PlayAs_CtxExchange(&s_sidePlayAs);

    s_owner ^= 1;
    s_equipIn[s_owner] = g_SavegamePtr->equippedWeapon;
    if (!teardown)
        Sx_AmmoFixIncoming();
}

static void Sx_Exchange(void)
{
    Sx_ExchangeEx(0);
}

static void Sx_PadsSwap(void)
{
    s_ControllerData t = g_GameWork.controllers[0];

    g_GameWork.controllers[0] = g_GameWork.controllers[1];
    g_GameWork.controllers[1] = t;
    s_padsSwapped ^= 1;
}

/* Who is where. */
static int Sx_WorkOwner(void)
{
    return s_owner ^ s_npcSwapped;
}

int Split_SlotOwner(void)
{
    return s_owner;
}

int Split_WorkOwner(void)
{
    return Sx_WorkOwner();
}

int Split_ViewOwner(void)
{
    return s_viewOwner;
}

struct _SubCharacter* Split_PlayerChara(int p)
{
    if ((p & 1) == Sx_WorkOwner())
        return &g_SysWork.playerWork.player;
    return &SIDE(s_PlayerWork, s_offWork)->player;
}

struct _SubCharacter* Split_SideChara(void)
{
    return Split_PlayerChara(Sx_WorkOwner() ^ 1);
}

int Split_P2InWorld(void)
{
    return s_state == SS_ACTIVE && s_sideInWorld && !s_sideParked;
}

int Split_PlayerNpcIdx(int p, int i)
{
    i &= 3;
    if ((p & 1) == Sx_WorkOwner())
        return g_SysWork.npcIdxs[i];
    return SIDE(s8, s_offNpcIdxs)[i];
}

unsigned short* Split_PlayerLatch(int p)
{
    if ((p & 1) == Sx_WorkOwner())
        return &g_SysWork.charaGroupFlags[3];
    return SIDE(u16, s_offLatch);
}

int Split_PlayerTargetNpc(int p)
{
    if ((p & 1) == s_owner)
        return g_SysWork.targetNpcIdx;
    return *SIDE(s8, s_offTargetNpc);
}

/* The part of a player monsters read and write: its work (position, health,
 * damage, state), combat, the commit latch, who holds it, and three flags. */
void Split_SwapNpcSet(void)
{
    int i;

    if (s_tableBad || !s_entCount)
        return;
    for (i = 0; i < s_entCount; i++)
        if (s_ent[i].npc)
            Sx_MemSwap(s_ent[i].p, s_side + s_ent[i].off, s_ent[i].n);
    s_npcSwapped ^= 1;
}

/* The side player, for chara-vs-chara movement (collision.c). */
s_SubCharacter* Split_CollidableSideChara(void)
{
    if (!g_XbSplitActive || !Split_P2InWorld())
        return NULL;
    return Split_SideChara();
}

/* ---- respawn ---- */

#define RS_BLINK_LEN    Q12(1.5f)
#define RS_BLINK_PERIOD Q12(0.06f)
#define RS_GRACE        Q12(2.0f)
#define RS_SETTLE_FRAMES 90
#define RS_NO_FLOOR     Q12(8.0f)

enum
{
    RS_ALIVE = 0,
    RS_HOLD,
    RS_BLINK,
    RS_SETTLE
};

typedef struct
{
    int    phase;
    q19_12 elapsed;
    int    settled;
    int    settleFrames;
    q19_12 graceLeft;
} SxResp;

static SxResp s_resp[2];

static int Sx_Holding(int p)
{
    return s_resp[p & 1].phase == RS_HOLD || s_resp[p & 1].phase == RS_BLINK;
}

static int Sx_IsDeathState(int st)
{
    return st == PlayerState_Death || st == PlayerState_InstantDeath || st == PlayerState_Unk36 ||
           st == PlayerState_EnemyGrabPinnedFront || st == PlayerState_EnemyGrabPinnedBack ||
           st == PlayerState_OnFloorFront || st == PlayerState_OnFloorBehind;
}

/* Up = can be hunted and can carry the game: alive, not waiting to respawn, in
 * the world. */
static int Sx_PlayerUp(int p)
{
    p &= 1;
    if (Sx_Holding(p))
        return 0;
    if (p != s_owner && !Split_P2InWorld())
        return 0;
    return Split_PlayerChara(p)->health > Q12(0.0f);
}

int Split_PlayerTargetable(int p)
{
    if (s_state != SS_ACTIVE)
        return (p & 1) == s_owner;
    return Sx_PlayerUp(p);
}

/* The player in the slots reached a game over (player_control.c's five death
 * sites). With the partner still up this player holds instead: returns 1, and
 * the death state keeps the corpse pose until the respawn. Both down: 0, and the
 * stock game over runs -- with the partner's health restored too, or the in-game
 * update would cancel the game over the moment that player came back into the
 * slots (game_sys_states.c: a non-gameplay state with a dead slot player is
 * forced back to gameplay). The alley's scripted death stays vanilla. */
int Split_DeathHold(void)
{
    const int me    = Sx_WorkOwner();
    const int other = me ^ 1;

    if (!g_XbSplitActive || g_SavegamePtr->mapIdx == MapIdx_MAP0_S00)
        return 0;
    if (Sx_Holding(me))
        return 1;
    if (!Sx_PlayerUp(other))
    {
        Split_PlayerChara(other)->health = Q12(100.0f);
        s_resp[other].phase              = RS_ALIVE;
        SH_DBG("[SPLIT] both players down: game over");
        return 0;
    }
    s_resp[me].phase   = RS_HOLD;
    s_resp[me].elapsed = 0;
    SH_DBG("[SPLIT] Player %d down, back up in %d s", me + 1, g_PcConfig.splitRespawnSec + 1);
    return 1;
}

int Split_DeathHolding(void)
{
    return g_XbSplitActive && Sx_Holding(Sx_WorkOwner());
}

static int Sx_ProbeGround(s_SubCharacter* c)
{
    s_CollisionSurface coll;

    Collision_SurfaceGet(&coll, c->position.vx, c->position.vz);
    if (coll.groundHeight == RS_NO_FLOOR)
        return 0;
    c->position.vy                    = coll.groundHeight;
    c->properties.player.groundHeight = coll.groundHeight;
    c->fallSpeed                      = 0;
    return 1;
}

/* The slot player's camera, snapped behind it (vcSetCameraUseWarp's pose, minus
 * the rail flag, which is shared and belongs to whoever opened the door). After a
 * map change the camera's road list is the old map's: reinit rebuilds it. */
static void Sx_CameraSnap(int reinit)
{
    s_SubCharacter* p   = &g_SysWork.playerWork.player;
    q3_12           rot = p->rotation.vy;
    VECTOR3         cam;

    if (reinit)
    {
        g_WorldGfxWork.vcCameraInternalInfo.mv_smooth = VC_MV_CHASE;
        vcInitVCSystem(g_MapOverlayHdr.cameraPaths);
        vcStartCameraSystem();
    }
    cam.vx = p->position.vx - Q12_MULT(Math_Sin(rot), Q12(1.5f));
    cam.vy = p->position.vy - Q12(1.7f);
    cam.vz = p->position.vz - Q12_MULT(Math_Cos(rot), Q12(1.5f));
    vcSetFirstCamWork(&cam, rot, false);
}

/* Stand the slot player up at pos/rot: Chara_PositionSet's placement, the state
 * machine reset, idle and controllable, camera behind. */
static void Sx_StandAt(const VECTOR3* pos, q3_12 rot, int reinitCam)
{
    s_SubCharacter* p = &g_SysWork.playerWork.player;

    p->position    = *pos;
    p->rotation.vx = 0;
    p->rotation.vy = rot;
    p->rotation.vz = 0;
    p->fallSpeed   = 0;
    p->moveSpeed   = 0;
    g_SysWork.cameraAngleY = rot;
    func_8007E9C4();
    Game_MapRoomIdxUpdate();
    g_MapOverlayHdr.playerControlUnfreeze(true);
    Player_AnimFlagsSet(AnimFlag_Unlocked | AnimFlag_Visible);
    Sx_ProbeGround(p);
    Sx_CameraSnap(reinitCam);
}

/* Walk the slot player by (ox, oz) through its own wall query in 0.15 m steps
 * (the online branch's ShNetG_WalkOffset): it can never end up inside a wall.
 * Returns the distance covered. */
static q19_12 Sx_WalkOffset(q19_12 ox, q19_12 oz)
{
    s_SubCharacter* me   = &g_SysWork.playerWork.player;
    q19_12          len  = Vc_VectorMagnitudeCalc(ox, Q12(0.0f), oz);
    q19_12          done = 0;

    while (len - done > Q12(0.01f))
    {
        s_CollisionResult res;
        VECTOR3           off;
        q19_12            step = (len - done > Q12(0.15f)) ? Q12(0.15f) : len - done;
        q19_12            got;

        off.vx = (q19_12)(((s64)ox * step) / len);
        off.vy = Q12(0.0f);
        off.vz = (q19_12)(((s64)oz * step) / len);
        Collision_CharaCollisionSetup(&res, &off, me);
        me->position.vx += res.offset.vx;
        me->position.vz += res.offset.vz;
        got   = Vc_VectorMagnitudeCalc(res.offset.vx, Q12(0.0f), res.offset.vz);
        done += step;
        if (got < step / 2)
            return done - step + got;
    }
    return len;
}

/* Put the slot player beside (x, z) facing rot: 0.7 m to the right, else the
 * left, else whichever side got further. */
static void Sx_StepBeside(const VECTOR3* at, q3_12 rot)
{
    s_SubCharacter* me  = &g_SysWork.playerWork.player;
    const q19_12    ox  = Q12_MULT(Math_Cos(rot), Q12(0.7f));
    const q19_12    oz  = -Q12_MULT(Math_Sin(rot), Q12(0.7f));
    q19_12          gotR, gotL;

    me->position = *at;
    gotR         = Sx_WalkOffset(ox, oz);
    if (gotR >= Q12(0.35f))
        return;
    me->position = *at;
    gotL         = Sx_WalkOffset(-ox, -oz);
    if (gotL >= gotR)
        return;
    me->position = *at;
    Sx_WalkOffset(ox, oz);
}

/* Back up 0.8 m behind the partner, on a floor near their height; else on
 * their spot. Runs with the respawning player in the slots. */
static void Sx_Respawn(int p)
{
    s_SubCharacter* me      = &g_SysWork.playerWork.player;
    s_SubCharacter* partner = Split_PlayerChara(p ^ 1);
    q3_12           rot     = partner->rotation.vy;
    VECTOR3         pos;
    s_CollisionSurface coll;

    pos.vx = partner->position.vx - Q12_MULT_FLOAT_PRECISE(Math_Sin(rot), 0.8f);
    pos.vz = partner->position.vz - Q12_MULT_FLOAT_PRECISE(Math_Cos(rot), 0.8f);
    pos.vy = partner->position.vy;
    Collision_SurfaceGet(&coll, pos.vx, pos.vz);
    if (coll.groundHeight == RS_NO_FLOOR || ABS(coll.groundHeight - partner->position.vy) > Q12(1.5f))
        pos = partner->position;

    Sx_StandAt(&pos, rot, 0);
    me->health = Q12(100.0f);

    s_resp[p].settled      = Sx_ProbeGround(me);
    s_resp[p].settleFrames = RS_SETTLE_FRAMES;
    s_resp[p].graceLeft    = RS_GRACE;
    s_resp[p].phase        = RS_SETTLE;
    s_resp[p].elapsed      = 0;
    g_Player_DisableDamage = true;
    SH_DBG("[SPLIT] Player %d back on their feet beside Player %d", p + 1, (p ^ 1) + 1);
}

/* One step of player p's respawn, with p in the slots. The clock only runs in
 * gameplay (menus and messages hold it). */
static void Sx_RespawnTick(int p)
{
    SxResp*         r = &s_resp[p & 1];
    s_SubCharacter* c = &g_SysWork.playerWork.player;

    switch (r->phase)
    {
        case RS_HOLD:
        case RS_BLINK:
            if (!Sx_IsDeathState((int)g_SysWork.playerWork.extra.state))
            {
                /* A script or a load stood this player up already. */
                Model_AnimFlagsSet(&c->model, AnimFlag_Visible);
                r->phase = RS_ALIVE;
                return;
            }
            r->elapsed += g_DeltaTime;
            if (r->phase == RS_HOLD)
            {
                if (r->elapsed >= Q12(g_PcConfig.splitRespawnSec > 0 ? g_PcConfig.splitRespawnSec : 7))
                {
                    r->phase   = RS_BLINK;
                    r->elapsed = 0;
                }
                return;
            }
            if (((r->elapsed / RS_BLINK_PERIOD) & 1) == 0)
                Model_AnimFlagsSet(&c->model, AnimFlag_Visible);
            else
                Model_AnimFlagsClear(&c->model, AnimFlag_Visible);
            if (r->elapsed >= RS_BLINK_LEN)
                Sx_Respawn(p & 1);
            return;

        case RS_SETTLE:
            if (!r->settled && r->settleFrames > 0)
            {
                r->settleFrames--;
                r->settled = Sx_ProbeGround(c);
            }
            if (r->graceLeft > 0)
            {
                r->graceLeft -= g_DeltaTime;
                if (r->graceLeft <= 0)
                    g_Player_DisableDamage = false;
            }
            if (r->graceLeft <= 0 && (r->settled || r->settleFrames == 0))
                r->phase = RS_ALIVE;
            return;

        default:
            return;
    }
}

/* ---- Player 2's character ---- */

static void Sx_PumpQueue(void)
{
    s32 pump = 0;

    while (Fs_QueueGetLength() > 0 && pump++ < 100000)
        Fs_QueueUpdate();
}

/* The model desc a skin gets: virtual pool slot 301, like play-as's 300. */
static void Sx_SlotDesc(s_FsImageDesc* d, int slot)
{
    memset(d, 0, sizeof(*d));
    d->tPage[0] = 0;
    d->tPage[1] = 28;
    d->u        = 0;
    d->v        = 0;
    d->clutX    = (s16)((slot % 64) * 16);
    d->clutY    = (s16)(HIRES_POOL_CLUT_ROW_BASE + (slot / 64) * HIRES_POOL_MAX_ROWS);
}

/* Read and bake Player 2's body into s_p2Model: the ILM into a private buffer,
 * a skin's TIM into slot 301, then WorldGfx_CharaModelProcessLoad's bake against
 * the skin's own TIM name (the material match is by name). Runs with Player 1 in
 * the slots, at a frame boundary (InGame entry, after DrawSync). Harry needs no
 * texture of his own: he samples Player 1's HERO parcel, or HERO.TIM's virtual
 * copy when Player 1 wears a skin. Returns 0 to leave Player 2 on Player 1's
 * model. */
static int Sx_P2ModelLoad(int idx)
{
    const int     charaId = Pc_PlayAs_CharaIdOf(idx);
    const int     skin    = charaId != Chara_Harry;
    s_FsImageDesc desc;
    s16           ilmFile, timFile;
    int           need;
    s_LmHeader*   lm;

    s_p2ModelOk = 0;
    s_p2SlotTex = 0;
    if (!skin)
        Xbox_ResidentPoolRelease(P2_SKIN_SLOT);
    if (skin && !g_PcConfig.xboxPalettedTex)
    {
        SH_DBG("[SPLIT] texture_paletted=0: Player 2 cannot wear a skin, playing as Harry");
        return 0;
    }

    ilmFile = skin ? CHARA_FILE_INFOS[charaId].modelFileIdx : (s16)FILE_CHARA_HERO_ILM;
    timFile = skin ? CHARA_FILE_INFOS[charaId].textureFileIdx : (s16)FILE_CHARA_HERO_TIM;
    if (ilmFile == (s16)NO_VALUE || timFile == (s16)NO_VALUE)
        return 0;

    if (skin)
    {
        if (Xbox_ResidentPoolGet(P2_SKIN_SLOT, NULL) == NULL &&
            Xbox_ResidentPoolFreeCharaSlabs() < 1 && Xbox_ResidentPoolReserveExtra(1) < 1)
        {
            SH_DBG("[SPLIT] no texture slab for Player 2's skin: playing as Harry");
            return 0;
        }
        Sx_SlotDesc(&desc, P2_SKIN_SLOT);
    }
    else if (CHARA_FILE_INFOS[Chara_Harry].textureFileIdx == FILE_CHARA_HERO_TIM)
    {
        Chara_FsImageCalc(&desc, Chara_Harry, 0);
    }
    else if (!Pc_PlayAs_HeroTimDesc(&desc))
    {
        return 0;
    }

    need = Fs_GetFileSectorAlignedSize(ilmFile) + 16;
    if (s_p2Ilm == NULL || s_p2IlmCap < need)
    {
        free(s_p2Ilm);
        s_p2Ilm    = (u8*)calloc(1, (size_t)need);
        s_p2IlmCap = s_p2Ilm ? need : 0;
    }
    if (s_p2Ilm == NULL)
    {
        SH_DBG("[SPLIT] Player 2 model: calloc(%d) failed", need);
        return 0;
    }
    memset(s_p2Ilm, 0, (size_t)s_p2IlmCap);

    Fs_QueueStartRead(ilmFile, s_p2Ilm);
    if (skin)
        Fs_QueueStartReadTim(timFile, FS_BUFFER_1, &desc);
    Sx_PumpQueue();

    lm = (s_LmHeader*)s_p2Ilm;
    if (lm->magic != LM_HEADER_MAGIC)
    {
        SH_DBG("[SPLIT] Player 2 model: ILM %d did not load", (int)ilmFile);
        return 0;
    }
    if (skin && Xbox_ResidentPoolGet(P2_SKIN_SLOT, NULL) == NULL)
    {
        SH_DBG("[SPLIT] Player 2 model: skin texture not resident, playing as Harry");
        return 0;
    }

    memset(&s_p2Model, 0, sizeof(s_p2Model));
    s_p2Model.charaId  = Chara_Harry;
    s_p2Model.isLoaded = true;
    s_p2Model.lmHdr    = lm;
    s_p2Model.texture  = desc;
    LmHeader_FixOffsets(lm);
    Lm_MaterialFileIdxApply(lm, (e_FsFile)timFile, &s_p2Model.texture,
                            CHARA_FILE_INFOS[Chara_Harry].materialBlendMode % 4);
    Lm_MaterialFlagsApply(lm);
    Skeleton_Init(&s_p2Model.skeleton, s_p2Model.skeleton.bones_C, 56);
    func_8004506C(&s_p2Model.skeleton, lm);
    func_800452EC(&s_p2Model.skeleton);
    func_800453E8(&s_p2Model.skeleton, true);
    if (skin)
        Lm_TransparentPrimSet(lm, false);

    s_p2ModelOk = 1;
    s_p2SlotTex = skin;
    SH_DBG("[SPLIT] Player 2 is %s (model %d parts, texture %s)", Pc_PlayAs_Label(idx),
           (int)lm->modelCount, skin ? "slot 301" : "HERO");
    return 1;
}

/* Player 2 wears character idx, else Harry; returns the one worn, or -1 when
 * even Harry would not load (Player 2 then wears Player 1's model). */
static int Sx_P2Wear(int idx)
{
    if (Sx_P2ModelLoad(idx))
        return idx;
    if (Pc_PlayAs_CharaIdOf(idx) != Chara_Harry && Sx_P2ModelLoad(0))
        return 0;
    return -1;
}

/* ---- Player 2 joins / follows a load ---- */

/* Player 2's starting weapon: Player 1's, with an empty magazine for a gun (the
 * stock reload fills it from the shared reserve). Runs with Player 2 in the
 * slots; mirrors Game_PlayerInfoInit's equip and Pc_EquipWeapon's model swap. */
static void Sx_EquipInSlots(u8 id)
{
    const u32 grp = INV_ITEM_GROUP(id);
    s32       i   = (id == InvItemId_Unequipped) ? NO_VALUE : Sx_ItemSlot(id);

    func_8004C564(0, NO_VALUE);
    if (i == NO_VALUE || (grp != InvItemGroup_MeleeWeapons && grp != InvItemGroup_GunWeapons))
    {
        g_SavegamePtr->equippedWeapon             = InvItemId_Unequipped;
        g_Inventory_EquippedItem                  = InvItemId_Unequipped;
        g_SysWork.playerCombat.weaponAttack       = NO_VALUE;
        g_SysWork.playerCombat.currentWeaponAmmo  = 0;
        g_SysWork.playerCombat.totalWeaponAmmo    = 0;
        g_SysWork.playerCombat.weaponInventoryIdx = NO_VALUE;
    }
    else
    {
        g_SavegamePtr->equippedWeapon             = id;
        g_Inventory_EquippedItem                  = id;
        g_SysWork.playerCombat.weaponAttack       = (s8)(id + InvItemId_KitchenKnife);
        g_SysWork.playerCombat.weaponInventoryIdx = (s8)i;
        g_SysWork.playerCombat.currentWeaponAmmo  = 0;
        g_SysWork.playerCombat.totalWeaponAmmo    = 0;
        if (grp == InvItemGroup_GunWeapons)
        {
            s32 a = Sx_ItemSlot((u8)INV_WEAPON_AMMO_ID(id));

            g_SysWork.playerCombat.totalWeaponAmmo = (a == NO_VALUE) ? 0 : g_SavegamePtr->items[a].count_1;
            g_SavegamePtr->items[i].count_1        = 0;
        }
        else
        {
            g_SysWork.playerCombat.currentWeaponAmmo = g_SavegamePtr->items[i].count_1;
        }
    }
    g_Player_WeaponAttack = g_SysWork.playerCombat.weaponAttack;
    s_equipIn[s_owner]    = g_SavegamePtr->equippedWeapon;

    GameFs_WeaponInfoUpdate();
    Gfx_PlayerHeldItemAttach((u8)g_SysWork.playerCombat.weaponAttack);
    WorldGfx_PlayerPrevHeldItem(&g_SysWork.playerCombat);
    func_8003D01C();
}

/* Player 2 from nothing: a copy of Player 1 with its own identity, set up in
 * place (with Player 2 in the slots, so every bone's parent pointer is a slot
 * address and the exchange never needs to fix one up), then stood beside
 * Player 1. */
static void Sx_P2Join(void)
{
    const int       idx   = Split_P2CharIndex();
    s_SubCharacter* p1    = &g_SysWork.playerWork.player;
    VECTOR3         at    = p1->position;
    q3_12           rot   = p1->rotation.vy;
    const u8        p1Eq  = g_SavegamePtr->equippedWeapon;
    int             i;
    s_HeldItem*     held;

    /* Player 2's private buffers. */
    if (s_p2Plm == NULL)
        s_p2Plm = (u8*)calloc(1, PLM_CAP);
    if (s_anmImg == NULL)
        s_anmImg = (u8*)malloc(ANM_WINDOW);
    s_imgTag = -1;
    if (s_p2Plm == NULL || s_anmImg == NULL)
    {
        SH_DBG("[SPLIT] Player 2: out of memory for its buffers, split screen off");
        s_state = SS_OFF;
        return;
    }

    /* Body and rig. The rig is built for the character actually worn; with no
     * model of its own Player 2 wears Player 1's, so it takes Player 1's
     * character too (the per-frame part visibility follows the character). */
    if (s_p2ReqIdx != idx || !s_p2ModelOk)
    {
        s_p2ModelIdx = Sx_P2Wear(idx);
        s_p2ReqIdx   = idx;
    }
    if (!Pc_PlayAs_BuildCtx(s_p2ModelIdx >= 0 ? s_p2ModelIdx : Pc_PlayAs_Current(), &s_sidePlayAs, &s_sideRootY))
        SH_DBG("[SPLIT] Player 2's rig could not be read: Harry's proportions");

    /* The side block = a copy of Player 1... */
    for (i = 0; i < s_entCount; i++)
        memcpy(s_side + s_ent[i].off, s_ent[i].p, s_ent[i].n);
    s_sideSave.mapRoomIdx       = g_SavegamePtr->mapRoomIdx;
    s_sideSave.equippedWeapon   = InvItemId_Unequipped;
    s_sideSave.healthSaturation = 0;
    s_equipIn[0]                = p1Eq;
    s_equipIn[1]                = InvItemId_Unequipped;

    /* ...with Player 2's own identity. */
    held = SIDE(s_HeldItem, s_offHeld);
    memset(held, 0, sizeof(*held));
    held->itemId = NO_VALUE;
    held->lmHdr  = (s_LmHeader*)s_p2Plm;
    *SIDE(s_CharaModel*, s_offModelPtr) = s_p2ModelOk ? &s_p2Model : &g_WorldGfxWork.harryModel;
    *SIDE(s32, s_offLastWeapon)         = NO_VALUE;
    *SIDE(s_EventData*, s_offMapEvent)  = NULL;
    memset(s_side + s_offItemEv, 0, sizeof(s_EventData*) * 5);
    memset(s_side + s_offItemIds, 0, sizeof(g_ItemTriggerItemIds));
    *SIDE(u16, s_offLatch)              = 0;

    Sx_Exchange();
    {
        s_SubCharacter* p2 = &g_SysWork.playerWork.player;

        Player_XbSplitLocalsReset();
        Anim_BoneInit((s_AnmHeader*)FS_BUFFER_0, g_SysWork.playerBoneCoords);
        memset(g_SysWork.unkCoords_E30, 0, sizeof(g_SysWork.unkCoords_E30));
        func_8007E9C4();
        p2->health = Q12(100.0f);
        g_Player_LastWeaponSelected = NO_VALUE;
        g_SavegamePtr->healthSaturation = 0;

        Sx_StepBeside(&at, rot);
        {
            VECTOR3 pos = p2->position;
            Sx_StandAt(&pos, rot, 1);
        }
        Sx_EquipInSlots(p1Eq);
    }
    Sx_Exchange();

    s_sideInWorld = 1;
    s_sideParked  = 0;
    s_resp[0].phase = s_resp[1].phase = RS_ALIVE;
    SH_DBG("[SPLIT] Player 2 joined beside Player 1 (weapon %d, pad port %d)",
           (int)s_sideSave.equippedWeapon, Pad_XboxPlayerPort(1));
}

/* Player 2's character was changed in the options: wear the new one from here,
 * reloaded at a frame boundary (InGame entry) with Player 1 in the slots and
 * Player 2 in the side block. */
static void Sx_P2CharacterRefresh(void)
{
    const int idx = Split_P2CharIndex();

    if (s_owner != 0 || idx == s_p2ReqIdx || !s_sideInWorld)
        return;
    s_p2ModelIdx = Sx_P2Wear(idx);
    s_p2ReqIdx   = idx;
    Pc_PlayAs_BuildCtx(s_p2ModelIdx >= 0 ? s_p2ModelIdx : Pc_PlayAs_Current(), &s_sidePlayAs, &s_sideRootY);
    *SIDE(s_CharaModel*, s_offModelPtr) = s_p2ModelOk ? &s_p2Model : &g_WorldGfxWork.harryModel;
    /* The new rig seats its own extra bones: the anim tick sets them up again
     * once they are clear (Pc_PlayAs_PlayerAnimTick). */
    memset(s_side + s_offE30, 0, sizeof(g_SysWork.unkCoords_E30));
    SH_DBG("[SPLIT] Player 2 changed character: %s", Pc_PlayAs_Label(s_p2ModelIdx >= 0 ? s_p2ModelIdx : idx));
}

/* After a room or map load: the slot player came through the load; the side
 * player's map-scoped state is refreshed and it stands beside them. */
static void Sx_SideFollowLoad(void)
{
    s_SubCharacter* own = &g_SysWork.playerWork.player;
    VECTOR3         at  = own->position;
    q3_12           rot = own->rotation.vy;
    const int       idx = Split_P2CharIndex();

    (void)idx;
    Sx_P2CharacterRefresh();

    Sx_Exchange();
    g_MapEventData = NULL;
    memset(g_ItemTriggerEvents, 0, sizeof(s_EventData*) * 5);
    memset(g_ItemTriggerItemIds, 0, sizeof(g_ItemTriggerItemIds));
    g_SysWork.charaGroupFlags[3] = 0;
    s_resp[s_owner].phase        = RS_ALIVE;
    if (g_SysWork.playerWork.player.health <= Q12(0.0f))
        g_SysWork.playerWork.player.health = Q12(100.0f);
    Sx_StepBeside(&at, rot);
    {
        VECTOR3 pos = g_SysWork.playerWork.player.position;
        Sx_StandAt(&pos, rot, 1);
    }
    Sx_Exchange();

    s_sideInWorld = 1;
    s_sideParked  = 0;
    g_XbSplitStreamValid = 0;
}

/* ---- scenes ---- */

static int Sx_SceneLive(void)
{
    if ((g_SysWork.sysFlags & SysFlag_CutsceneActive) ||
        g_SysWork.cutsceneBorderState != CutsceneBorderState_None)
        return 1;
    if (g_GameWork.gameState == GameState_MapEvent)
        return 1;
    if ((g_SysWork.sysState == SysState_EventCallback || g_SysWork.sysState == SysState_Fmv) && Pc_ScriptOwnsShot())
        return 1;
    return 0;
}

/* A scene parks the side player (hidden, frozen, out of streaming and of the
 * monsters' reach) and the screen shows the owner alone; afterwards the side
 * player is back, and stands beside the owner if the scene took them apart. */
static void Sx_ParkUpdate(void)
{
    if (!s_sideInWorld)
        return;
    if (Sx_SceneLive())
    {
        if (!s_sideParked)
        {
            s_sideParked         = 1;
            g_XbSplitStreamValid = 0;
            SH_DBG("[SPLIT] scene: Player %d steps out", (s_owner ^ 1) + 1);
        }
        return;
    }
    if (s_sideParked && g_SysWork.sysState == SysState_Gameplay && g_GameWork.gameState == GameState_InGame)
    {
        s_SubCharacter* own  = &g_SysWork.playerWork.player;
        s_SubCharacter* side = Split_SideChara();
        q19_12          dx   = side->position.vx - own->position.vx;
        q19_12          dz   = side->position.vz - own->position.vz;

        s_sideParked = 0;
        if (Vc_VectorMagnitudeCalc(dx, Q12(0.0f), dz) > Q12(3.0f) ||
            s_sideSave.mapRoomIdx != g_SavegamePtr->mapRoomIdx)
        {
            Sx_SideFollowLoad();
        }
        SH_DBG("[SPLIT] scene over: Player %d back", (s_owner ^ 1) + 1);
    }
}

/* ---- owner ---- */

static void Sx_SetOwner(int p)
{
    if ((p & 1) == s_owner)
        return;
    Sx_Exchange();
    if (s_padsSwapped)
        Sx_PadsSwap();
    if (s_pubSwapPending)
        s_pubSwapPending = 0;
    else
        SplitRender_PubSwap();
}

/* ---- arming and teardown ---- */

static void Sx_Force(void)
{
    if (s_forced)
        return;
    s_forced                 = 1;
    s_savedStyle             = g_ControlStyle;
    s_savedControl2d         = g_PcConfig.control2d;
    s_savedMovementOriginal  = g_PcConfig.movementOriginal;
    s_savedDarkHarry         = g_PcConfig.darkHarry;
    Xbox_ApplyControlStyle(0);
    g_PcConfig.control2d        = 0;
    g_PcConfig.movementOriginal = 1;
    g_PcConfig.darkHarry        = 0;
}

static void Sx_Unforce(void)
{
    if (!s_forced)
        return;
    s_forced                    = 0;
    Xbox_ApplyControlStyle(s_savedStyle);
    g_PcConfig.control2d        = s_savedControl2d;
    g_PcConfig.movementOriginal = s_savedMovementOriginal;
    g_PcConfig.darkHarry        = s_savedDarkHarry;
}

static void Sx_Arm(void)
{
    Sx_TableInit();
    if (s_tableBad)
    {
        s_state = SS_OFF;
        return;
    }
    s_state        = SS_ARMED;
    s_owner        = 0;
    s_npcSwapped   = 0;
    s_sideInWorld  = 0;
    s_sideParked   = 0;
    s_placePending = 1;
    s_resp[0].phase = s_resp[1].phase = RS_ALIVE;
    Sx_Force();
    SH_DBG("[SPLIT] armed: layout %d, Player 2 %s, respawn %d s", g_PcConfig.splitLayout,
           Pc_PlayAs_Label(Split_P2CharIndex()), g_PcConfig.splitRespawnSec);
}

/* Everything back to one player. P1 back in the slots first: harryModel, the
 * play-as state and the root height must be Player 1's before anything draws
 * Harry again (the save screen does), and before Player 2's buffers go. */
static void Sx_Teardown(int keepSession)
{
    if (s_npcSwapped)
        Split_SwapNpcSet();
    if (s_owner != 0)
        Sx_ExchangeEx(1);
    if (s_padsSwapped)
        Sx_PadsSwap();
    memset(&g_GameWork.controllers[1], 0, sizeof(g_GameWork.controllers[1]));

    g_XbSplitActive      = 0;
    g_XbSplitTwoViews    = 0;
    g_XbSplitSidePass    = 0;
    g_XbSplitStreamValid = 0;
    s_sideInWorld        = 0;
    s_sideParked         = 0;
    s_placePending       = 0;
    s_pubSwapPending     = 0;
    s_resp[0].phase = s_resp[1].phase = RS_ALIVE;
    GpuXbox_ViewEnd();
    GpuNv2a_SetScissor(0, 0, 0, 0);
    SplitRender_Reset();
    SplitAudio_Reset();
    SplitNpc_Reset();

    Xbox_ResidentPoolRelease(P2_SKIN_SLOT);
    Xbox_ResidentPoolRelease(P2_WEAPON_SLOT);
    free(s_p2Ilm);
    free(s_p2Plm);
    free(s_anmImg);
    s_p2Ilm      = NULL;
    s_p2IlmCap   = 0;
    s_p2Plm      = NULL;
    s_anmImg     = NULL;
    s_imgTag     = -1;
    s_p2ModelOk  = 0;
    s_p2ReqIdx   = -1;
    s_p2ModelIdx = -1;
    s_p2SlotTex  = 0;
    s_weaponSlotOk = 0;
    Sx_Unforce();

    if (keepSession)
    {
        s_state = SS_MENU;
        SH_DBG("[SPLIT] game over: back to the Multiplayer menu");
    }
    else
    {
        Pad_XboxUnbindP2();
        GpuNv2a_SplitPool(0);
        s_state = SS_OFF;
        SH_DBG("[SPLIT] ended");
    }
}

/* ---- title (title.c) ---- */

int Split_TitleBusy(void)
{
    return s_state == SS_MENU || s_state == SS_BOOTING || SplitMenu_IsOpen();
}

void Split_TitleFrame(int canContinue, int canLoad)
{
    SplitMenu_SetAvailable(canContinue, canLoad);

    if (s_state >= SS_ARMED)
    {
        SH_DBG("[SPLIT] title reached with a game under way: ending split screen");
        Sx_Teardown(0);
    }
    if (SplitMenu_IsOpen())
    {
        if (s_state == SS_OFF)
            s_state = SS_MENU;
        return;
    }
    /* Back at the main menu with a session: a split start was backed out of
     * (difficulty select, load screen), or a split game over. */
    if ((s_state == SS_MENU || s_state == SS_BOOTING) && s_bootEntry < 0)
    {
        SplitMenu_OpenAt(s_resume || s_lastKind == SPLIT_START_CONTINUE);
        s_resume = 0;
        s_state  = SS_MENU;
    }
}

int Split_TitleTakeBootEntry(void)
{
    int e = s_bootEntry;

    s_bootEntry = -1;
    return e;
}

void Split_MenuStart(int kind, int p2Port)
{
    Pad_XboxBindP2(p2Port);
    GpuNv2a_SplitPool(1);
    s_bootEntry = (kind == SPLIT_START_CONTINUE) ? MainMenuEntry_Continue
                : (kind == SPLIT_START_LOAD)     ? MainMenuEntry_Load
                :                                  MainMenuEntry_Start;
    s_state     = SS_BOOTING;
    s_lastKind  = kind;
    SH_DBG("[SPLIT] starting %s: Player 2 on port %d, layout %d, Player 2 %s",
           kind == SPLIT_START_CONTINUE ? "Continue" : kind == SPLIT_START_LOAD ? "Load Game" : "New Game",
           p2Port, g_PcConfig.splitLayout, Pc_PlayAs_Label(Split_P2CharIndex()));
}

void Split_MenuClosed(void)
{
    if (s_state == SS_MENU || s_state == SS_BOOTING || s_state == SS_OFF)
    {
        Sx_Teardown(0);
        /* The title arms the attract demo on its entry frame only, and a session
         * up at that frame held it back; left like that, the demo boot would run
         * later without its flag, as a live game. Re-enter the title. */
        if (g_GameWork.gameState == GameState_MainMenu)
            g_GameWork.gameStateSteps[0] = 0;
    }
}

/* ---- game flow ---- */

void Split_OnBootStep0(int processFlags)
{
    if (processFlags & ProcessFlag_BootDemo)
    {
        if (s_state != SS_OFF)
        {
            SH_DBG("[SPLIT] attract demo boot: ending split screen");
            Sx_Teardown(0);
        }
        return;
    }

    switch (s_state)
    {
        case SS_BOOTING:
            if (processFlags & (ProcessFlag_NewGame | ProcessFlag_LoadSave | ProcessFlag_Continue))
                Sx_Arm();
            break;

        case SS_ACTIVE:
            if (processFlags & (ProcessFlag_NewGame | ProcessFlag_LoadSave | ProcessFlag_Continue))
            {
                /* A save loaded from inside the game: Player 2 starts over. The
                 * boot has already rebuilt the slot player from the save
                 * (GameBoot_PlayerInit), so with Player 2 in the slots an exchange
                 * would put Player 1's pre-load state back over it; only what the
                 * boot does not rebuild is handed back. */
                if (s_owner != 0)
                {
                    u8 y = ((u8*)FS_BUFFER_0)[0x13];

                    ((u8*)FS_BUFFER_0)[0x13] = s_sideRootY;
                    s_sideRootY              = y;
                    Pc_PlayAs_CtxExchange(&s_sidePlayAs);
                    g_WorldGfxWork.registeredCharaModels[Chara_Harry] = &g_WorldGfxWork.harryModel;
                    Player_XbSplitLocalsReset();
                    s_owner = 0;
                }
                if (s_padsSwapped)
                    Sx_PadsSwap();
                g_XbSplitActive   = 0;
                g_XbSplitTwoViews = 0;
                Sx_Arm();
            }
            else
            {
                s_placePending       = 1;
                s_sideParked         = 0;
                g_XbSplitStreamValid = 0;
            }
            break;

        default:
            break;
    }
}

int Split_ExteriorChunkSlots(int count, int interior)
{
    if ((s_state == SS_ARMED || s_state == SS_ACTIVE) && !interior && count < 8)
        return 8;
    return count;
}

void Split_StreamPointsInvalidate(void)
{
    g_XbSplitStreamValid = 0;
}

void Split_OnInGameEnter(void)
{
    if (s_state == SS_ARMED)
    {
        if (g_SysWork.sysFlags & SysFlag_DemoActive)
        {
            Sx_Teardown(0);
            return;
        }
        Sx_P2Join();
        if (s_state == SS_OFF)
        {
            Sx_Teardown(0);
            return;
        }
        s_state         = SS_ACTIVE;
        g_XbSplitActive = 1;
        s_placePending  = 0;
        return;
    }
    if (s_state == SS_ACTIVE && s_placePending)
    {
        s_placePending = 0;
        Sx_SideFollowLoad();
    }
    else if (s_state == SS_ACTIVE)
    {
        /* Back from a menu: the options may have changed Player 2's character. */
        Sx_P2CharacterRefresh();
    }
}

void Split_OnGameOverExit(void)
{
    if (s_state == SS_ACTIVE)
        s_resume = 1;
}

void Split_OnWarmBoot(void)
{
    if (s_state == SS_OFF)
        return;
    Sx_Teardown(s_resume);
}

/* ---- per frame ---- */

void Split_FrameInputBegin(void)
{
    if (s_padsSwapped)
        Sx_PadsSwap();
    if (s_state == SS_ACTIVE)
        Pad_XboxLatchP2();
}

void Split_FrameInputEnd(void)
{
    /* With Player 2's pad unplugged, Player 1's drives whatever Player 2 left
     * open (a pause, a menu, a message) rather than an idle report. */
    if (s_state == SS_ACTIVE && s_owner != 0 && !s_padsSwapped && Pad_XboxP2Connected())
    {
        Sx_PadsSwap();
        /* The d-pad-as-stick axes (sticks_24: the paper map pans with them) are
         * derived for controllers[0] only; Player 2's were built as pad 2. */
        ControllerData_AnalogToDigital(g_Controller0,
                                       (*(u16*)&g_Controller0->analogController.status & 0x5300) == 0x5300);
    }
}

void Split_FrameBegin(void)
{
    if (s_state != SS_ACTIVE)
    {
        g_XbSplitTwoViews = 0;
        return;
    }
    s_handBack = 0;
    if (s_pubSwapPending)
    {
        s_pubSwapPending = 0;
        SplitRender_PubSwap();
    }
    g_XbSplitTwoViews = Split_P2InWorld() &&
                        g_GameWork.gameState == GameState_InGame &&
                        g_SysWork.sysState != SysState_GameOver &&
                        !Sx_SceneLive();
    SplitRender_FrameStart();
    SplitAudio_FrameTick();
}

/* After the state dispatch: park around scenes, and hand the slots back to
 * Player 1 once a state Player 2 started is over. */
void Split_AfterDispatch(void)
{
    if (s_state != SS_ACTIVE)
        return;
    Sx_ParkUpdate();
    if (s_owner != 0 && g_GameWork.gameState == GameState_InGame &&
        g_SysWork.sysState == SysState_Gameplay && !Sx_SceneLive())
    {
        Sx_SetOwner(0);
        s_handBack = 1;
    }
}

/* The slot player's camera is final for the frame: it is view 1's. */
void Split_SlotCameraDone(void)
{
    if (s_state != SS_ACTIVE)
        return;
    s_viewOwner = s_owner;
    SplitRender_CapturePub(0);
    SplitAudio_ListenerCapture(s_owner);
}

/* Player 2's light for view 2: the flashlight's last stage again with the side
 * player's bones in the slots, captured, and the slot player's put back. The
 * blend state (field_2388) was advanced once, by the slot player's update. */
static void Sx_SideLightCapture(void)
{
    s_WorldEnvWork  saved     = g_WorldEnvWork;
    VECTOR3         shadow    = g_PcFlashlightShadowWorld;
    int             fpsMode   = g_PsyX_FlashlightFpsMode;
    s_StructUnk3*   fx        = &g_SysWork.field_2388.field_154;
    GsCOORDINATE2*  coord     = NULL;
    SVECTOR         rot;
    s32             intensity;

    intensity = Q12_MULT(func_8003F4DC(&coord, &rot, fx->effectsInfo_0.field_4,
                                       fx->effectsInfo_0.field_0.s_field_0.field_2,
                                       Vc_LensFlareTypeGet(), &g_SysWork),
                         g_SysWork.lightIntensity);
    func_800554C4(intensity, fx->flashlightLensFlareIntensity_2C, coord, g_SysWork.lightBoneCoord, &rot,
                  g_SysWork.lightPosition.vx, g_SysWork.lightPosition.vy, g_SysWork.lightPosition.vz,
                  g_WorldGfxWork.mapInfo ? g_WorldGfxWork.mapInfo->waterZones : NULL);
    func_80055814(fx->fogDistance_30);
    SplitRender_CaptureEnv2();

    g_WorldEnvWork            = saved;
    g_PcFlashlightShadowWorld = shadow;
    g_PsyX_FlashlightFpsMode  = fpsMode;
}

static void Sx_DrawSlotPlayer(int intoView2)
{
    s_SubCharacter* p = &g_SysWork.playerWork.player;

    if (intoView2)
    {
        if (!SplitRender_View2Begin())
            return;
    }
    Pc_Pick_CharaPreDraw(p, -1, g_SysWork.playerBoneCoords);
    func_8003DA9C(Chara_Harry, g_SysWork.playerBoneCoords, 1, p->timer_C6, 0);
    if (intoView2)
        SplitRender_View2End();
}

/* The other player's turn, after the slot player's update, draw and attacks
 * (game_sys_states.c), plus the slot player's body in view 2. Outside gameplay
 * (a message, a pickup) the other player only stands there: drawn, not run. */
void Split_SidePass(void)
{
    const int side     = s_owner ^ 1;
    int       promoted = 0;

    if (s_state != SS_ACTIVE)
        return;
    if (g_SysWork.sysState == SysState_Gameplay)
        Sx_RespawnTick(s_owner);

    if (Split_P2InWorld() && g_GameWork.gameState == GameState_InGame)
    {
        if (g_SysWork.sysState == SysState_Gameplay && !g_Player_DisableControl && !Sx_SceneLive() &&
            !(g_SysWork.sysFlags & SysFlag_DemoActive))
        {
            const q19_12    watchY = vcWork.watch_pos_y;
            const q20_12    walkD  = g_SavegamePtr->walkDistance;
            const q20_12    runD   = g_SavegamePtr->runDistance;
            s_SubCharacter* p;

            Sx_PadsSwap();
            Sx_Exchange();
            g_XbSplitSidePass = 1;
            p = &g_SysWork.playerWork.player;

            /* Camera 2 (the watch height is a script anchor the slot camera owns). */
            vcWork.watch_pos_y = watchY;
            vcMoveAndSetCamera(false, false, false, false, false, false, false, false);
            SplitRender_CapturePub(1);
            SplitAudio_ListenerCapture(side);
            Ipd_SplitStreamPointsGet(g_XbSplitStreamPos);
            g_XbSplitStreamValid = 1;

            /* Events, menus, pause: the stock gameplay state, for this player. Not
             * on the frame the slots came back from this player: the state it
             * closed ran its exit this frame with button events off, and the
             * click that closed it would open it again. */
            World_NearbyPlayerCollisionTriggersGet();
            if (!Sx_Holding(side) && p->health > Q12(0.0f) && !s_handBack)
                SysState_Gameplay_Update();

            Player_Update(p, (s_AnmHeader*)FS_BUFFER_0, g_SysWork.playerBoneCoords);
            g_SavegamePtr->walkDistance = walkD;
            g_SavegamePtr->runDistance  = runD;
            Sx_RespawnTick(side);
            Sx_SideLightCapture();
            SplitRender_PublishPub(0);

            if (p->model.anim.flags & AnimFlag_Visible)
            {
                int b;

                for (b = 0; b < HarryBone_Count; b++)
                    g_SysWork.playerBoneCoords[b].flg = 0;
                Sx_DrawSlotPlayer(0);
                Sx_DrawSlotPlayer(1);
                memcpy(s_sideBoneMirror, g_SysWork.playerBoneCoords, sizeof(s_sideBoneMirror));
                Chara_Flag8Clear(p);
                Player_CombatUpdate(p, s_sideBoneMirror);
                func_8008A3AC(p);
            }

            g_XbSplitSidePass = 0;
            /* Anything modal this player started plays out with them in the
             * slots. This frame's views stay as they were built; the cameras
             * change hands at the next frame. */
            promoted = g_SysWork.sysState != SysState_Gameplay;
            if (!promoted)
            {
                Sx_Exchange();
                Sx_PadsSwap();
                World_NearbyPlayerCollisionTriggersGet();
            }
            else
            {
                s_pubSwapPending = 1;
                SH_DBG("[SPLIT] Player %d took the game into state %d", side + 1, (int)g_SysWork.sysState);
            }
        }
        else if (g_XbSplitTwoViews)
        {
            /* Frozen this frame: the last pose, cached in its bones, in both views.
             * Its streaming points are refreshed under its own camera, since after
             * a hand-over they still describe the other player. */
            Sx_ExchangeEx(1);
            if (SplitRender_ViewPos(1) != NULL)
            {
                SplitRender_PublishPub(1);
                Ipd_SplitStreamPointsGet(g_XbSplitStreamPos);
                g_XbSplitStreamValid = 1;
                SplitRender_PublishPub(0);
            }
            if (g_SysWork.playerWork.player.model.anim.flags & AnimFlag_Visible)
            {
                Sx_DrawSlotPlayer(0);
                Sx_DrawSlotPlayer(1);
            }
            Sx_ExchangeEx(1);
        }
    }

    /* The slot player in view 2: its bones were drawn into view 1 by the stock
     * code this frame, so the cached world matrices are reused. */
    if (!promoted && g_XbSplitTwoViews && (g_SysWork.playerWork.player.model.anim.flags & AnimFlag_Visible))
        Sx_DrawSlotPlayer(1);
}

/* ---- pacing (game_main.c) ---- */

/* Two views take a frame of 22-52 ms. The stock wait (present, then N more
 * vblanks) turns any frame over one vblank into an extra one, so split screen
 * presents once per loop against a real-vblank target instead: 2 vblanks after
 * the last present when the frame made it (30fps), the next vblank when it did
 * not. A run of misses settles at 3 (a steady 20fps beats alternating), and two
 * seconds of short frames go back to 2. */
int Split_PacedVSync(int* vblanks, int* prevVblanks)
{
    static int s_lastMs, s_miss, s_window, s_good;
    int        now, cpuMs, got;

    if (!g_XbSplitTwoViews)
        return 0;

    now   = GpuNv2a_Ms();
    cpuMs = s_lastMs ? now - s_lastMs : 0;
    got   = VSync_Paced(s_paceN);
    s_lastMs = GpuNv2a_Ms();

    if (s_paceN == 2)
    {
        s_miss += (got > 2);
        if (++s_window >= 30)
        {
            if (s_miss >= 6)
            {
                s_paceN = 3;
                s_good  = 0;
                SH_DBG("[SPLIT] pacing 20fps (%d of 30 frames missed 30)", s_miss);
            }
            s_miss = s_window = 0;
        }
    }
    else
    {
        s_good = (cpuMs > 0 && cpuMs < 27) ? s_good + 1 : 0;
        if (s_good >= 40)
        {
            s_paceN = 2;
            s_miss  = s_window = 0;
            SH_DBG("[SPLIT] pacing 30fps");
        }
    }

    /* [SPLIT] frame summary every ~10 s of two-view play: the CPU time per frame
     * (update + both views + walks), what pacing made of it, and the slot owner. */
    {
        static int s_n, s_sum, s_max, s_vbl;

        if (cpuMs > 0)
        {
            s_n++;
            s_sum += cpuMs;
            s_vbl += got;
            if (cpuMs > s_max)
                s_max = cpuMs;
        }
        if (s_n >= 300)
        {
            SH_DBG("[SPLIT] %d frames: cpu avg %d ms max %d ms, %d fps, pacing %d, owner P%d, free %uKB",
                   s_n, s_sum / s_n, s_max, s_vbl ? (s_n * 60) / s_vbl : 0, s_paceN, s_owner + 1,
                   Xbox_MemFreeKB());
            s_n = s_sum = s_max = s_vbl = 0;
        }
    }

    *vblanks     = got;
    *prevVblanks = VSync(-1);
    return 1;
}

/* ---- rumble and held items (shared hooks) ---- */

/* bodyprog_80089090.c: a vibration effect requested while Player 2's state is
 * in the slots goes to Player 2's pad as a pulse (the effect engine itself is a
 * single self-referential instance and stays Player 1's). The low-health
 * heartbeat (12..17) is re-requested every frame and is left to Player 1. */
int Split_RumbleToP2(s32 idx, s32 strength)
{
    /* 0..4 are engine commands (suspend/resume/clear around loads and movies),
     * not effects. */
    if (!g_XbSplitActive || Sx_WorkOwner() != 1 || (idx >= 0 && idx <= 4))
        return 0;
    if (g_GameWork.config.vibrationEnabled && (idx < 12 || idx > 17))
        Pad_XboxP2Pulse((unsigned)(strength > 255 ? 255 : strength < 0 ? 0 : strength), 180);
    return 1;
}

/* world_draw.c: Player 2's own-texture weapons load into texture slot 302, so the
 * one VRAM rect those weapons share stays Player 1's. */
void Split_HeldItemImageDesc(s_FsImageDesc* image)
{
    if (s_state < SS_ARMED || s_owner != 1 || image == NULL)
        return;
    if (!s_weaponSlotOk)
    {
        s_weaponSlotOk = Xbox_ResidentPoolGet(P2_WEAPON_SLOT, NULL) != NULL ||
                         Xbox_ResidentPoolFreeCharaSlabs() > 0 || Xbox_ResidentPoolReserveExtra(1) > 0;
        if (!s_weaponSlotOk)
            return;
    }
    Sx_SlotDesc(image, P2_WEAPON_SLOT);
}
