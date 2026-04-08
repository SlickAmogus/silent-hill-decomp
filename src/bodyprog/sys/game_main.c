#include "game.h"

#ifdef SH_PC_PORT
#include "sh_log.h"
#include "pc_config.h"
#include <string.h>
extern void PsyX_EndScene(void);
extern void PsyX_UpdateInput(void);
extern float g_PsyX_FogColor[3];
extern int g_PcHorPlusEnabled;
#include <stdio.h>
#include <SDL_scancode.h>
extern u8 g_WorldEnvWork[];
#define PC_WorldEnvWork (*(s_WorldEnvWork*)g_WorldEnvWork)
#include "bodyprog/view/vw_main.h"
extern void vcGetNowCamPos(VECTOR3* cam_pos);
extern const unsigned char* g_sdlKeyboardState;
#include "debug_console.h"
#include "map_registry.h"
#endif
#include <psyq/libetc.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/demo.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/screen/vsync.h"
#include "bodyprog/sys/joy.h"
#include "bodyprog/text/text_draw.h"
#include "bodyprog/math/math.h"
#include "bodyprog/memcard.h"
#include "bodyprog/sound_system.h"
#include "screens/b_konami/b_konami.h"

#include "bodyprog/memcard.h"
#include "bodyprog/sys/game_main.h"
#include "screens/saveload.h"

// ========================================
// GLOBAL VARIABLES
// ========================================

s32 g_Demo_FrameCount = 0;
s32 g_WarmBootTimer = 0;

// ========================================
// STATIC VARIABLES
// ========================================

static s32 g_PrevVBlanks = 0;

#ifdef SH_PC_PORT
/* Packet buffer corruption detection canaries */
/* With preloading, up to ~25 chunks render (5x5 grid around player).
 * Each chunk uses ~40KB of primitives. 25 * 40 = 1MB. 2MB gives headroom. */
#define PC_PKTBUF_SIZE (2 * 1024 * 1024)
#define PC_CANARY_SIZE 64
#define PC_CANARY_VAL  0xDE
static PACKET* s_PcPacketBufs[2] = { NULL, NULL };
static PACKET* s_PcPacketBufEnds[2] = { NULL, NULL };
#endif

// Audio task for `SD_Call` meant to load base VAB audios.
static u16 g_baseVabAudiosTaskId[] = {
   160,
   162,
   0
};

static void (*g_GameStateUpdateFuncs[])(void) = {
    GameState_Boot_Update,
    GameState_KonamiLogo_Update,
    GameState_KcetLogo_Update,
    GameState_MovieIntroFadeIn_Update,
    GameState_AutoLoadSavegame_Update,
    GameState_MovieIntroAlternate_Update,
    GameState_MovieIntro_Update,
    GameState_MainMenu_Update,
    GameState_LoadSavegameScreen_Update,
    GameState_MovieOpening_Update,
    GameState_LoadScreen_Update,
    GameState_InGame_Update,
    GameState_MapEvent_Update,
    GameState_ExitMovie_Update,
    GameState_ItemScreens_Update,
    GameState_MapScreen_Update,
    GameState_LoadSavegameScreen_Update,
    GameState_DebugMoviePlayer_Update,
    GameState_Options_Update,
    GameState_LoadStatusScreen_Update,
    GameState_LoadMapScreen_Update,
    GameState_Unk15_Update
};

// ========================================
// DEBUG CAMERA (PC PORT)
// ========================================

#ifdef SH_PC_PORT
int g_DebugCamEnabled = 0;  /* 0 = normal camera, 1 = debug camera */
int g_DebugFogDisabled = 0; /* 0 = fog normal, 1 = fog forced off (debug cam only) */
int g_DebugNoWallCollision = 0;  /* 0 = wall collision on, 1 = walk through walls */
int g_DebugNoFloorCollision = 0; /* 0 = floor collision on, always on (toggle removed) */
int g_DebugThirdPersonCam = 0;   /* 0 = game camera, 1 = static third-person follow cam */
int g_DebugUnlockFps = 0;        /* 0 = fps_cap from config, 1 = uncapped (debug toggle) */
static int g_DebugCamInited = 0;
static int g_DebugCamTogglePrev = 0; /* for edge detection on toggle key */
static int g_DebugFogTogglePrev = 0;
static VECTOR3 g_DebugCamPos;
static VECTOR3 g_DebugCamLookAt;
static q3_12 g_DebugCamAngleY = 0;
static VECTOR3 g_DebugCamSavedHarryPos; /* Harry's position when debug cam was enabled */
static s32 g_DebugCamSavedHarryPosY;    /* Separate Y for collision restore */

