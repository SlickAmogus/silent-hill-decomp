#include "game.h"
#ifdef SH_PC_PORT
#include "sh_log.h"
#include <stdio.h>
#endif

#include <psyq/libetc.h>
#include <psyq/libpad.h>
#include <psyq/strings.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/game_boot/fs_chara_anim.h"
#include "bodyprog/demo.h"
#include "bodyprog/events/bgm.h"
#include "bodyprog/events/radio.h"
#include "bodyprog/game_boot/game_boot.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/math/math.h"
#include "bodyprog/memcard.h"
#include "bodyprog/player.h"
#include "bodyprog/ranking.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/text/text_draw.h"
#include "main/fsqueue.h"
#include "main/mem.h"
#include "main/rng.h"
#include "screens/stream/stream.h"

#ifdef SH_PC_PORT
static void GameBoot_LoadingScreen(void);
#endif

static inline void Game_StateStepIncrement(void) // TODO: Move to header?
{
    s32 gameStateSteps0 = g_GameWork.gameStateSteps[0];

    g_SysWork.counters_1C[1]        = 0;
    g_GameWork.gameStateSteps[1] = 0;
    g_GameWork.gameStateSteps[2] = 0;
    g_GameWork.gameStateSteps[0] = gameStateSteps0 + 1;
}

void Anim_CharaTypeAnimInfoClear(void) // 0x800348C0
{
    bzero(&g_CharaTypeAnimInfo[1], 72);
}

void GameState_LoadMapScreen_Update(void) // 0x800348E8
{
#ifdef SH_PC_PORT
#endif
    GameBoot_LoadingScreen();
#ifdef SH_PC_PORT
#endif
    GameBoot_GameStartup();
#ifdef SH_PC_PORT
#endif

    if (g_SysWork.sysFlags & SysFlag_LoadActive)
    {
        D_800BCDD4++;

        if (D_800BCDD4 >= 21)
        {
            g_SysWork.sysFlags &= ~SysFlag_LoadActive;

            SD_Call(Sfx_Unk1502);
            SD_Call(Sfx_Unk1501);
        }
    }
}

