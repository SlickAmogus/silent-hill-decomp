#include "game.h"

#ifdef SH_PC_PORT
#include "sh_log.h"
#include <stdio.h>
#endif

#include <psyq/libetc.h>
#include <psyq/libpad.h>
#include <psyq/strings.h>

#include "bodyprog/bodyprog.h"
#ifdef SH_PC_PORT
extern s_WorldEnvWork g_WorldEnvWork;
#endif
#include "bodyprog/events/bodyprog_data_800A99B4.h"
#include "bodyprog/events/events_main.h"
#include "bodyprog/events/npc_main.h"
#include "bodyprog/events/radio.h"
#include "bodyprog/demo.h"
#include "bodyprog/gfx/map_effects.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/math/math.h"
#include "bodyprog/memcard.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/text/text_draw.h"
#include "bodyprog/player.h"
#include "bodyprog/view/vw_main.h"
#include "bodyprog/ranking.h"
#include "bodyprog/sound_system.h"
#include "main/fsqueue.h"
#include "main/rng.h"
#ifdef SH_PC_PORT
#include <stdio.h>
extern void vcGetNowCamPos(VECTOR3* cam_pos);
extern void DebugCamera_Update(void);
#endif

#ifndef PAD_HACK_IGNORE
    s8  __pad_bss_800BCD81[3];
    s32 __pad_bss_800BCD88[2];
    s32 __pad_bss_800BCD94[5];
    s32 __pad_bss_800BCDD0;
    s8  __pad_bss_800BCDD5[3];
#endif

// ========================================
// STATIC VARIABLES
// ========================================

static void (*g_SysStateFuncs[])(void) = {
    SysState_Gameplay_Update,
    SysState_OptionsMenu_Update,
    SysState_StatusMenu_Update,
    SysState_MapScreen_Update,
    SysState_Fmv_Update,
    SysState_LoadArea_Update,
    SysState_LoadArea_Update,
    SysState_ReadMessage_Update,
    SysState_SaveMenu_Update,
    SysState_SaveMenu_Update,
    SysState_EventCallback_Update,
    SysState_EventSetFlag_Update,
    SysState_EventPlaySound_Update,
    SysState_GameOver_Update,
    SysState_GamePaused_Update
};

/** Used to store the previous delta time state of the delta timer. There are some instances where 2D backgrounds
 * are drawn using `g_DeltaTimeRaw` while `g_DeltaTime` is stopped.
 */
static s32 g_DeltaTimeCpy;

// ========================================
// GLOBAL VARIABLES
// ========================================

s_EventData* g_ItemTriggerEvents[];
s_800BCDA8   D_800BCDA8[2];
s_MapPoint2d D_800BCDB0;
s32          g_ItemTriggerItemIds[5];
u8           D_800BCDD4;
s_EventData* g_MapEventData;

#ifdef SH_PC_PORT
/* Backup of D_800BCDB0 — the original gets zeroed between SysState_LoadArea_Update
 * and AreaLoad_UpdatePlayerPosition (unknown cause, possibly BSS overlap or bzero).
 * We save it right after assignment and restore before use. */
static s_MapPoint2d s_PC_D_800BCDB0_Backup;
static int s_PC_D_800BCDB0_Saved = 0;
#endif

