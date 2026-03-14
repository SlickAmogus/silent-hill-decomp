#include "game.h"

#ifdef SH_PC_PORT
extern void PsyX_EndScene(void);
extern void PsyX_UpdateInput(void);
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
int g_DebugFogDisabled = 0; /* 0 = fog normal, 1 = fog forced off */
static int g_DebugCamInited = 0;
static int g_DebugCamTogglePrev = 0; /* for edge detection on toggle key */
static int g_DebugFogTogglePrev = 0;
static VECTOR3 g_DebugCamPos;
static VECTOR3 g_DebugCamLookAt;
static q3_12 g_DebugCamAngleY = 0;

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
                fprintf(stderr, "[DBGCAM] ENABLED pos=(%ld,%ld,%ld)\n",
                    (long)g_DebugCamPos.vx, (long)g_DebugCamPos.vy, (long)g_DebugCamPos.vz);
            } else {
                fprintf(stderr, "[DBGCAM] DISABLED — returning to game camera\n");
            }
            fflush(stderr);
        }
        g_DebugCamTogglePrev = cur;
    }

    /* Numpad .: toggle fog on/off (edge-triggered) */
    {
        int cur = g_sdlKeyboardState[SDL_SCANCODE_KP_PERIOD];
        if (cur && !g_DebugFogTogglePrev) {
            g_DebugFogDisabled = !g_DebugFogDisabled;
            if (g_DebugFogDisabled) {
                PC_WorldEnvWork.isFogEnabled_1 = 0;
                fprintf(stderr, "[DEBUG] Fog DISABLED\n");
            } else {
                fprintf(stderr, "[DEBUG] Fog ENABLED\n");
            }
            fflush(stderr);
        }
        g_DebugFogTogglePrev = cur;
    }

    /* Keep fog off every frame if toggled (game re-enables it) */
    if (g_DebugFogDisabled) {
        PC_WorldEnvWork.isFogEnabled_1 = 0;
    }

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
            fprintf(stderr, "[DEBUG] Switched to map %s (overlay %d)\n",
                MapRegistry_GetName(nextId), nextId);
            fflush(stderr);
        }
        prevKey = cur;
    }