void GameBoot_GameStartup(void) // 0x80034964
{
    // It makes up to 5 attemps. If the load fails, it restarts
    // the entire process by restarting the timer used to check if a demo
    // should be triggered.
    static s32 demoLoadAttempCount;

    switch (g_GameWork.gameStateSteps[0])
    {
        case 0:
#ifdef SH_PC_PORT
            SH_DBG("[TRANSITION] GameStartup step=0: processFlags=0x%X (%s) sizeof(s_WorldGfxWork)=%zu",
                   g_SysWork.processFlags,
                   (g_SysWork.processFlags == ProcessFlag_RoomTransition) ? "RoomTransition" :
                   (g_SysWork.processFlags == ProcessFlag_OverlayTransition) ? "OverlayTransition" :
                   (g_SysWork.processFlags == ProcessFlag_BootDemo) ? "BootDemo" :
                   (g_SysWork.processFlags == ProcessFlag_LoadSave) ? "LoadSave" :
                   (g_SysWork.processFlags == ProcessFlag_Continue) ? "Continue" : "Unknown",
                   sizeof(s_WorldGfxWork));
#endif
            g_IntervalVBlanks                  = 1;
            g_GameWork.background2dColor.r = 0;
            g_GameWork.background2dColor.g = 0;
            g_GameWork.background2dColor.b = 0;

            if (g_SysWork.processFlags == ProcessFlag_RoomTransition)
            {
                AreaLoad_UpdatePlayerPosition();
                g_GameWork.gameStateSteps[0] = 7;
            }
            else if (g_SysWork.processFlags == ProcessFlag_BootDemo)
            {
                demoLoadAttempCount             = 0;
                g_GameWork.gameStateSteps[0] = 1;
                g_SysWork.counters_1C[1]        = 1;
            }
            else
            {
                g_GameWork.gameStateSteps[0] = 3;
            }

            SD_Call(19);
            break;

        case 1:
            if (g_SysWork.counters_1C[1] > 1200 && Fs_QueueGetLength() == 0 && !Sd_AudioStreamingCheck())
            {
                Demo_DemoFileSavegameUpdate();
                GameBoot_PlayerInit();

                if (Demo_PlayFileBufferSetup() != 0)
                {
                    GameBoot_MapLoad(g_SavegamePtr->mapIdx);

                    g_GameWork.gameStateSteps[0] = 2;
                    g_SysWork.counters_1C[1]              = 0;
                    g_GameWork.gameStateSteps[1] = 0;
                    g_GameWork.gameStateSteps[2] = 0;
                    break;
                }

                Demo_SequenceAdvance(1);
                Demo_DemoDataRead();

                demoLoadAttempCount++;
                if (demoLoadAttempCount >= 5)
                {
                    demoLoadAttempCount = 0;
                    g_SysWork.counters_1C[1]  = 0;
                    break;
                }
            }
            break;

        case 2:
            if (Fs_QueueGetLength() == 0 && !Sd_AudioStreamingCheck())
            {
                Demo_PlayDataRead();

                g_GameWork.gameStateSteps[0] = 3;
                g_SysWork.counters_1C[1]              = 0;
                g_GameWork.gameStateSteps[1] = 0;
                g_GameWork.gameStateSteps[2] = 0;
            }
            break;

        case 3:
#ifdef SH_PC_PORT
            /* Dump pending queue entries every 60 frames so we can identify
             * which file load is stalling during room/map transitions. The
             * door-transition "crash" is actually a hang here at queueLen>0;
             * post-load is failing on at least one entry. */
            {
                static s32 _step3Dumped = -1;
                s32 _qlen = Fs_QueueGetLength();
                if (_qlen > 0 && _step3Dumped != g_FsQueue.read.idx) {
                    s_FsQueueEntry* re = g_FsQueue.read.ptr;
                    s_FsQueueEntry* pe = g_FsQueue.postLoad.ptr;
                    _step3Dumped = g_FsQueue.read.idx;
                }
            }
#endif
            if (Fs_QueueGetLength() == 0)
            {
                g_GameWork.gameStateSteps[0] = 4;
            }
            break;

        case 4:
            if (g_SysWork.processFlags == ProcessFlag_OverlayTransition)
            {
                AreaLoad_UpdatePlayerPosition();
            }
            else if (g_SysWork.processFlags == ProcessFlag_LoadSave ||
                     g_SysWork.processFlags == ProcessFlag_Continue)
            {
                g_SysWork.loadingScreenIdx = LoadingScreenId_PlayerRun;
            }

            g_GameWork.gameStateSteps[0]++;
            break;

        case 5:
#ifdef SH_PC_PORT
#endif
            Fs_CharaAnimDataAlloc(1, g_MapOverlayHdr.charaGroupIds[0], NULL, 0);
            Fs_CharaAnimDataAlloc(2, g_MapOverlayHdr.charaGroupIds[1], NULL, 0);
            Fs_CharaAnimDataAlloc(3, g_MapOverlayHdr.charaGroupIds[2], NULL, 0);
            WorldGfx_MapInitCharaLoad(&g_MapOverlayHdr);

            g_GameWork.gameStateSteps[0]++;

        case 6:
            if (Fs_QueueGetLength() == 0)
            {
                g_GameWork.gameStateSteps[0]++;
            }
            break;

        case 7:
#ifdef SH_PC_PORT
            SH_DBG("[TRANSITION] GameStartup step=7: processFlags=0x%X playerPos=(%d,%d,%d) func_80039F90=0x%X",
                   g_SysWork.processFlags,
                   g_SysWork.playerWork.player.position.vx,
                   g_SysWork.playerWork.player.position.vy,
                   g_SysWork.playerWork.player.position.vz,
                   func_80039F90());
#endif
            if (func_80039F90() & EventParamUnkState_0)
            {
                Map_WorldClear();
            }

            Ipd_PlayerChunkInit(&g_MapOverlayHdr, g_SysWork.playerWork.player.position.vx, g_SysWork.playerWork.player.position.vz);
#ifdef SH_PC_PORT
            SH_DBG("[TRANSITION] GameStartup step=7 post-init: playerPos=(%d,%d,%d) mapTag='%.4s'",
                   g_SysWork.playerWork.player.position.vx,
                   g_SysWork.playerWork.player.position.vy,
                   g_SysWork.playerWork.player.position.vz,
                   g_MapOverlayHdr.mapInfo ? g_MapOverlayHdr.mapInfo->tag : "NULL");
#endif
            if (g_SysWork.processFlags == ProcessFlag_OverlayTransition)
            {
                Game_RadioSoundStop();
            }

            g_GameWork.gameStateSteps[0]++;

        case 8:
#ifdef SH_PC_PORT
            /* Flush the FS queue to complete pending reads. On PC, CdRead is
             * synchronous via PsyCross so each Fs_QueueUpdate() call completes
             * one state transition instantly. A few hundred iterations is enough
             * to drain all pending chunk reads. */
            {
                int flushCount = 0;
                while (Fs_QueueGetLength() > 0 && flushCount < 500)
                {
                    Fs_QueueUpdate();
                    flushCount++;
                }
            }
#endif
            if (Ipd_ChunkInitCheck() != false)
            {
                Game_StateStepIncrement();
            }
#ifdef SH_PC_PORT
            else
            {
                /* Safety: if chunks don't load after many frames, skip anyway
                 * to avoid hanging on the loading screen forever. */
                static int step8_frames = 0;
                step8_frames++;
                if (step8_frames > 120)
                {
                    SH_DBG("[SH] step=8: chunk wait timed out after %d frames, continuing", step8_frames);
                    step8_frames = 0;
                    Game_StateStepIncrement();
                }
            }
#endif
            break;

        case 9:
#ifdef SH_PC_PORT
            {
                /* Log actual return + state every 60 frames so we can see why
                 * Bgm_Init is failing to return 0 during scene transitions. */
                static int bgm_frame_counter = 0;
                s32 bgmResult = Bgm_Init();
                bgm_frame_counter++;
                if (bgm_frame_counter == 1 || bgm_frame_counter % 60 == 0) {
                    extern u16 Sd_GetXaAudioIdx(void);
                }
                if (bgmResult == 0)
                {
                    bgm_frame_counter = 0;
                    g_GameWork.gameState = GameState_MainLoadScreen;
                    Game_StateStepIncrement();
                }
                /* Watchdog: after 5 sec stuck, force-clear xaAudioIdx_4 and bgmStep
                 * so the load screen doesn't hang forever from a stale audio state. */
                if (bgm_frame_counter > 300) {
                    extern u16 Sd_GetXaAudioIdx(void);
                    extern void Sd_ForceClearXaAudioIdx(void);
                    SH_DBG("[SH] step9 WATCHDOG: forcing clear (xaIdx %d -> 0, bgmStep %d -> 0)",
                           (int)Sd_GetXaAudioIdx(), (int)g_GameWork.gameStateSteps[1]);
                    Sd_ForceClearXaAudioIdx();
                    g_GameWork.gameStateSteps[1] = 0;
                    bgm_frame_counter = 0;
                }
            }
#else
            if (Bgm_Init() == 0)
            {
                g_GameWork.gameState = GameState_MainLoadScreen;
                Game_StateStepIncrement();
            }
#endif
            break;

        case 10:
            if (g_SysWork.processFlags == ProcessFlag_BootDemo && !(g_SysWork.sysFlags & SysFlag_DemoActive))
            {
                Demo_Start();
                g_SysWork.sysFlags |= SysFlag_DemoActive;
            }

            if (func_80039F90() & EventParamUnkState_2 || Sd_AmbientSfxInit() == 0)
            {
                Game_StateStepIncrement();
            }
            break;

        case 11:
#ifdef SH_PC_PORT
#endif
            if (g_SysWork.counters_1C[0] >= 60)
            {
                if (g_SysWork.processFlags == ProcessFlag_RoomTransition)
                {
                    GameBoot_NpcInit();
                }
                else
                {
                    GameBoot_InGameInit();
                }

                if (g_SysWork.processFlags <= (u32)ProcessFlag_OverlayTransition)
                {
                    AreaLoad_TransitionSound();
                }

                MemCard_SysDisable();
                g_GameWork.gameStateSteps[0]++;
            }
            break;

        case 12:
#ifdef SH_PC_PORT
            {
                Game_StateSetNext(GameState_InGame);
#else
            if (!Sd_AudioStreamingCheck())
            {
                Game_StateSetNext(GameState_InGame);
#endif

                if (func_80039F90() & EventParamUnkState_1)
                {
                    g_GameWork.gameStateSteps[0] = 1;
                    g_Screen_FadeStatus             = SCREEN_FADE_STATUS(ScreenFadeState_ResetTimestep, IS_SCREEN_FADE_WHITE(g_Screen_FadeStatus));
                }
            }
            break;

        default:
            break;
    }
}

/** @brief Initalizes drawing of a loading screen. */
static void GameBoot_LoadingScreen(void) // 0x80034E58
{
#ifdef SH_PC_PORT
#endif
    if (g_SysWork.loadingScreenIdx != LoadingScreenId_None && g_GameWork.gameStateSteps[0] < 10)
    {
        ScreenFade_Start(false, true, false);
        g_ScreenFadeTimestep = Q12(0.8f);
#ifdef SH_PC_PORT
#endif
        g_MapOverlayHdr.loadingScreenFuncs[g_SysWork.loadingScreenIdx]();
    }

#ifdef SH_PC_PORT
#endif
    Screen_BackgroundMotionBlur(SyncMode_Wait2);
#ifdef SH_PC_PORT
#endif
}