void GameState_InGame_Update(void) // 0x80038BD4
{
    s_SubCharacter* player;

    Demo_DemoRandSeedBackup();

    switch (g_GameWork.gameStateSteps[0])
    {
        case 0:
            ScreenFade_Start(true, true, false);
            g_ScreenFadeTimestep            = Q12(3.0f);
            g_GameWork.gameStateSteps[0] = 1;

        case 1:
            DrawSync(SyncMode_Wait);
            func_80037154();
            Savegame_MapRoomIdxUpdate();
            func_800892A4(1);

#ifdef SH_PC_PORT
            /* Snap Harry's Y to the actual ground height at his current position.
             * Chara_PositionSet (used by door triggers and spawn points) zeros Y,
             * and Game_PlayerHeightUpdate (called in GameBoot_InGameInit/NpcInit)
             * may run before Harry is at his final spawn position. This one-shot
             * call ensures Y and positionY_EC are in sync with collision data at
             * the moment InGame actually starts, preventing the first movement tick
             * from wrongly snapping Harry via a stale positionY_EC=0. */
            {
                s_Collision snapColl;
                s_SubCharacter* snapHp = &g_SysWork.playerWork.player;
                Collision_Get(&snapColl, snapHp->position.vx, snapHp->position.vz);
                if (snapColl.groundHeight_0 != Q12(8.0f)) {
                    snapHp->position.vy = snapColl.groundHeight_0;
                    snapHp->properties.player.positionY_EC = snapColl.groundHeight_0;
                    SH_DBG("[INIT] InGame Y snap: vy=%ld positionY_EC=%ld at (%ld,%ld)",
                           (long)snapColl.groundHeight_0, (long)snapColl.groundHeight_0,
                           (long)snapHp->position.vx, (long)snapHp->position.vz);
                } else {
                    SH_DBG("[INIT] InGame Y snap: no collision data at (%ld,%ld), keeping vy=%ld",
                           (long)snapHp->position.vx, (long)snapHp->position.vz, (long)snapHp->position.vy);
                }
            }
#endif

            g_IntervalVBlanks = 2;
            g_GameWork.gameStateSteps[0]++;
            g_SysWork.bgmStatusFlags |= BgmStatusFlag_6;
            break;
    }

    if (g_SysWork.sysState != SysState_Gameplay && g_SysWork.playerWork.player.health <= Q12(0.0f))
    {
        SysWork_StateSetNext(SysState_Gameplay);
    }

    if (g_DeltaTime != Q12(0.0f))
    {
        g_DeltaTimeCpy = g_DeltaTime;
    }
    else
    {
        g_DeltaTimeCpy = g_DeltaTimeRaw;
    }

    if (g_SysWork.sysState == SysState_Gameplay)
    {
        g_SysWork.isMgsStringSet = false;
        g_SysStateFuncs[SysState_Gameplay]();
    }
    else
    {
#ifdef SH_PC_PORT
        /* On PSX, events run at a different timing cadence where g_DeltaTime=0
         * during EventCallFunc is compensated by how often events fire.
         * On PC, this causes cutscene timers to never advance. Use the raw
         * delta time so timer-based cutscene steps can progress. */
        g_DeltaTime = g_DeltaTimeRaw;
#else
        g_DeltaTime = Q12(0.0f);
#endif
        g_SysStateFuncs[g_SysWork.sysState]();

        if (g_SysWork.sysState == SysState_Gameplay)
        {
            Event_Update(true);

            if (g_MapEventSysState != SysState_Invalid)
            {
                SysWork_StateSetNext(g_MapEventSysState);
            }
        }
    }
    Demo_DemoRandSeedRestore();

    D_800A9A0C = ScreenFade_IsFinished() && Fs_QueueChunksLoad();

    if (!(g_SysWork.bgmStatusFlags & BgmStatusFlag_Pause) && g_MapOverlayHeader.worldObjectsUpdate_40 != NULL)
    {
        g_MapOverlayHeader.worldObjectsUpdate_40();
    }

    Screen_CutsceneCameraStateUpdate();
    Bgm_TrackUpdate(false);
    Demo_DemoRandSeedRestore();
    Demo_DemoRandSeedRestore();

    if (!(g_SysWork.bgmStatusFlags & BgmStatusFlag_Pause))
    {
        func_80040014();
        vcMoveAndSetCamera(false, false, false, false, false, false, false, false);

#ifdef SH_PC_PORT
        /* Original camera system (vcMoveAndSetCamera above) uses camera road
         * data from the map overlay to position fixed-angle cameras like the
         * PSX game. The fallback third-person chase cam was a placeholder
         * before the road system worked. Now removed — original cameras active.
         * Debug camera (numpad *) still works independently. */
#endif

        if (g_MapOverlayHeader.func_44 != NULL)
        {
            g_MapOverlayHeader.func_44();
        }

        Demo_DemoRandSeedRestore();

#ifdef SH_PC_PORT
        /* Debug camera toggle (numpad *). Without this call the toggle
         * never fires and the free cam becomes unreachable. */
        {
            extern void DebugCamera_Update(void);
            DebugCamera_Update();
        }
#endif

        player = &g_SysWork.playerWork.player;
        Player_Update(player, FS_BUFFER_0, g_SysWork.playerBoneCoords);

        Demo_DemoRandSeedRestore();
        Gfx_FlashlightUpdate();

        if (g_SavegamePtr->mapOverlayId_A4 != MapIdx_MAP7_S03)
        {
            g_MapOverlayHeader.particlesUpdate_168(0, g_SavegamePtr->mapOverlayId_A4, 1);
        }

        Demo_DemoRandSeedRestore();

#ifdef SH_PC_PORT
        /* PC: AnimFlag_Visible gets cleared during/after cutscenes and never
         * restored, leaving Harry permanently invisible.  Force it back on each
         * InGame frame so the render block below always runs. */
        player->model.anim.flags |= AnimFlag_Visible;
#endif

        if (player->model.anim.flags & AnimFlag_Visible)
        {
#ifdef SH_PC_PORT
            /* Force all Harry skeleton bones visible. PSX implicitly leaves
             * them visible; PC sees stray AnimFlag clears that hide the
             * model entirely after a cutscene. */
            {
                s_CharaModel* harryModel = g_WorldGfxWork.registeredCharaModels[Chara_Harry];
                if (harryModel != NULL) {
                    func_800453E8(&harryModel->skeleton, true);
                }
            }
            /* Reset bone-coord flg values so the matrix hierarchy gets
             * fully recomputed this frame. Stale cached workm matrices
             * cause Harry's model to alternate-frame shrink/collapse. */
            {
                int _bi;
                for (_bi = 0; _bi < HarryBone_Count; _bi++) {
                    g_SysWork.playerBoneCoords[_bi].flg = 0;
                }
            }
            /* Temporarily disable fog for Harry's render -- the fogRamp_CC
             * lookup can produce out-of-range indices that corrupt his
             * vertex colors when fed through the full lighting pipeline. */
            {
                u8 savedFog = g_WorldEnvWork.isFogEnabled_1;
                u8 savedEnv = g_WorldEnvWork.field_0;
                g_WorldEnvWork.isFogEnabled_1 = 0;
                g_WorldEnvWork.field_0        = 0;
                func_8003DA9C(Chara_Harry, g_SysWork.playerBoneCoords, 1, g_SysWork.playerWork.player.timer_C6, 0);
                g_WorldEnvWork.isFogEnabled_1 = savedFog;
                g_WorldEnvWork.field_0        = savedEnv;
            }
#else
            func_8003DA9C(Chara_Harry, g_SysWork.playerBoneCoords, 1, g_SysWork.playerWork.player.timer_C6, 0);
#endif
            Chara_Flag8Clear(&g_SysWork.playerWork.player);
            Player_CombatUpdate(&g_SysWork.playerWork, g_SysWork.playerBoneCoords);
            func_8008A3AC(&g_SysWork.playerWork.player);
        }

        Demo_DemoRandSeedRestore();
#ifdef SH_PC_PORT
        SH_DBG("[FRAME] pre-NpcRoomInitSpawn");
        {
            extern void Pc_OtSentinelScan(GsOT* ot, const char* phase, const char* otName);
            extern s32 g_ActiveBufferIdx;
            extern GsOT g_OrderingTable0[2], g_OrderingTable2[2];
            Pc_OtSentinelScan(&g_OrderingTable0[g_ActiveBufferIdx], "pre-NpcRoomInitSpawn", "OT0");
            Pc_OtSentinelScan(&g_OrderingTable2[g_ActiveBufferIdx], "pre-NpcRoomInitSpawn", "OT2");
        }
#endif
        Game_NpcRoomInitSpawn(true);
#ifdef SH_PC_PORT
        SH_DBG("[FRAME] pre-NpcUpdate");
        {
            extern void Pc_OtSentinelScan(GsOT* ot, const char* phase, const char* otName);
            extern s32 g_ActiveBufferIdx;
            extern GsOT g_OrderingTable0[2], g_OrderingTable2[2];
            Pc_OtSentinelScan(&g_OrderingTable0[g_ActiveBufferIdx], "post-NpcRoomInitSpawn", "OT0");
        }
#endif
        Game_NpcUpdate();
#ifdef SH_PC_PORT
        SH_DBG("[FRAME] pre-5E89C");
        {
            extern void Pc_OtSentinelScan(GsOT* ot, const char* phase, const char* otName);
            extern s32 g_ActiveBufferIdx;
            extern GsOT g_OrderingTable0[2];
            Pc_OtSentinelScan(&g_OrderingTable0[g_ActiveBufferIdx], "post-NpcUpdate", "OT0");
        }
#endif
        func_8005E89C();
#ifdef SH_PC_PORT
        SH_DBG("[FRAME] pre-IpdCloseRange");
        {
            extern void Pc_OtSentinelScan(GsOT* ot, const char* phase, const char* otName);
            extern s32 g_ActiveBufferIdx;
            extern GsOT g_OrderingTable0[2];
            Pc_OtSentinelScan(&g_OrderingTable0[g_ActiveBufferIdx], "post-func_8005E89C-particles", "OT0");
        }
#endif
        Ipd_CloseRangeChunksInit();
#ifdef SH_PC_PORT
        SH_DBG("[FRAME] pre-InGameDraw");
        {
            extern void Pc_OtSentinelScan(GsOT* ot, const char* phase, const char* otName);
            extern s32 g_ActiveBufferIdx;
            extern GsOT g_OrderingTable0[2];
            Pc_OtSentinelScan(&g_OrderingTable0[g_ActiveBufferIdx], "post-IpdCloseRange", "OT0");
        }
#endif
        Gfx_InGameDraw(1);
#ifdef SH_PC_PORT
        SH_DBG("[FRAME] post-InGameDraw");
        {
            extern void Pc_OtSentinelScan(GsOT* ot, const char* phase, const char* otName);
            extern s32 g_ActiveBufferIdx;
            extern GsOT g_OrderingTable0[2];
            Pc_OtSentinelScan(&g_OrderingTable0[g_ActiveBufferIdx], "post-Gfx_InGameDraw", "OT0");
        }
#endif
        Demo_DemoRandSeedAdvance();
#ifdef SH_PC_PORT
    ingame_done:
        (void)0;
#endif
    }
}