void DebugCamera_Update(void)
{
    #define DBG_CAM_MOVE_SPEED 2048  /* Q12(0.5) */
    #define DBG_CAM_TURN_SPEED 64
    #define DBG_CAM_VERT_SPEED 1228  /* Q12(0.3) */

    if (!g_sdlKeyboardState) return;
    if (g_GameWork.gameState_594 != GameState_InGame) return;

    /* Numpad *: toggle debug camera on/off (edge-triggered) */
    {
        int cur = g_sdlKeyboardState[SDL_SCANCODE_KP_MULTIPLY];
        if (cur && !g_DebugCamTogglePrev) {
            g_DebugCamEnabled = !g_DebugCamEnabled;
            if (g_DebugCamEnabled) {
                /* Capture current camera as starting point */
                vcGetNowCamPos(&g_DebugCamPos);
                g_DebugCamAngleY = g_SysWork.cameraAngleY_237A;
                g_DebugCamInited = 1;
                /* Save Harry's position to restore when debug cam is disabled */
                g_DebugCamSavedHarryPos = g_SysWork.playerWork_4C.player_0.position_18;
                g_DebugCamSavedHarryPosY = g_SysWork.playerWork_4C.player_0.properties_E4.player.positionY_EC;
                SH_DBG("[DBGCAM] ENABLED pos=(%ld,%ld,%ld) harryPos saved=(%ld,%ld,%ld)",
                    (long)g_DebugCamPos.vx, (long)g_DebugCamPos.vy, (long)g_DebugCamPos.vz,
                    (long)g_DebugCamSavedHarryPos.vx, (long)g_DebugCamSavedHarryPos.vy, (long)g_DebugCamSavedHarryPos.vz);
            } else {
                /* Restore Harry's original position */
                g_SysWork.playerWork_4C.player_0.position_18 = g_DebugCamSavedHarryPos;
                g_SysWork.playerWork_4C.player_0.properties_E4.player.positionY_EC = g_DebugCamSavedHarryPosY;
                SH_DBG("[DBGCAM] DISABLED — restored harry to (%ld,%ld,%ld)",
                    (long)g_DebugCamSavedHarryPos.vx, (long)g_DebugCamSavedHarryPos.vy, (long)g_DebugCamSavedHarryPos.vz);
            }
        }
        g_DebugCamTogglePrev = cur;
    }

    /* Fog toggle moved to main loop (runs after game sets fog params) */

    /* Numpad 0: cycle to next map overlay (edge-triggered)
     * DISABLED: runtime map switching crashes (map data not safely teardown-able).
     * Use config.cfg map= setting instead. */
#if 0
    {
        static int prevKey = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_KP_0];
        if (cur && !prevKey) {
            int curId = (int)g_SavegamePtr->mapOverlayId_A4;
            int nextId = (curId + 1) % (MapOverlayId_MAPX_S00 + 1);
            g_SavegamePtr->mapOverlayId_A4 = nextId;
            MapRegistry_Load((e_MapOverlayId)nextId);
            extern void GameBoot_MapLoad(s32 mapIdx);
            GameBoot_MapLoad(nextId);
            SH_DBG("[DEBUG] Switched to map %s (overlay %d)",
                MapRegistry_GetName(nextId), nextId);
        }
        prevKey = cur;
    }