#endif

    /* If debug cam is off, let normal camera handle everything */
    if (!g_DebugCamEnabled) return;

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
            fprintf(stderr, "[DBGCAM] COORDS: pos=(%ld,%ld,%ld) angleY=%d\n",
                (long)g_DebugCamPos.vx, (long)g_DebugCamPos.vy, (long)g_DebugCamPos.vz,
                g_DebugCamAngleY);
            fprintf(stderr, "[DBGCAM] HARRY:  pos=(%ld,%ld,%ld)\n",
                (long)g_SysWork.playerWork_4C.player_0.position_18.vx,
                (long)g_SysWork.playerWork_4C.player_0.position_18.vy,
                (long)g_SysWork.playerWork_4C.player_0.position_18.vz);
        }
        dbg_slash_prev = dbg_slash_cur;
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
            fprintf(stderr, "[DBGCAM] pos=(%ld,%ld,%ld) angleY=%d harry=(%ld,%ld,%ld)\n",
                (long)g_DebugCamPos.vx, (long)g_DebugCamPos.vy, (long)g_DebugCamPos.vz,
                g_DebugCamAngleY,
                (long)g_SysWork.playerWork_4C.player_0.position_18.vx,
                (long)g_SysWork.playerWork_4C.player_0.position_18.vy,
                (long)g_SysWork.playerWork_4C.player_0.position_18.vz);
            fflush(stderr);
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

        GsClearOt(0, 0, &g_OrderingTable0[g_ActiveBufferIdx]);
        GsClearOt(0, 0, &g_OrderingTable2[g_ActiveBufferIdx]);

        g_SysWork.sysFlags_22A0 = SysFlag_None;

        // Call update function for current GameState.
        g_GameStateUpdateFuncs[g_GameWork.gameState_594]();

        Demo_Update();
        Demo_GameRandSeedSet();

        if (MainLoop_ShouldWarmReset() == 2)
        {
            Game_WarmBoot();
            continue;
        }

        Screen_FadeUpdate();
        MemCard_Update();
        Sd_TaskPoolExecute();

        if (!Sd_AudioStreamingCheck())
        {
            Fs_QueueUpdate();
        }

        func_80089128();
        func_8008D78C(); // Camera update?
        DrawSync(SyncMode_Wait);
        // Handle V sync.
        if (g_SysWork.flags_22A4 & SysFlag2_1)
        {
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
                g_VBlanks     = VSync(SyncMode_Count) - g_PrevVBlanks;
                g_PrevVBlanks = VSync(SyncMode_Count);
                VSync(SyncMode_Wait);
            }
            else
            {
                if (!ScreenFade_IsNone())
                {
                    VSync(SyncMode_Wait);
                }

                g_VBlanks     = VSync(SyncMode_Count) - g_PrevVBlanks;
                g_PrevVBlanks = VSync(SyncMode_Count);

                while (g_VBlanks < g_IntervalVBlanks)
                {
                    VSync(SyncMode_Wait);
                    g_VBlanks++;
                    g_PrevVBlanks++;
                }
            }

            // Update V blanks.
            g_UncappedVBlanks = g_VBlanks;
            g_VBlanks         = MIN(g_VBlanks, V_BLANKS_MAX);

            // Update V count.
            vCount     = MIN(GsGetVcount(), H_BLANKS_PER_FRAME_MIN); // NOTE: Will call `GsGetVcount` twice.
            vCountCopy = vCount;
        }

        // Update delta time.
        g_DeltaTime    = Q12_MULT(vCount, H_BLANKS_Q12_TO_SEC_SCALE);
        g_DeltaTimeRaw = Q12_MULT(vCountCopy, H_BLANKS_Q12_TO_SEC_SCALE);
        g_GravitySpeed = Q12_MULT(vCount, H_BLANKS_GRAVITY_SCALE);
        GsClearVcount();

        // Draw objects?
        GsSwapDispBuff();
#ifdef SH_PC_PORT
        /* Override background color with fog color during InGame.
         * fog params are set by Gfx_FlashlightUpdate from the previous frame's
         * update, so they're valid by frame 2+. Use the normal GsSortClear path
         * which PsyCross handles via activeDrawEnv.isbg in PsyX_BeginScene. */
        if (g_GameWork.gameState_594 == 11 && PC_WorldEnvWork.isFogEnabled_1) {
            g_GameWork.background2dColor_58C.r = PC_WorldEnvWork.fogColor_1C.r;
            g_GameWork.background2dColor_58C.g = PC_WorldEnvWork.fogColor_1C.g;
            g_GameWork.background2dColor_58C.b = PC_WorldEnvWork.fogColor_1C.b;
        }
#endif
        GsSortClear(g_GameWork.background2dColor_58C.r, g_GameWork.background2dColor_58C.g, g_GameWork.background2dColor_58C.b, &g_OrderingTable0[g_ActiveBufferIdx]);
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
                    cur = (OT_TAG*)nextPrim(cur);
                    w2++;
                }
            }
        }

#endif
        GsDrawOt(&g_OrderingTable0[g_ActiveBufferIdx]);
#ifdef SH_PC_PORT
        /* Always sanitize OT2 — subsystems like flashlight and particles
         * may add garbage prims that crash PsyCross's primitive parser */
        if (g_GameWork.gameState_594 == 11) {
            GsClearOt(0, 0, &g_OrderingTable2[g_ActiveBufferIdx]);
        }
#endif
        GsDrawOt(&g_OrderingTable2[g_ActiveBufferIdx]);
#ifdef SH_PC_PORT
        DebugConsole_Render();
        PsyX_EndScene();
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