void SysState_Gameplay_Update(void) // 0x80038BD4
{
    s_SubCharacter* player;

    player = &g_SysWork.playerWork.player;

    Event_Update(player->attackReceived != NO_VALUE);
    Savegame_MapRoomIdxUpdate();

    switch (FP_ROUND_SCALED(player->health, 10, Q12_SHIFT))
    {
        case 0:
            func_800892A4(17);
            break;

        case 1:
        case 2:
            func_800892A4(16);
            break;

        case 3:
            func_800892A4(15);
            break;

        case 4:
            func_800892A4(14);
            break;

        case 5:
            func_800892A4(13);
            break;

        case 6:
            func_800892A4(12);
            break;
    }

    if (g_SysWork.playerWork.player.health <= Q12(0.0f))
    {
        return;
    }

    if (g_Controller0->btnsClicked_10 & g_GameWorkPtr->config.controllerConfig.light &&
        g_SysWork.field_2388.field_154.effectsInfo_0.field_0.s_field_0.field_0 & (1 << 1))
    {
        Game_FlashlightToggle();
    }

    if (g_MapEventSysState != SysState_Invalid)
    {
        SysWork_StateSetNext(g_MapEventSysState);
    }
    else if (g_Controller0->btnsClicked_10 & g_GameWorkPtr->config.controllerConfig.pause)
    {
        SysWork_StateSetNext(SysState_GamePaused);
    }
    else if (Player_IsAttacking() == true)
    {
        return;
    }
    else if (g_Controller0->btnsClicked_10 & g_GameWorkPtr->config.controllerConfig.item)
    {
        SysWork_StateSetNext(SysState_StatusMenu);
    }
    else if (g_Controller0->btnsClicked_10 & g_GameWorkPtr->config.controllerConfig.map)
    {
        SysWork_StateSetNext(SysState_MapScreen);
        g_SysWork.isMgsStringSet = false;
    }
    else if (g_Controller0->btnsClicked_10 & g_GameWorkPtr->config.controllerConfig.option)
    {
        SysWork_StateSetNext(SysState_OptionsMenu);
    }

    if (g_SysWork.sysState == SysState_OptionsMenu ||
        g_SysWork.sysState == SysState_StatusMenu ||
        g_SysWork.sysState == SysState_MapScreen)
    {
        g_SysWork.flags_22A4 |= UnkSysFlag_MenuOpen;
    }
    else if (ScreenFade_IsNone())
    {
        g_SysWork.flags_22A4 &= ~UnkSysFlag_MenuOpen;
    }
}

void SysState_GamePaused_Update(void) // 0x800391E8
{
    static s32 D_800A9A68 = 0;

#ifdef SH_PC_PORT
    /* Make pause actually pause. `BgmStatusFlag_Pause` is misleadingly named:
     * it's the flag the InGame update flow uses to gate world-objects, camera,
     * player, NPC, particle, and character-render updates (see this file
     * lines ~190 and ~200). MainLoop resets bgmStatusFlags to None at the
     * top of every frame, so we have to re-set it each tick during pause.
     * Other gameplay sub-states that should pause the world (paper-map
     * screen, cutscene borders, various event states) already do this. */
    g_SysWork.bgmStatusFlags |= BgmStatusFlag_Pause;
#endif

    D_800A9A68 += g_DeltaTimeRaw;
    if (!((D_800A9A68 >> 11) & (1 << 0)))
    {
#if VERSION_REGION_IS(NTSCJ)
        Gfx_StringSetPosition(SCREEN_POSITION_X(41.0f), SCREEN_POSITION_Y(43.5f));
        Gfx_StringDraw("\x07PAUSE", DEFAULT_MAP_MESSAGE_LENGTH);
#else
        Gfx_StringSetPosition(SCREEN_POSITION_X(39.25f), SCREEN_POSITION_Y(43.5f));
        Gfx_StringDraw("\x07PAUSED", DEFAULT_MAP_MESSAGE_LENGTH);
#endif
    }

    func_80091380();
    Game_TimerUpdate();

    if (g_SysWork.sysStateSteps[0] == 0)
    {
        SD_Call(3);
        g_SysWork.sysStateSteps[0]++;
    }

    // Debug button combo to bring up save screen from pause screen.
    // DPad-Left + L2 + L1 + LS-Left + RS-Left + L3
    if ((g_Controller0->btnsHeld_C == (ControllerFlag_L3 |
                                       ControllerFlag_DpadLeft |
                                       ControllerFlag_L2 |
                                       ControllerFlag_L1 |
                                       ControllerFlag_LStickLeft2 |
                                       ControllerFlag_RStickLeft |
                                       ControllerFlag_LStickLeft)) &&
        (g_Controller0->btnsClicked_10 & ControllerFlag_L3))
    {
        D_800A9A68 = 0;
        SD_Call(4);
        g_MapEventParam = 0;
        SysWork_StateSetNext(SysState_SaveMenu1);
        return;
    }

    if (g_Controller0->btnsClicked_10 & g_GameWorkPtr->config.controllerConfig.pause)
    {
        D_800A9A68 = 0;

        SD_Call(4);
        SysWork_StateSetNext(SysState_Gameplay);
    }
}

void SysState_OptionsMenu_Update(void) // 0x80039344
{
    switch (g_SysWork.sysStateSteps[0])
    {
        case 0:
            ScreenFade_Start(true, false, false);
            g_ScreenFadeTimestep        = Q12(0.0f);
            g_SysWork.sysStateSteps[0] = 1;

        case 1:
            if (Ipd_ChunkInitCheck() != 0)
            {
                SD_Call(19);
                GameFs_OptionBinLoad();

                g_SysWork.sysStateSteps[0]++;
            }
            break;
    }

    if (D_800A9A0C != 0)
    {
        Game_StateSetNext(GameState_OptionScreen);
    }
}