#endif

    /* Numpad 1: toggle wall collision (edge-triggered) */
    {
        static int prevKey = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_KP_1];
        if (cur && !prevKey) {
            g_DebugNoWallCollision = !g_DebugNoWallCollision;
            SH_DBG("[DEBUG] Wall collision: %s", g_DebugNoWallCollision ? "OFF (noclip)" : "ON");
        }
        prevKey = cur;
    }
    /* Numpad 2: toggle third-person follow camera (edge-triggered) */
    {
        static int prevKey = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_KP_2];
        if (cur && !prevKey) {
            g_DebugThirdPersonCam = !g_DebugThirdPersonCam;
            SH_DBG("[DEBUG] Third-person camera: %s", g_DebugThirdPersonCam ? "ON" : "OFF");
        }
        prevKey = cur;
    }

    /* Numpad 3: rescue teleport — snap Harry back to room spawn position (edge-triggered) */
    {
        static int prevKey = 0;
        s_SubCharacter* hp = &g_SysWork.playerWork_4C.player_0;

        int cur = g_sdlKeyboardState[SDL_SCANCODE_KP_3];
        if (cur && !prevKey) {
            /* Use the spawn position from the savegame data, then query
             * Collision_Get at that position for the correct ground height. */
            VECTOR3 spawnPos;
            s_Collision _rescueColl;
            spawnPos.vx = g_SavegamePtr->playerPositionX_244;
            spawnPos.vz = g_SavegamePtr->playerPositionZ_24C;

            /* If savegame position looks invalid (all zeros), use a known default */
            if (spawnPos.vx == 0 && spawnPos.vz == 0) {
                spawnPos.vx = 573440;  /* map0_s00 default spawn */
                spawnPos.vz = 77824;
            }

            /* Query ground height at spawn position. If Collision_Get returns
             * the default (Q12(8.0)=32768, meaning chunk not loaded), use 0
             * which is the correct floor level for most maps. */
            Collision_Get(&_rescueColl, spawnPos.vx, spawnPos.vz);
            spawnPos.vy = _rescueColl.groundHeight_0;
            if (spawnPos.vy == Q12(8.0f)) {
                spawnPos.vy = 0;  /* default ground level */
            }

            /* Skip teleport during cutscenes (ev_cam_rate > 0 = border active).
             * Updating Harry's position while DMS is running would conflict with
             * bone animation and corrupt the skeleton for the rest of the scene. */
            if (g_WorldGfxWork.vcCameraInternalInfo_1BDC.ev_cam_rate > 0) {
                SH_DBG("[DEBUG] Rescue teleport: skipped (cutscene active)");
            } else {
                SH_DBG("[DEBUG] Rescue teleport: (%ld,%ld,%ld) -> spawn (%ld,%ld,%ld)",
                       (long)hp->position_18.vx, (long)hp->position_18.vy, (long)hp->position_18.vz,
                       (long)spawnPos.vx, (long)spawnPos.vy, (long)spawnPos.vz);
                hp->position_18 = spawnPos;
                hp->fallSpeed_34 = Q12(0.0f);
                hp->properties_E4.player.positionY_EC = spawnPos.vy;
                /* Do NOT touch playerBoneCoords_890 — the normal skeleton update will
                 * sync root bone coords from position_18 on the next frame. Updating
                 * them here during a cutscene conflicts with DMS bone animation. */
            }
        }
        prevKey = cur;
    }
    /* Numpad 0: toggle FPS cap on/off (edge-triggered) */
    {
        static int prevKey = 0;
        int cur = g_sdlKeyboardState[SDL_SCANCODE_KP_0];
        if (cur && !prevKey) {
            g_DebugUnlockFps = !g_DebugUnlockFps;
            SH_DBG("[DEBUG] FPS cap: %s", g_DebugUnlockFps ? "UNLOCKED" : "config value");
        }
        prevKey = cur;
    }

    /* If free-fly debug cam is off */
    if (!g_DebugCamEnabled) {
        /* Third-person follow camera: static overhead-behind view, no input handling */
        if (g_DebugThirdPersonCam) {
            #define TP_DIST         Q12(2.5f)    /* world units behind Harry */
            #define TP_HEIGHT       Q12(-1.4f)   /* world units above Harry (Y-up = negative) */
            #define TP_LOOKAT_OFS   Q12(-0.85f)  /* Y offset for look target (Harry's chest) */

            s_SubCharacter* tp_hr = &g_SysWork.playerWork_4C.player_0;
            s32 tpSinY = Math_Sin(tp_hr->rotation_24.vy);
            s32 tpCosY = Math_Cos(tp_hr->rotation_24.vy);

            VECTOR3 tpCamPos, tpLookAt;
            /* Place camera behind Harry at elevated position */
            tpCamPos.vx = tp_hr->position_18.vx - (s32)((s64)TP_DIST * tpSinY >> 12);
            tpCamPos.vz = tp_hr->position_18.vz - (s32)((s64)TP_DIST * tpCosY >> 12);
            tpCamPos.vy = tp_hr->position_18.vy + TP_HEIGHT;

            /* Look at Harry's upper body */
            tpLookAt.vx = tp_hr->position_18.vx;
            tpLookAt.vz = tp_hr->position_18.vz;
            tpLookAt.vy = tp_hr->position_18.vy + TP_LOOKAT_OFS;

            Vw_SetLookAtMatrix(&tpCamPos, &tpLookAt);
            vwSetViewInfo();

            #undef TP_DIST
            #undef TP_HEIGHT
            #undef TP_LOOKAT_OFS
        }
        return;
    }

    int moved = 0;
    s32 sinY = Math_Sin(g_DebugCamAngleY);
    s32 cosY = Math_Cos(g_DebugCamAngleY);

    /* Numpad 8: forward */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_8]) {
        g_DebugCamPos.vx += (s32)((s64)DBG_CAM_MOVE_SPEED * sinY >> 12);
        g_DebugCamPos.vz += (s32)((s64)DBG_CAM_MOVE_SPEED * cosY >> 12);
        moved = 1;
    }
    /* Numpad 5: backward */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_5]) {
        g_DebugCamPos.vx -= (s32)((s64)DBG_CAM_MOVE_SPEED * sinY >> 12);
        g_DebugCamPos.vz -= (s32)((s64)DBG_CAM_MOVE_SPEED * cosY >> 12);
        moved = 1;
    }
    /* Numpad 4: strafe left */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_4]) {
        g_DebugCamPos.vx -= (s32)((s64)DBG_CAM_MOVE_SPEED * cosY >> 12);
        g_DebugCamPos.vz += (s32)((s64)DBG_CAM_MOVE_SPEED * sinY >> 12);
        moved = 1;
    }
    /* Numpad 6: strafe right */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_6]) {
        g_DebugCamPos.vx += (s32)((s64)DBG_CAM_MOVE_SPEED * cosY >> 12);
        g_DebugCamPos.vz -= (s32)((s64)DBG_CAM_MOVE_SPEED * sinY >> 12);
        moved = 1;
    }
    /* Numpad 7: turn left */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_7]) {
        g_DebugCamAngleY -= DBG_CAM_TURN_SPEED;
        moved = 1;
    }
    /* Numpad 9: turn right */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_9]) {
        g_DebugCamAngleY += DBG_CAM_TURN_SPEED;
        moved = 1;
    }
    /* Numpad +: move up (Y-, PSX Y is inverted) */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_PLUS]) {
        g_DebugCamPos.vy -= DBG_CAM_VERT_SPEED;
        moved = 1;
    }
    /* Numpad -: move down (Y+) */
    if (g_sdlKeyboardState[SDL_SCANCODE_KP_MINUS]) {
        g_DebugCamPos.vy += DBG_CAM_VERT_SPEED;
        moved = 1;
    }
    /* Numpad /: print debug camera coordinates to log */
    {
        static int dbg_slash_prev = 0;
        int dbg_slash_cur = g_sdlKeyboardState[SDL_SCANCODE_KP_DIVIDE];
        if (dbg_slash_cur && !dbg_slash_prev) {
            SH_DBG("[DBGCAM] COORDS: pos=(%ld,%ld,%ld) angleY=%d",
                (long)g_DebugCamPos.vx, (long)g_DebugCamPos.vy, (long)g_DebugCamPos.vz,
                g_DebugCamAngleY);
            SH_DBG("[DBGCAM] HARRY:  pos=(%ld,%ld,%ld)",
                (long)g_SysWork.playerWork_4C.player_0.position_18.vx,
                (long)g_SysWork.playerWork_4C.player_0.position_18.vy,
                (long)g_SysWork.playerWork_4C.player_0.position_18.vz);
        }
        dbg_slash_prev = dbg_slash_cur;
    }

    /* Move Harry to follow behind the debug camera. This ensures that the
     * material/texture system (which loads textures near Harry's position)
     * textures chunks around wherever the debug camera is exploring. */
    {
        s_SubCharacter* hp = &g_SysWork.playerWork_4C.player_0;
        hp->position_18.vx = g_DebugCamPos.vx;
        hp->position_18.vz = g_DebugCamPos.vz;
        /* Keep Harry at ground level (don't follow camera Y) */
    }

    /* Set look-at point ahead of camera */
    g_DebugCamLookAt.vx = g_DebugCamPos.vx + (s32)((s64)20480 * Math_Sin(g_DebugCamAngleY) >> 12);
    g_DebugCamLookAt.vy = g_DebugCamPos.vy;
    g_DebugCamLookAt.vz = g_DebugCamPos.vz + (s32)((s64)20480 * Math_Cos(g_DebugCamAngleY) >> 12);

    /* Override the camera view */
    Vw_SetLookAtMatrix(&g_DebugCamPos, &g_DebugCamLookAt);
    vwSetViewInfo();

    if (moved) {
        static int dbg_print_counter = 0;
        if (++dbg_print_counter % 30 == 0) {
            SH_DBG("[DBGCAM] pos=(%ld,%ld,%ld) angleY=%d harry=(%ld,%ld,%ld)",
                (long)g_DebugCamPos.vx, (long)g_DebugCamPos.vy, (long)g_DebugCamPos.vz,
                g_DebugCamAngleY,
                (long)g_SysWork.playerWork_4C.player_0.position_18.vx,
                (long)g_SysWork.playerWork_4C.player_0.position_18.vy,
                (long)g_SysWork.playerWork_4C.player_0.position_18.vz);
        }
    }

    #undef DBG_CAM_MOVE_SPEED
    #undef DBG_CAM_TURN_SPEED
    #undef DBG_CAM_VERT_SPEED
}
#endif