void func_8003943C(void) // 0x8003943C
{
    s32 roundedVal0;
    s32 roundedVal1;
    s32 val0;
    s32 val1;

    #define isRockDrillAttack (g_SysWork.playerCombat.weaponAttack == WEAPON_ATTACK(EquippedWeaponId_RockDrill, AttackInputType_Tap))

    func_8008B3E4(0);

    if (g_SysWork.field_275C > Q12(256.0f))
    {
        val0        = g_SysWork.field_275C - Q12(256.0f);
        roundedVal0 = FP_ROUND_TO_ZERO(val0, Q12_SHIFT);
        func_8008B438(!isRockDrillAttack, roundedVal0, 0);

        if (isRockDrillAttack)
        {
            val1        = g_SysWork.field_2764 - Q12(256.0f);
            roundedVal1 = FP_ROUND_TO_ZERO(val1, Q12_SHIFT);
            func_8008B40C(roundedVal1, 0);
        }
    }
    else
    {
        func_8008B438(!isRockDrillAttack, 0, 0);

        if (isRockDrillAttack)
        {
            func_8008B40C(0, 0);
        }
    }

    switch (g_SavegamePtr->mapOverlayId_A4)
    {
        case MapIdx_MAP0_S01:
        case MapIdx_MAP0_S02:
        case MapIdx_MAP1_S00:
        case MapIdx_MAP1_S01:
        case MapIdx_MAP1_S02:
        case MapIdx_MAP1_S03:
        case MapIdx_MAP1_S04:
        case MapIdx_MAP1_S05:
        case MapIdx_MAP1_S06:
        case MapIdx_MAP2_S00:
        case MapIdx_MAP2_S01:
        case MapIdx_MAP2_S02:
        case MapIdx_MAP2_S03:
        case MapIdx_MAP2_S04:
        case MapIdx_MAP3_S00:
        case MapIdx_MAP3_S01:
        case MapIdx_MAP3_S02:
        case MapIdx_MAP3_S04:
        case MapIdx_MAP3_S05:
        case MapIdx_MAP3_S06:
        case MapIdx_MAP4_S00:
        case MapIdx_MAP4_S01:
        case MapIdx_MAP4_S02:
        case MapIdx_MAP4_S03:
        case MapIdx_MAP4_S04:
        case MapIdx_MAP4_S05:
        case MapIdx_MAP4_S06:
        case MapIdx_MAP5_S00:
        case MapIdx_MAP5_S01:
        case MapIdx_MAP5_S02:
        case MapIdx_MAP5_S03:
        case MapIdx_MAP6_S00:
        case MapIdx_MAP6_S01:
        case MapIdx_MAP6_S02:
        case MapIdx_MAP6_S03:
        case MapIdx_MAP6_S04:
        case MapIdx_MAP6_S05:
        case MapIdx_MAP7_S00:
        case MapIdx_MAP7_S01:
        case MapIdx_MAP7_S02:
            break;

        case MapIdx_MAP3_S03:
            Sd_SfxStop(Sfx_Unk1525);
            Sd_SfxStop(Sfx_Unk1527);
            break;

        case MapIdx_MAP0_S00:
            Sd_SfxStop(Sfx_Unk1358);
            break;
    }

    #undef isRockDrillAttack
}

void SysState_StatusMenu_Update(void) // 0x80039568
{
    e_GameState gameState;

    gameState = g_GameWork.gameState;

    g_GameWork.gameState = GameState_LoadStatusScreen;
    g_SysWork.counters_1C[0] = 0;
    g_SysWork.counters_1C[1] = 0;

    g_GameWork.gameStateSteps[1] = 0;
    g_GameWork.gameStateSteps[2] = 0;

    SysWork_StateSetNext(SysState_Gameplay);

    g_GameWork.gameStateSteps[0] = gameState;
    g_GameWork.gameStatePrev    = gameState;
    g_GameWork.gameStateSteps[0] = 0;
}

void GameState_LoadStatusScreen_Update(void) // 0x800395C0
{
    s_Savegame* save;

    if (g_GameWork.gameStateSteps[0] == 0)
    {
        DrawSync(SyncMode_Wait);
        g_IntervalVBlanks = 1;
        ScreenFade_Reset();

        func_8003943C();

        if (Sd_AudioStreamingCheck())
        {
            SD_Call(19);
        }

        save = g_SavegamePtr;
        func_800540A4(save->mapOverlayId_A4);
        GameFs_MapItemsTextureLoad(save->mapOverlayId_A4);

        g_GameWork.gameStateSteps[0]++;
    }

    Screen_BackgroundMotionBlur(SyncMode_Wait2);

    if (Fs_QueueChunksLoad())
    {
        Game_StateSetNext(GameState_InventoryScreen);
    }
}

void SysState_MapScreen_Update(void) // 0x800396D4
{
    if (!HAS_MAP(g_SavegamePtr->paperMapIdx_A9))
    {
        if (g_Controller0->btnsClicked_10 & g_GameWorkPtr->config.controllerConfig.map ||
            Gfx_MapMsg_Draw(MapMsgIdx_NoMap) > MapMsgState_Idle)
        {
            SysWork_StateSetNext(SysState_Gameplay);
        }
    }
    else if ((g_SysWork.field_2388.field_154.effectsInfo_0.field_0.s_field_0.field_0 & (1 << 1)) && !g_SysWork.field_2388.isFlashlightOn_15 &&
             ((g_SysWork.field_2388.field_1C[0].effectsInfo_0.field_0.s_field_0.field_0 & (1 << 0)) ||
              (g_SysWork.field_2388.field_1C[1].effectsInfo_0.field_0.s_field_0.field_0 & (1 << 0))))
    {
        if (g_Controller0->btnsClicked_10 & g_GameWorkPtr->config.controllerConfig.map ||
            Gfx_MapMsg_Draw(MapMsgIdx_TooDarkForMap) > MapMsgState_Idle)
        {
            SysWork_StateSetNext(SysState_Gameplay);
        }
    }
    else
    {
        if (g_SysWork.sysStateSteps[0] == 0)
        {
            if (g_PaperMapMarkingFileIdxs[g_SavegamePtr->paperMapIdx_A9] != NO_VALUE)
            {
                Fs_QueueStartReadTim(FILE_TIM_MR_0TOWN_TIM + g_PaperMapMarkingFileIdxs[g_SavegamePtr->paperMapIdx_A9], FS_BUFFER_1, &g_PaperMapMarkingAtlasImg);
            }

            Fs_QueueStartSeek(FILE_TIM_MP_0TOWN_TIM + g_PaperMapFileIdxs[g_SavegamePtr->paperMapIdx_A9]);

            ScreenFade_Start(true, false, false);
            g_ScreenFadeTimestep = Q12(0.0f);
            g_SysWork.sysStateSteps[0]++;
        }

        if (D_800A9A0C != 0)
        {
            Game_StateSetNext(GameState_MapScreen);
        }
    }
}

void GameState_LoadMapScreen_Update(void) // 0x8003991C
{
    if (g_GameWork.gameStateSteps[0] == 0)
    {
        DrawSync(SyncMode_Wait);
        g_IntervalVBlanks = 1;

        func_8003943C();
        func_80066E40();

        if (g_PaperMapMarkingFileIdxs[g_SavegamePtr->paperMapIdx_A9] != NO_VALUE)
        {
            Fs_QueueStartReadTim(FILE_TIM_MR_0TOWN_TIM + g_PaperMapMarkingFileIdxs[g_SavegamePtr->paperMapIdx_A9], FS_BUFFER_1, &g_PaperMapMarkingAtlasImg);
        }

        Fs_QueueStartReadTim(FILE_TIM_MP_0TOWN_TIM + g_PaperMapFileIdxs[g_SavegamePtr->paperMapIdx_A9], FS_BUFFER_2, &g_PaperMapImg);
        g_GameWork.gameStateSteps[0]++;
    }

    Screen_BackgroundMotionBlur(SyncMode_Wait2);

    if (Fs_QueueChunksLoad())
    {
        Game_StateSetNext(GameState_MapScreen);
    }
}

void SysState_Fmv_Update(void) // 0x80039A58
{
    #define BASE_AUDIO_FILE_IDX FILE_XA_ZC_14392

    static RECT D_800A9A6C = { 320, 256, 160, 240 };

    switch (g_SysWork.sysStateSteps[0])
    {
        case 0:
            ScreenFade_Start(false, false, false);
            D_800A9A0C                  = 0;
            g_SysWork.sysStateSteps[0] = 1;

        case 1:
            if (Ipd_ChunkInitCheck() != 0)
            {
                GameFs_StreamBinLoad();
                g_SysWork.sysStateSteps[0]++;
            }
            break;
    }

    if (D_800A9A0C == 0)
    {
        return;
    }

    // Copy framebuffer into `IMAGE_BUFFER_0` before movie playback.
    DrawSync(SyncMode_Wait);
    StoreImage(&D_800A9A6C, (u32*)IMAGE_BUFFER_0);
    DrawSync(SyncMode_Wait);

    func_800892A4(0);
    func_80089128();

    // Start playing movie. File to play is based on file ID `BASE_AUDIO_FILE_IDX - g_MapEventParam`.
    // Blocks until movie has finished playback or user has skipped it.
    open_main(BASE_AUDIO_FILE_IDX - g_MapEventParam, g_FileTable[BASE_AUDIO_FILE_IDX - g_MapEventParam].blockCount);

    func_800892A4(1);

    // Restore copied framebuffer from `IMAGE_BUFFER_0`.
    GsSwapDispBuff();
    LoadImage(&D_800A9A6C, (u32*)IMAGE_BUFFER_0);
    DrawSync(SyncMode_Wait);

    // Set savegame flag based on `g_MapEventData->disabledEventFlag` flag ID.
    Savegame_EventFlagSetAlt(g_MapEventData->disabledEventFlag);

    // Return to game.
    Game_StateSetNext(GameState_InGame);

    // If flag is set, returns to `GameState_InGame` with `gameStateSteps[0]` = 1.
    if (g_MapEventData->flags_8_13 & EventParamUnkState_1)
    {
        g_GameWork.gameStateSteps[0] = 1;
    }
}

void SysState_LoadArea_Update(void) // 0x80039C40
{
    u32           offsetZ;
    s_MapPoint2d* mapPoint;

#ifdef SH_PC_PORT
    /* Crash dump from 2026-05-01 23:49 had FAILURE_BUCKET_ID
     * INVALID_POINTER_READ at SysState_LoadArea_Update+0x3fe with no
     * preceding [DOOR] log entry — meaning the crash is somewhere
     * before line 779's SH_DBG fires. The function dereferences
     * g_MapEventData and g_MapOverlayHeader.mapPointsOfInterest_1C
     * heavily, so guard them first and log enough state to localize. */
    SH_DBG("[DOOR-ENTRY] SysState_LoadArea_Update: g_MapEventData=%p mapPointsOfInterest=%p sysState=%d",
           (void*)g_MapEventData, (void*)g_MapOverlayHeader.mapPointsOfInterest_1C,
           (int)g_SysWork.sysState);
    fflush(g_ShDebugLog);  /* flush NOW so the trace survives the crash */
    if (g_MapEventData == NULL) {
        SH_DBG("[DOOR-ENTRY] g_MapEventData is NULL — bailing");
        fflush(g_ShDebugLog);
        return;
    }
    if (g_MapOverlayHeader.mapPointsOfInterest_1C == NULL) {
        SH_DBG("[DOOR-ENTRY] mapPointsOfInterest_1C is NULL — bailing");
        fflush(g_ShDebugLog);
        return;
    }
    SH_DBG("[DOOR-ENTRY] g_MapEventData fields: sfxPairIdx_8_19=%d flags_8_13=0x%X eventParam=%d pointOfInterestIdx=%d mapIdx=%d",
           g_MapEventData->sfxPairIdx_8_19, g_MapEventData->flags_8_13,
           g_MapEventData->eventParam, g_MapEventData->pointOfInterestIdx,
           g_MapEventData->mapIdx);
    fflush(g_ShDebugLog);
#endif

    g_SysWork.field_229C            = 0;
    g_SysWork.loadingScreenIdx = D_800BCDB0.loadingScreenId_4_9;
    g_SysWork.sfxPairIdx_2283       = g_MapEventData->sfxPairIdx_8_19;
    g_SysWork.field_2282            = g_MapEventData->flags_8_13;

    SD_Call(SFX_PAIRS[g_SysWork.sfxPairIdx_2283].sfx_0);

    if (g_SysWork.sfxPairIdx_2283 == SfxPairIdx_7)
    {
        D_800BCDD4            = 0;
        g_SysWork.flags_22A4 |= UnkSysFlag_10;
    }

    D_800BCDB0 = g_MapOverlayHeader.mapPointsOfInterest_1C[g_MapEventData->eventParam];

#ifdef SH_PC_PORT
    SH_DBG("[DOOR] SysState_LoadArea: eventParam=%d pointOfInterestIdx=%d sysState=%d",
           g_MapEventData->eventParam, g_MapEventData->pointOfInterestIdx, g_SysWork.sysState);
    SH_DBG("[DOOR]   D_800BCDB0: posX=%d posZ=%d triggerParam0=%d triggerParam1=%d",
           D_800BCDB0.positionX_0, D_800BCDB0.positionZ_8,
           D_800BCDB0.triggerParam0_4_16, D_800BCDB0.triggerParam1_4_24);
    SH_DBG("[DOOR]   mapPointsOfInterest=%p playerPos=(%d,%d)",
           (void*)g_MapOverlayHeader.mapPointsOfInterest_1C,
           g_SysWork.playerWork.player.position.vx,
           g_SysWork.playerWork.player.position.vz);
#endif

    if (D_800BCDB0.triggerParam1_4_24 == 1)
    {
        mapPoint                = &g_MapOverlayHeader.mapPointsOfInterest_1C[g_MapEventData->pointOfInterestIdx];
        offsetZ                 = g_SysWork.playerWork.player.position.vz - mapPoint->positionZ_8;
        D_800BCDB0.positionX_0 += g_SysWork.playerWork.player.position.vx - mapPoint->positionX_0;
        D_800BCDB0.positionZ_8 += offsetZ;
    }

#ifdef SH_PC_PORT
    /* D_800BCDB0 gets zeroed somewhere between here and AreaLoad_Update-
     * PlayerPosition (PSX path runs synchronously, PC's GameBoot_MapLoad
     * trips through extra subsystems that clear it). Save a backup here
     * and restore it in AreaLoad_UpdatePlayerPosition if it's been
     * zeroed -- otherwise the player spawns at (0,0,0) on every door. */
    s_PC_D_800BCDB0_Backup = D_800BCDB0;
    s_PC_D_800BCDB0_Saved  = 1;
#endif

#ifdef SH_PC_PORT
    /* Snapshot scalar fields from g_MapEventData BEFORE GameBoot_MapLoad
     * runs. Reason: GameBoot_MapLoad unloads the current map overlay,
     * which deallocates the s_EventData struct that g_MapEventData
     * points into (it lives in g_MapOverlayHeader.mapEvents_18 and the
     * old map's overlay gets unloaded by MapOverlay_Unload). Using
     * g_MapEventData after the map load is a use-after-free; on PC it
     * crashed with INVALID_POINTER_READ at the disabledEventFlag access
     * (movzx eax, word ptr [rax+2]). Crash hash db439cd4 in dump
     * 2026-05-02 — same scenario kept reproducing because the lifetime
     * mismatch is in the source flow, not in any of our PC shims. */
    s16 _eventData_disabledEventFlag = g_MapEventData->disabledEventFlag;
    u32 _eventData_field_8_24        = g_MapEventData->field_8_24;
    u32 _eventData_mapIdx            = g_MapEventData->mapIdx;
    s32 _eventData_eventParam        = g_MapEventData->eventParam;
    SH_DBG("[DOOR-ENTRY] snapshotted: disabledFlag=%d mapIdx=%u eventParam=%d field_8_24=%u",
           _eventData_disabledEventFlag, _eventData_mapIdx,
           _eventData_eventParam, _eventData_field_8_24);
    fflush(g_ShDebugLog);
#endif

    if (g_SysWork.sysState == SysState_LoadOverlay)
    {
        g_SysWork.processFlags    = ProcessFlag_OverlayTransition;
#ifdef SH_PC_PORT
        SH_DBG("[DOOR-ENTRY] LoadOverlay branch: pre savegame write, g_SavegamePtr=%p",
               (void*)g_SavegamePtr);
        fflush(g_ShDebugLog);
        g_SavegamePtr->mapOverlayId_A4 = _eventData_mapIdx;
        SH_DBG("[DOOR-ENTRY] LoadOverlay branch: pre GameBoot_MapLoad mapIdx=%u",
               (unsigned)_eventData_mapIdx);
        fflush(g_ShDebugLog);
#else
        g_SavegamePtr->mapOverlayId_A4 = g_MapEventData->mapIdx;
#endif
        GameBoot_MapLoad(g_SavegamePtr->mapOverlayId_A4);
#ifdef SH_PC_PORT
        SH_DBG("[DOOR-ENTRY] LoadOverlay branch: post GameBoot_MapLoad");
        fflush(g_ShDebugLog);
#endif
    }
    else
    {
        g_SysWork.processFlags = ProcessFlag_RoomTransition;
#ifdef SH_PC_PORT
        SH_DBG("[DOOR-ENTRY] RoomTransition branch: pre Bgm_TrackChange mapIdx=%u",
               (unsigned)_eventData_mapIdx);
        fflush(g_ShDebugLog);
        Bgm_TrackChange(_eventData_mapIdx);
        if (g_MapOverlayHeader.mapPointsOfInterest_1C[_eventData_eventParam].field_4_5 != 0)
        {
            g_SysWork.field_2349 = g_MapOverlayHeader.mapPointsOfInterest_1C[_eventData_eventParam].field_4_5 - 1;
        }
#else
        Bgm_TrackChange(g_MapEventData->mapIdx);
        if (g_MapOverlayHeader.mapPointsOfInterest_1C[g_MapEventData->eventParam].field_4_5 != 0)
        {
            g_SysWork.field_2349 = g_MapOverlayHeader.mapPointsOfInterest_1C[g_MapEventData->eventParam].field_4_5 - 1;
        }
#endif
    }

#ifdef SH_PC_PORT
    Savegame_EventFlagSetAlt(_eventData_disabledEventFlag);

    if (_eventData_field_8_24)
    {
        g_SysWork.flags_22A4 |= UnkSysFlag_6;
    }
    else
    {
        g_SysWork.flags_22A4 &= ~UnkSysFlag_6;
    }
#else
    Savegame_EventFlagSetAlt(g_MapEventData->disabledEventFlag);

    if (g_MapEventData->field_8_24)
    {
        g_SysWork.flags_22A4 |= UnkSysFlag_6;
    }
    else
    {
        g_SysWork.flags_22A4 &= ~UnkSysFlag_6;
    }
#endif

    g_SysWork.bgmStatusFlags |= BgmStatusFlag_Pause;
    Game_StateSetNext(GameState_MainLoadScreen);
    Screen_BackgroundMotionBlur(SyncMode_Immediate);
}