// ========================================
// MAINLOOP
// ========================================

void GameState_Boot_Update(void) // 0x80032D1C
{
    s32 gameState;
    s32 VabAudioTaskId;

    switch (g_GameWork.gameStateStep_598[0])
    {
        case 0:
            g_GameWork.background2dColor_58C.r = 0;
            g_GameWork.background2dColor_58C.g = 0;
            g_GameWork.background2dColor_58C.b = 0;

            Screen_Init(SCREEN_WIDTH, false);
            g_SysWork.counters_1C[1]              = 0;
            g_GameWork.gameStateStep_598[1] = 0;
            g_GameWork.gameStateStep_598[2] = 0;
            g_GameWork.gameStateStep_598[0]++;
            break;

        case 1:
            if (!Sd_AudioStreamingCheck())
            {
                VabAudioTaskId = g_baseVabAudiosTaskId[g_GameWork.gameStateStep_598[1]];
                if (VabAudioTaskId != 0)
                {
                    SD_Call(VabAudioTaskId);
                    g_GameWork.gameStateStep_598[1]++;
                }
                else
                {
                    g_SysWork.counters_1C[1]              = 0;
                    g_GameWork.gameStateStep_598[1] = 0;
                    g_GameWork.gameStateStep_598[2] = 0;
                    g_GameWork.gameStateStep_598[0]++;
                }
            }
            break;

        case 2:
            Fs_QueueStartReadTim(FILE_1ST_FONT16_TIM, FS_BUFFER_1, &g_Font16AtlasImg);
            Fs_QueueStartReadTim(FILE_1ST_KONAMI_TIM, FS_BUFFER_1, &g_KonamiLogoImg);

            ScreenFade_Start(true, false, false);
            g_GameWork.gameStateStep_598[0]++;
            break;

        case 3:
            if (ScreenFade_IsFinished())
            {
                Fs_QueueWaitForEmpty();

                gameState = g_GameWork.gameState_594;

                g_SysWork.counters_1C[0] = 0;
                g_SysWork.counters_1C[1] = 0;

                g_GameWork.gameStateStep_598[1] = 0;
                g_GameWork.gameStateStep_598[2] = 0;

                SysWork_StateSetNext(SysState_Gameplay);

                g_GameWork.gameStateStep_598[0] = gameState;
#ifdef SH_PC_PORT
                /* Skip logos/movie for non-default maps — jump straight to MainMenu */
                if (strcmp(g_PcConfig.mapName, "map0_s00") != 0)
                    g_GameWork.gameState_594 = GameState_MainMenu;
                else
#endif
                g_GameWork.gameState_594        = gameState + 1;
                g_GameWork.gameStatePrev_590    = gameState;
                g_GameWork.gameStateStep_598[0] = 0;
            }
            break;
    }

    func_80033548();
    Screen_BackgroundImgDraw(&g_MainImg0);
    func_80089090(1);
}