void AreaLoad_UpdatePlayerPosition(void) // 0x80039F30
{
#ifdef SH_PC_PORT
    SH_DBG("[TRANSITION] AreaLoad_UpdatePlayerPosition: BEFORE playerPos=(%d,%d,%d) targetPos=(%d,%d) loadScreen=%d",
           g_SysWork.playerWork.player.position.vx,
           g_SysWork.playerWork.player.position.vy,
           g_SysWork.playerWork.player.position.vz,
           D_800BCDB0.positionX_0, D_800BCDB0.positionZ_8,
           D_800BCDB0.loadingScreenId_4_9);
    /* Restore backup if D_800BCDB0 was zeroed */
    if (s_PC_D_800BCDB0_Saved && D_800BCDB0.positionX_0 == 0 && D_800BCDB0.positionZ_8 == 0 &&
        (s_PC_D_800BCDB0_Backup.positionX_0 != 0 || s_PC_D_800BCDB0_Backup.positionZ_8 != 0))
    {
        SH_DBG("[TRANSITION] D_800BCDB0 was ZEROED! Restoring backup: posX=%d posZ=%d tp0=%d tp1=%d",
               s_PC_D_800BCDB0_Backup.positionX_0, s_PC_D_800BCDB0_Backup.positionZ_8,
               s_PC_D_800BCDB0_Backup.triggerParam0_4_16, s_PC_D_800BCDB0_Backup.triggerParam1_4_24);
        D_800BCDB0 = s_PC_D_800BCDB0_Backup;
    }
    s_PC_D_800BCDB0_Saved = 0;
#endif
    Chara_PositionSet(&D_800BCDB0);
#ifdef SH_PC_PORT
    SH_DBG("[TRANSITION] AreaLoad_UpdatePlayerPosition: AFTER playerPos=(%d,%d,%d)",
           g_SysWork.playerWork.player.position.vx,
           g_SysWork.playerWork.player.position.vy,
           g_SysWork.playerWork.player.position.vz);
#endif
}

void AreaLoad_TransitionSound(void) // 0x80039F54
{
    SD_Call(SFX_PAIRS[g_SysWork.sfxPairIdx_2283].sfx_2);
}

s8 func_80039F90(void) // 0x80039F90
{
    if (g_SysWork.processFlags & (ProcessFlag_RoomTransition | ProcessFlag_OverlayTransition))
    {
        return g_SysWork.field_2282;
    }

    return 0;
}

void SysState_ReadMessage_Update(void) // 0x80039FB8
{
    s32 i;
    void (**unfreezePlayerFunc)(bool);

    // When `SysState_ReadMessage_Update` is called, the game world freezes.
    // The following conditions unfreeze:
    // - A specific event related flag is disenabled.
    // - A specific camera related flag is disenabled.
    // - There is no alive enemy.
    if (!(g_MapEventData->flags_8_13 & EventParamUnkState_0) && !(g_SysWork.flags_22A4 & UnkSysFlag_5))
    {
        for (i = 0; i < ARRAY_SIZE(g_SysWork.npcs); i++)
        {
            if (g_SysWork.npcs[i].model.charaId >= Chara_Harry && g_SysWork.npcs[i].model.charaId <= Chara_MonsterCybil &&
                g_SysWork.npcs[i].health > Q12(0.0f))
            {
                break;
            }
        }

        if (i == ARRAY_SIZE(g_SysWork.npcs))
        {
            g_DeltaTime = g_DeltaTimeCpy;
        }
    }
    else
    {
        g_DeltaTime = g_DeltaTimeCpy;
    }

    if (g_SysWork.isMgsStringSet == false)
    {
        g_MapOverlayHeader.playerControlFreeze_C8();
    }

    switch (Gfx_MapMsg_Draw(g_MapEventParam))
    {
        case MapMsgState_Finish:
            break;

        case MapMsgState_Idle:
            break;

        case MapMsgState_SelectEntry0:
            Savegame_EventFlagSetAlt(g_MapEventData->disabledEventFlag);

            unfreezePlayerFunc = &g_MapOverlayHeader.playerControlUnfreeze_CC;

            SysWork_StateSetNext(SysState_Gameplay);

            (*unfreezePlayerFunc)(false);
            break;
    }
}

void SysWork_SavegameUpdatePlayer(void) // 0x8003A120
{
    s_Savegame* save;

    save = g_SavegamePtr;

    save->locationId_A8       = g_MapEventParam;
    save->playerPositionX_244 = g_SysWork.playerWork.player.position.vx;
    save->playerPositionZ_24C = g_SysWork.playerWork.player.position.vz;
    save->playerRotationY_248 = g_SysWork.playerWork.player.rotation.vy;
    save->playerHealth_240    = g_SysWork.playerWork.player.health;
}

void func_8003A16C(void) // 0x8003A16C
{
    if (!(g_SysWork.flags_22A4 & UnkSysFlag_1))
    {
        // Update `savegame` with player info.
        SysWork_SavegameUpdatePlayer();

        g_GameWork.autosave = g_GameWork.savegame;
    }
}

void SysWork_SavegameReadPlayer(void) // 0x8003A1F4
{
    g_SysWork.playerWork.player.position.vx = g_SavegamePtr->playerPositionX_244;
    g_SysWork.playerWork.player.position.vz = g_SavegamePtr->playerPositionZ_24C;
    g_SysWork.playerWork.player.rotation.vy = g_SavegamePtr->playerRotationY_248;
    g_SysWork.playerWork.player.health      = g_SavegamePtr->playerHealth_240;
}

void SysState_SaveMenu_Update(void) // 0x8003A230
{
    s32 gameState;

    func_80033548();

    switch (g_SysWork.sysStateSteps[0])
    {
        case 0:
            SysWork_SavegameUpdatePlayer();

            if (Savegame_EventFlagGet(EventFlag_SeenSaveScreen) ||
                g_SavegamePtr->locationId_A8 == SaveLocationId_NextFear || g_MapEventParam == 0)
            {
                GameFs_SaveLoadBinLoad();

                ScreenFade_Start(true, false, false);
                SysWork_StateStepIncrement(0);
            }
            else if (Gfx_MapMsg_Draw(MapMsgIdx_SaveGame) == MapMsgState_SelectEntry0)
            {
                Savegame_EventFlagSet(EventFlag_SeenSaveScreen);

                GameFs_SaveLoadBinLoad();

                ScreenFade_Start(true, false, false);
                SysWork_StateStepIncrement(0);
            }
            break;

        case 1:
            if (D_800A9A0C != 0)
            {
                ScreenFade_Start(true, true, false);

                func_8003943C();

                gameState = g_GameWork.gameState;

                g_GameWork.gameState = GameState_SaveScreen;

                g_SysWork.counters_1C[0] = 0;
                g_SysWork.counters_1C[1] = 0;

                g_GameWork.gameStateSteps[1] = 0;
                g_GameWork.gameStateSteps[2] = 0;

                SysWork_StateSetNext(SysState_Gameplay);

                g_GameWork.gameStateSteps[0] = gameState;
                g_GameWork.gameStatePrev    = gameState;
                g_GameWork.gameStateSteps[0] = 0;
            }
            break;
    }
}

void SysState_EventCallback_Update(void) // 0x8003A3C8
{
#ifdef SH_PC_PORT
    if (g_MapEventData == NULL) {
        g_SysWork.sysState = SysState_Gameplay;
        return;
    }
#endif
    if (g_MapEventData->flags_8_13 != EventParamUnkState_None)
    {
        Savegame_EventFlagSetAlt(g_MapEventData->disabledEventFlag);
    }

    g_DeltaTime = g_DeltaTimeCpy;
#ifdef SH_PC_PORT
    /* Guard OOB: mapEventFuncs_20 arrays vary per map (e.g. map0_s02 has 7).
     * A stale lastUsedItem can produce a garbage param well past the end. */
    if (g_MapEventParam < 0 || g_MapEventParam >= 64) {
        SH_DBG("[SS] EventCallFunc param=%d OOB — skip", g_MapEventParam);
        g_SysWork.sysState = SysState_Gameplay;
        return;
    }
    SH_DBG("[SS] EventCallFunc param=%d func=%p step0=%d step1=%d step2=%d", g_MapEventParam,
            (void*)g_MapOverlayHeader.mapEventFuncs_20[g_MapEventParam],
            (int)g_SysWork.sysStateSteps[0], (int)g_SysWork.sysStateSteps[1], (int)g_SysWork.sysStateSteps[2]);
    if (g_MapOverlayHeader.mapEventFuncs_20[g_MapEventParam] == NULL) {
        SH_DBG("[SS] EventCallFunc NULL — skip");
        g_SysWork.sysState = SysState_Gameplay;
        return;
    }
#endif
    g_MapOverlayHeader.mapEventFuncs_20[g_MapEventParam]();
}

void SysState_EventSetFlag_Update(void) // 0x8003A460
{
    g_DeltaTime = g_DeltaTimeCpy;
    Savegame_EventFlagSetAlt(g_MapEventData->disabledEventFlag);
    g_SysWork.sysState = SysState_Gameplay;
}

void SysState_EventPlaySound_Update(void) // 0x8003A4B4
{
    g_DeltaTime = g_DeltaTimeCpy;

    SD_Call(((u16)g_MapEventParam + Sfx_Base) & 0xFFFF);

    Savegame_EventFlagSetAlt(g_MapEventData->disabledEventFlag);
    g_SysWork.sysState = SysState_Gameplay;
}