void MainLoop(void) // 0x80032EE0
{
    #define TICKS_PER_SECOND_MIN (TICKS_PER_SECOND / 4)
    #define H_BLANKS_PER_SECOND  15780
    #define H_BLANKS_PER_TICK    (H_BLANKS_PER_SECOND / TICKS_PER_SECOND) // 263

    #define H_BLANKS_TO_SEC_CONVERSION_FACTOR ((float)Q12(1.0f) / (float)H_BLANKS_PER_SECOND)             // 0.25956907477f
    #define H_BLANKS_PER_FRAME_MIN            (H_BLANKS_PER_SECOND / TICKS_PER_SECOND_MIN)                // 1052
    #define H_BLANKS_Q12_TO_SEC_SCALE         (s32)(H_BLANKS_TO_SEC_CONVERSION_FACTOR * (float)Q12(1.0f)) // 1063
    #define H_BLANKS_GRAVITY_SCALE            Q12(9.8f * H_BLANKS_TO_SEC_CONVERSION_FACTOR)               // 10419
    #define V_BLANKS_MAX                      4

    s32 vBlanks;
    s32 vCount;
    s32 vCountCopy;
    s32 interval;

    // Initialize engine.
    GsInitVcount();
    MemCard_SysInit();
    MemCard_SysInit2();
    MemCard_InitStatus();
    Joy_Init();
    VSyncCallback(&Screen_VSyncCallback);

    // NTSC-J moves these calls into the `HP_SAFE1` / `S__SAFE2` anti-modchip overlays.
    // Likely to make sure those overlays aren't patched out by pirates.
#if !VERSION_REGION_IS(NTSCJ)
    InitGeom();
    ItemScreen_TmdGsFCallInit();
#ifdef SH_PC_PORT
    /* Skip vibration init - requires PadInfoMode which may crash without real pad */
#else
    func_800890B8();
#endif
#endif

    SD_Init();

#ifdef SH_PC_PORT
    /* SD_Init -> SdInit -> SpuInit -> ResetCallback clears the VSync callback.
     * Re-register it after SD_Init to ensure the callback stays active. */
    VSyncCallback(&Screen_VSyncCallback);
#endif
    // Run game.
    while (true)
    {
        g_TickCount++;

#ifdef SH_PC_PORT
        /* PsyCross requires explicit input polling — on PSX this happens
         * via hardware interrupt during VBlank. */
        PsyX_UpdateInput();
        DebugConsole_Update();
#endif
        // Update input.
        Joy_ReadP1();
        Demo_ControllerDataUpdate();
        Joy_ControllerDataUpdate();

        if (MainLoop_ShouldWarmReset() == 2)
        {
            Game_WarmBoot();
            continue;
        }

        g_ActiveBufferIdx = GsGetActiveBuff();

#ifdef SH_PC_PORT
        /* PC primitives are larger than PSX (8-byte pointers, bigger structs).
         * The original 128KB packet buffer overflows when rendering 2+ characters.
         * Allocate 512KB per buffer from heap instead of fixed PSX temp memory.
         * Extra 64 bytes of canary at the end for corruption detection. */
        if (!s_PcPacketBufs[0]) {
            s_PcPacketBufs[0] = (PACKET*)calloc(1, PC_PKTBUF_SIZE + PC_CANARY_SIZE);
            s_PcPacketBufs[1] = (PACKET*)calloc(1, PC_PKTBUF_SIZE + PC_CANARY_SIZE);
            s_PcPacketBufEnds[0] = s_PcPacketBufs[0] + PC_PKTBUF_SIZE;
            s_PcPacketBufEnds[1] = s_PcPacketBufs[1] + PC_PKTBUF_SIZE;
            memset(s_PcPacketBufEnds[0], PC_CANARY_VAL, PC_CANARY_SIZE);
            memset(s_PcPacketBufEnds[1], PC_CANARY_VAL, PC_CANARY_SIZE);
        }
        GsOUT_PACKET_P = s_PcPacketBufs[g_ActiveBufferIdx];
#else
        if (g_GameWork.gameState_594 == GameState_MainLoadScreen ||
            g_GameWork.gameState_594 == GameState_InGame)
        {
            GsOUT_PACKET_P = (PACKET*)(TEMP_MEMORY_ADDR + (g_ActiveBufferIdx << 17));
        }
        else if (g_GameWork.gameState_594 == GameState_InventoryScreen)
        {
            GsOUT_PACKET_P = (PACKET*)(TEMP_MEMORY_ADDR + (g_ActiveBufferIdx * 40000));
        }
        else
        {
            GsOUT_PACKET_P = (PACKET*)(TEMP_MEMORY_ADDR + (g_ActiveBufferIdx << 15));
        }
#endif

        GsClearOt(0, 0, &g_OrderingTable0[g_ActiveBufferIdx]);
        GsClearOt(0, 0, &g_OrderingTable2[g_ActiveBufferIdx]);

        g_SysWork.sysFlags_22A0 = SysFlag_None;

        // Call update function for current GameState.
        g_GameStateUpdateFuncs[g_GameWork.gameState_594]();
#ifdef SH_PC_PORT
        if (g_GameWork.gameState_594 == GameState_InGame) {
            /* Canary checks after InGame state update */
            /* --- Canary checks after game state update --- */
            {
                PACKET* pktEnd0 = s_PcPacketBufEnds[0];
                PACKET* pktEnd1 = s_PcPacketBufEnds[1];
                PACKET* pktStart = s_PcPacketBufs[g_ActiveBufferIdx];
                ptrdiff_t pktUsed = GsOUT_PACKET_P - pktStart;
                int canaryOk = 1;
                int i;
                for (i = 0; i < PC_CANARY_SIZE; i++) {
                    if (pktEnd0[i] != PC_CANARY_VAL) { canaryOk = 0; break; }
                }
                if (!canaryOk) {
                    SH_DBG("[CANARY] *** PACKET BUF 0 OVERFLOW! byte %d changed to 0x%02X (used=%td/%d)", i, (unsigned char)pktEnd0[i], pktUsed, PC_PKTBUF_SIZE);
                }
                canaryOk = 1;
                for (i = 0; i < PC_CANARY_SIZE; i++) {
                    if (pktEnd1[i] != PC_CANARY_VAL) { canaryOk = 0; break; }
                }
                if (!canaryOk) {
                    SH_DBG("[CANARY] *** PACKET BUF 1 OVERFLOW! byte %d changed to 0x%02X (used=%td/%d)", i, (unsigned char)pktEnd1[i], pktUsed, PC_PKTBUF_SIZE);
                }
                SH_DBG("[PKTBUF] used=%td/%d (%.1f%%)", pktUsed, PC_PKTBUF_SIZE, (double)pktUsed * 100.0 / PC_PKTBUF_SIZE);
            }
        }
#endif

        Demo_Update();
        Demo_GameRandSeedSet();

        if (MainLoop_ShouldWarmReset() == 2)
        {
            Game_WarmBoot();
            continue;
        }

#ifdef SH_PC_PORT
#define ML_TRACE(tag) ((void)0)
#else
#define ML_TRACE(tag) ((void)0)
#endif
        ML_TRACE("Screen_FadeUpdate");
        Screen_FadeUpdate();
        ML_TRACE("MemCard_Update");
        MemCard_Update();
        ML_TRACE("Sd_TaskPoolExecute");
        Sd_TaskPoolExecute();

        if (!Sd_AudioStreamingCheck())
        {
            ML_TRACE("Fs_QueueUpdate");
            Fs_QueueUpdate();
        }

        ML_TRACE("func_80089128");
        func_80089128();
        ML_TRACE("func_8008D78C");
        func_8008D78C(); // Camera update?
        ML_TRACE("DrawSync");
        DrawSync(SyncMode_Wait);
        ML_TRACE("VSync-begin");
        // Handle V sync.
        if (g_SysWork.flags_22A4 & SysFlag2_1)
        {
            ML_TRACE("VSync-flag2_1-branch");
            vBlanks   = VSync(SyncMode_Count);
            g_VBlanks = vBlanks - g_PrevVBlanks;

            Demo_PresentIntervalUpdate();

            interval      = g_Demo_VideoPresentInterval;
            g_PrevVBlanks = vBlanks;

            if (interval < g_IntervalVBlanks)
            {
                interval = g_IntervalVBlanks;
            }

            do
            {
                VSync(SyncMode_Wait);
                g_VBlanks++;
                g_PrevVBlanks++;
            }
            while (g_VBlanks < interval);

            g_UncappedVBlanks = g_VBlanks;
            g_VBlanks         = MIN(g_VBlanks, 4);

            vCount     = g_Demo_VideoPresentInterval * H_BLANKS_PER_TICK;
            vCountCopy = g_UncappedVBlanks * H_BLANKS_PER_TICK;
            g_VBlanks  = g_Demo_VideoPresentInterval;
        }
        else
        {
            if (g_SysWork.sysState_8 != SysState_Gameplay)
            {
                ML_TRACE("VSync-nonGameplay");
                g_VBlanks     = VSync(SyncMode_Count) - g_PrevVBlanks;
                g_PrevVBlanks = VSync(SyncMode_Count);
                ML_TRACE("VSync-Wait");
                VSync(SyncMode_Wait);
                ML_TRACE("VSync-Wait-done");
            }
            else
            {
                ML_TRACE("VSync-gameplay");
                if (!ScreenFade_IsNone())
                {
                    VSync(SyncMode_Wait);
                }

                g_VBlanks     = VSync(SyncMode_Count) - g_PrevVBlanks;
                g_PrevVBlanks = VSync(SyncMode_Count);

#ifdef SH_PC_PORT
                /* Compute effective vblank interval from fps_cap config and debug toggle.
                 * Only override the game's own g_IntervalVBlanks when fully in gameplay —
                 * menu sub-states (map, items, pause text) set g_IntervalVBlanks=1 themselves
                 * and must not be overridden, or they drop to 30fps while overlays are open. */
                {
                    int effectiveMin = g_IntervalVBlanks;
                    if (g_GameWork.gameState_594 == GameState_InGame)
                    {
                        if (g_DebugUnlockFps || g_PcConfig.fpsCap == 0)
                        {
                            effectiveMin = 0; /* uncapped: don't wait */
                        }
                        else if (g_PcConfig.fpsCap > 0 && g_PcConfig.fpsCap < 60)
                        {
                            /* e.g. fps_cap=30 → 60/30=2 vblanks, fps_cap=20 → 60/20=3 vblanks */
                            effectiveMin = 60 / g_PcConfig.fpsCap;
                        }
                    }

                    while (g_VBlanks < effectiveMin)
                    {
                        VSync(SyncMode_Wait);
                        g_VBlanks++;
                        g_PrevVBlanks++;
                    }
                }
#else
                while (g_VBlanks < g_IntervalVBlanks)
                {
                    VSync(SyncMode_Wait);
                    g_VBlanks++;
                    g_PrevVBlanks++;
                }
#endif
            }

            // Update V blanks.
            g_UncappedVBlanks = g_VBlanks;
            g_VBlanks         = MIN(g_VBlanks, V_BLANKS_MAX);

            // Update V count.
            vCount     = MIN(GsGetVcount(), H_BLANKS_PER_FRAME_MIN); // NOTE: Will call `GsGetVcount` twice.
            vCountCopy = vCount;
        }

        ML_TRACE("deltaTime");
        // Update delta time.
        g_DeltaTime    = Q12_MULT(vCount, H_BLANKS_Q12_TO_SEC_SCALE);
        g_DeltaTimeRaw = Q12_MULT(vCountCopy, H_BLANKS_Q12_TO_SEC_SCALE);
        g_GravitySpeed = Q12_MULT(vCount, H_BLANKS_GRAVITY_SCALE);
        GsClearVcount();

        ML_TRACE("GsSwapDispBuff");
        // Draw objects?
        GsSwapDispBuff();
        ML_TRACE("post-GsSwapDispBuff");
#ifdef SH_PC_PORT
        /* Numpad .: toggle fog on/off (only active during debug camera) */
        if (g_sdlKeyboardState && g_GameWork.gameState_594 == 11) {
            int cur = g_sdlKeyboardState[SDL_SCANCODE_KP_PERIOD];
            if (cur && !g_DebugFogTogglePrev && g_DebugCamEnabled) {
                g_DebugFogDisabled = !g_DebugFogDisabled;
                SH_DBG("[DEBUG] Fog: %s", g_DebugFogDisabled ? "OFF" : "ON");
            }
            g_DebugFogTogglePrev = cur;

            /* When debug camera is off, fog is always normal */
            if (!g_DebugCamEnabled) {
                g_DebugFogDisabled = 0;
            }

            if (g_DebugFogDisabled) {
                PC_WorldEnvWork.isFogEnabled_1 = 0;
            }
        }

        /* Enable hor+ widescreen only during 3D world states; 2D UI screens
         * (menus, loading screen, memory card warning, etc.) use 4:3 ortho. */
        g_PcHorPlusEnabled = (g_GameWork.gameState_594 == GameState_InGame ||
                              g_GameWork.gameState_594 == GameState_MapEvent) ? 1 : 0;

        /* Override background color with fog color during InGame.
         * fog params are set by Gfx_FlashlightUpdate from the previous frame's
         * update, so they're valid by frame 2+. Use the normal GsSortClear path
         * which PsyCross handles via activeDrawEnv.isbg in PsyX_BeginScene. */
        if (g_GameWork.gameState_594 == 11 && PC_WorldEnvWork.isFogEnabled_1) {
            g_GameWork.background2dColor_58C.r = PC_WorldEnvWork.fogColor_1C.r;
            g_GameWork.background2dColor_58C.g = PC_WorldEnvWork.fogColor_1C.g;
            g_GameWork.background2dColor_58C.b = PC_WorldEnvWork.fogColor_1C.b;
            g_PsyX_FogColor[0] = PC_WorldEnvWork.fogColor_1C.r / 255.0f;
            g_PsyX_FogColor[1] = PC_WorldEnvWork.fogColor_1C.g / 255.0f;
            g_PsyX_FogColor[2] = PC_WorldEnvWork.fogColor_1C.b / 255.0f;
        }
#endif
        ML_TRACE("GsSortClear");
#ifdef SH_PC_PORT
        /* Stack canary — detect if anything corrupted our stack frame */
        {
            volatile u32 _stackCanary = 0xDEADBEEF;
#endif
        GsSortClear(g_GameWork.background2dColor_58C.r, g_GameWork.background2dColor_58C.g, g_GameWork.background2dColor_58C.b, &g_OrderingTable0[g_ActiveBufferIdx]);
        ML_TRACE("post-GsSortClear");
#ifdef SH_PC_PORT
        if (g_GameWork.gameState_594 == 11) {
            /* Sanitize InGame OT0 — only allow known-safe rendering primitives.
             * Strip DR_MODE (0xE0) which crashes PsyCross ProcessDrawEnv,
             * lines (0x40/0x50), and any unknown types. Texture page info is
             * embedded in POLY_FT/GT prims so DR_MODE isn't needed for textures. */
            GsOT* ot0 = &g_OrderingTable0[g_ActiveBufferIdx];
            {
                OT_TAG* cur = (OT_TAG*)ot0->tag;
                int w2 = 0;
                while (cur && !isendprim(cur) && w2 < 8192) {
                    int len = getlen(cur);
                    if (len > 0) {
                        u8 hi = ((P_TAG*)cur)->code & 0xF0;
                        if (len > 32 || (hi != 0x00 && hi != 0x20 && hi != 0x30 &&
                            hi != 0x60 && hi != 0x70 && hi != 0xA0)) {
                            setlen(cur, 0);
                        }
                    }
                    OT_TAG* next = (OT_TAG*)nextPrim(cur);
                    /* Guard against wild pointers from corrupted OT entries */
                    if (next && ((uintptr_t)next < 0x1000 || (uintptr_t)next > (uintptr_t)0x7FFFFFFFFFFF)) {
                        setlen(cur, 0);
                        break;
                    }
                    cur = next;
                    w2++;
                }
            }
        }

#endif
        ML_TRACE("OT0-draw");
#ifdef SH_PC_PORT
        /* Pre-draw canary check: detect if corruption happened during OT build */
        if (g_GameWork.gameState_594 == GameState_InGame) {
            int _ci; int _canaryOk = 1;
            for (_ci = 0; _ci < PC_CANARY_SIZE; _ci++) {
                if (s_PcPacketBufEnds[g_ActiveBufferIdx][_ci] != PC_CANARY_VAL) { _canaryOk = 0; break; }
            }
            if (!_canaryOk) {
                SH_DBG("[CANARY] *** PRE-DRAW: buf %d overflow at byte %d (0x%02X)", g_ActiveBufferIdx, _ci, (unsigned char)s_PcPacketBufEnds[g_ActiveBufferIdx][_ci]);
            }
        }
#endif
        GsDrawOt(&g_OrderingTable0[g_ActiveBufferIdx]);
        ML_TRACE("OT0-done");
#ifdef SH_PC_PORT
        /* Sanitize InGame OT2 — extended whitelist for 2D overlays.
         * OT2 holds text, screen fade, cutscene borders via g_OtTags0 layers.
         * Text uses SPRT (0x64) + DR_TPAGE (0xE1) per glyph, so allow 0xE0
         * range here (DR_TPAGE is safe; the DR_MODE crashes are in OT0). */
        if (g_GameWork.gameState_594 == 11) {
            GsOT* ot2 = &g_OrderingTable2[g_ActiveBufferIdx];
            OT_TAG* cur2 = (OT_TAG*)ot2->tag;
            int w3 = 0;
            while (cur2 && !isendprim(cur2) && w3 < 4096) {
                int len2 = getlen(cur2);
                if (len2 > 0) {
                    u8 hi2 = ((P_TAG*)cur2)->code & 0xF0;
                    if (len2 > 32 || (hi2 != 0x00 && hi2 != 0x20 && hi2 != 0x30 &&
                        hi2 != 0x60 && hi2 != 0x70 && hi2 != 0xA0 && hi2 != 0xE0)) {
                        setlen(cur2, 0);
                    }
                }
                cur2 = (OT_TAG*)nextPrim(cur2);
                w3++;
            }
        }
#endif
        ML_TRACE("OT2-draw");
        GsDrawOt(&g_OrderingTable2[g_ActiveBufferIdx]);
        ML_TRACE("OT2-done");
#ifdef SH_PC_PORT
        ML_TRACE("DebugConsole_Render");
        DebugConsole_Render();
        ML_TRACE("PsyX_EndScene");
        PsyX_EndScene();
        ML_TRACE("frame-done");
        /* End stack canary check */
        if (_stackCanary != 0xDEADBEEF) {
            SH_DBG("[CANARY] *** STACK CORRUPTION! canary=0x%08X", _stackCanary);
        }
        } /* close _stackCanary scope */
#endif
    }

    #undef TICKS_PER_SECOND_MIN
    #undef H_BLANKS_PER_SECOND
    #undef H_BLANKS_PER_TICK
    #undef H_BLANKS_TO_SEC_CONVERSION_FACTOR
    #undef H_BLANKS_PER_FRAME_MIN
    #undef H_BLANKS_Q12_TO_SEC_SCALE
    #undef H_BLANKS_GRAVITY_SCALE
    #undef V_BLANKS_MAX
}