void SysState_GameOver_Update(void) // 0x8003A52C
{
    #define TIP_COUNT 15

    static u8 prevTipIdx;
    u16       seenTipIdxs[1];
    s32       tipIdx;
    s32       randTipVal;
    u16*      temp_a0;

    switch (g_SysWork.sysStateSteps[0])
    {
        case 0:
            g_MapOverlayHeader.playerControlFreeze_C8();
            g_SysWork.field_28 = Q12(0.0f);

            if (g_GameWork.autosave.continueCount_27B < 99)
            {
                g_GameWork.autosave.continueCount_27B++;
            }

            MainMenu_SelectedOptionIdxReset();

            // If every game over tip has been seen, reset flag bits.
            if (g_GameWork.config.seenGameOverTips_2E[0] == SHRT_MAX)
            {
                g_GameWork.config.seenGameOverTips_2E[0] = 0;
            }

            randTipVal = 0;

            seenTipIdxs[0] = g_GameWork.config.seenGameOverTips_2E[0];
            for (tipIdx = 0; tipIdx < TIP_COUNT; tipIdx++)
            {
                if (!Flags16b_IsSet(seenTipIdxs, tipIdx))
                {
                    if ((!(g_SysWork.field_2388.field_154.effectsInfo_0.field_0.field_0 & 0x3) && (tipIdx - 13) >= 2u) ||
                        ( (g_SysWork.field_2388.field_154.effectsInfo_0.field_0.field_0 & 0x3) && (tipIdx - 13) <  2u))
                    {
                        randTipVal += 3;
                    }
                    else
                    {
                        randTipVal++;
                    }
                }
            }

            randTipVal = Rng_GenerateInt(0, randTipVal - 1);

            // `randTipVal` seems to go unused after loop, gets checked during loop and can cause early exit,
            // thereby affecting what `tipIdx` will contain.
            for (tipIdx = 0; tipIdx < TIP_COUNT; tipIdx++)
            {
                if (!Flags16b_IsSet(seenTipIdxs, tipIdx))
                {
                    if ((!(g_SysWork.field_2388.field_154.effectsInfo_0.field_0.field_0 & 0x3) && (tipIdx - 13) >= 2u) ||
                        ( (g_SysWork.field_2388.field_154.effectsInfo_0.field_0.field_0 & 0x3) && (tipIdx - 13) <  2u))
                    {
                        if (randTipVal < 3)
                        {
                            break;
                        }

                        randTipVal -= 3;
                    }
                    else
                    {
                        if (randTipVal <= 0)
                        {
                            break;
                        }

                        randTipVal--;
                    }
                }
            }

            // Store current shown `tipIdx`, later `sysStateSteps == 7` will set it inside `seenGameOverTips_2E`.
            prevTipIdx = tipIdx;

#if VERSION_REGION_IS(NTSC)
            Fs_QueueStartReadTim(FILE_TIM_TIPS_E01_TIM + tipIdx, FS_BUFFER_1, &g_DeathTipImg);
#elif VERSION_REGION_IS(NTSCJ)
            Fs_QueueStartReadTim(FILE_TIM_TIPS_J01_TIM + tipIdx, FS_BUFFER_1, &g_DeathTipImg);
#endif
            SysWork_StateStepIncrement(0);

        case 1:
            SysWork_StateStepIncrementAfterFade(2, true, 0, Q12(0.5f), false);
            break;

        case 2:
            SysWork_StateStepIncrementAfterFade(0, false, 0, Q12(0.5f), false);
            SysWork_StateStepIncrement(0);

        case 3:
            Gfx_StringSetPosition(SCREEN_POSITION_X(32.5f), SCREEN_POSITION_Y(43.5f));
            Gfx_StringDraw("\aGAME_OVER", DEFAULT_MAP_MESSAGE_LENGTH);
            g_SysWork.field_28++;

            if ((g_Controller0->btnsClicked_10 & (g_GameWorkPtr->config.controllerConfig.enter |
                                                  g_GameWorkPtr->config.controllerConfig.cancel)) ||
                g_SysWork.field_28 > Q12(1.0f / 17.0f))
            {
                SysWork_StateStepIncrement(0);
            }
            break;

        case 4:
            Gfx_StringSetPosition(SCREEN_POSITION_X(32.5f), SCREEN_POSITION_Y(43.5f));
            Gfx_StringDraw("\aGAME_OVER", DEFAULT_MAP_MESSAGE_LENGTH);
            SysWork_StateStepIncrementAfterFade(2, true, 0, Q12(2.0f), false);
            break;

        case 5:
            if (g_SavegamePtr->gameDifficulty_260 == GameDifficulty_Hard)
            {
                SysWork_StateStepReset();
                break;
            }
            else
            {
                Fs_QueueWaitForEmpty();
                Game_RadioSoundStop();
                SysWork_StateStepIncrement(0);
            }

        case 6:
            SysWork_StateStepIncrementAfterFade(2, false, 0, Q12(2.0f), false);
            g_SysWork.field_28 = Q12(0.0f);
            Screen_BackgroundImgDraw(&g_DeathTipImg);
            break;

        case 7:
            g_SysWork.field_28++;
            Screen_BackgroundImgDraw(&g_DeathTipImg);

            if (!(g_Controller0->btnsClicked_10 & (g_GameWorkPtr->config.controllerConfig.enter |
                                                   g_GameWorkPtr->config.controllerConfig.cancel)))
            {
                if (g_SysWork.field_28 <= 480)
                {
                    break;
                }
            }

            // TODO: some inline FlagSet func? couldn't get matching ver, but pretty sure temp_a0 can be removed somehow
            temp_a0 = &g_GameWork.config.seenGameOverTips_2E[(prevTipIdx >> 5)];
            *temp_a0 |= (1 << 0) << (prevTipIdx & 0x1F);

            SysWork_StateStepIncrement(0);
            break;

        case 8:
            Screen_BackgroundImgDraw(&g_DeathTipImg);
            SysWork_StateStepIncrementAfterFade(2, true, 0, Q12(2.0f), false);
            break;

        default:
            g_MapOverlayHeader.playerControlUnfreeze_CC(0);
            SysWork_StateSetNext(SysState_Gameplay);
            Game_WarmBoot();
            break;
    }

    if (g_SysWork.sysStateSteps[0] >= 2 || g_GameWork.gameState != GameState_InGame)
    {
        g_SysWork.bgmStatusFlags |= BgmStatusFlag_Pause;
    }

    #undef TIP_COUNT
}

void GameState_MapEvent_Update(void) // 0x8003AA4C
{
    if (g_GameWork.gameStateSteps[0] == 0)
    {
        g_IntervalVBlanks               = 1;
        ScreenFade_Start(true, true, false);
        g_GameWork.gameStateSteps[0] = 1;
    }

    D_800A9A0C = ScreenFade_IsFinished() && Fs_QueueChunksLoad();

    Savegame_EventFlagSetAlt(g_MapEventData->disabledEventFlag);

#ifdef SH_PC_PORT
    if (g_MapEventParam < 0 || g_MapEventParam >= 64
        || g_MapOverlayHeader.mapEventFuncs_20[g_MapEventParam] == NULL) {
        SH_DBG("[SS] MapEvent param=%d OOB/NULL — skip", g_MapEventParam);
        Screen_BackgroundImgDraw(&g_ItemInspectionImg);
        return;
    }
#endif
    g_MapOverlayHeader.mapEventFuncs_20[g_MapEventParam]();

    Screen_BackgroundImgDraw(&g_ItemInspectionImg);
}
