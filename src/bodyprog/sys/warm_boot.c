#include "game.h"

#ifdef SH_PC_PORT
#include "sh_log.h"
#endif

#include <memory.h>
#include <psyq/libetc.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/demo.h"
#include "bodyprog/sys/joy.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/sys/game_main.h"
#include "bodyprog/math/math.h"
#include "bodyprog/sound/sound_system.h"

void SysWork_Clear(void) // 0x800340E0
{
    bzero(&g_SysWork, sizeof(s_SysWork));
}

#ifdef SH_PC_PORT
/* Named diagnostic, one line per warm reset (never per-frame): a reset that
 * nobody asked for drops the player to the title mid-game, and the log has to
 * say which of the four triggers fired. */
static s32 Pc_WarmResetLog(const char* cause)
{
    SH_DBG("[WARMRESET] cause=%s held=0x%04X timer=%d sysFlags=0x%X gameState=%d sysState=%d",
           cause, (unsigned int)(g_Controller0->heldBtnFlags & 0xFFFF), (int)g_WarmBootTimer,
           (unsigned int)g_SysWork.sysFlags, (int)g_GameWork.gameState, (int)g_SysWork.sysState);
    return ResetType_WarmBoot;
}
#define WARM_RESET(cause) return Pc_WarmResetLog(cause)
#else
#define WARM_RESET(cause) return ResetType_WarmBoot
#endif

s32 MainLoop_ShouldWarmReset(void) // 0x80034108
{
    #define WARM_BOOT_COMBO_HOLD      (ControllerFlag_Select | ControllerFlag_Start)
    #define WARM_BOOT_COMBO_PRESS     (ControllerFlag_Select | ControllerFlag_Start | \
                                       ControllerFlag_L2 | ControllerFlag_R2 | ControllerFlag_L1 | ControllerFlag_R1)
    #define WARM_BOOT_COMBO_PRESS_ALT (ControllerFlag_Start | ControllerFlag_Triangle | ControllerFlag_Square)

    if (g_GameWork.gameState < GameState_MovieIntroAlternate)
    {
        return ResetType_None;
    }

    if (g_GameWork.gameState == GameState_LoadSavegameScreen && g_GameWork.gameStateSteps[0] == 4)
    {
        return ResetType_None;
    }

    if (g_GameWork.gameState == GameState_SaveScreen &&
        (g_GameWork.gameStateSteps[0] == 2 || g_GameWork.gameStateSteps[0] == 3))
    {
        return ResetType_None;
    }

    if (g_SysWork.sysFlags & SysFlag_DemoActive)
    {
        if (g_Demo_FrameCount > (TICKS_PER_SECOND * 30))
        {
            WARM_RESET("demo-timeout");
        }
    }
    else
    {
        g_Demo_FrameCount = ResetType_None;
    }

    if (g_GameWork.gameState == GameState_MainMenu)
    {
        return ResetType_None;
    }

    // Reset frame counter if reset buttons not held.
    if ((g_Controller0->heldBtnFlags & WARM_BOOT_COMBO_HOLD) != WARM_BOOT_COMBO_HOLD)
    {
        g_WarmBootTimer = 0;
    }

    if (g_WarmBootTimer > (TICKS_PER_SECOND * 2))
    {
        WARM_RESET("select+start-2s");
    }
    else if (g_Controller0->heldBtnFlags == WARM_BOOT_COMBO_PRESS && (g_Controller0->clickedBtnFlags & WARM_BOOT_COMBO_PRESS))
    {
        WARM_RESET("combo");
    }
    else if (g_Controller0->heldBtnFlags == WARM_BOOT_COMBO_PRESS_ALT && (g_Controller0->clickedBtnFlags & ControllerFlag_Start))
    {
        WARM_RESET("combo-alt");
    }

#ifdef SH_PC_PORT
    if (g_SysWork.sysFlags & SysFlag_DoWarmReset)
    {
        WARM_RESET("flag");
    }

    return ResetType_None;
#else
    return (g_SysWork.sysFlags & SysFlag_DoWarmReset) ? ResetType_WarmBoot : ResetType_None;
#endif

    #undef WARM_BOOT_COMBO_HOLD
    #undef WARM_BOOT_COMBO_PRESS
    #undef WARM_BOOT_COMBO_PRESS_ALT
}

void Game_WarmBoot(void) // 0x80034264
{
    e_GameState prevState;

#ifdef SH_PC_PORT
    /* A warm reset can fire from any state that holds the PC freeze-frame:
     * pause, the map-screen messages ("I don't have a map" / "too dark") and
     * the item pickup all re-arm g_PsxPresentLastFrame every tick and only drop
     * it on their own exit path, which a reset never takes. Left set, nothing
     * else releases it -- gameplay is over -- and PsyX_BeginScene kept
     * re-presenting the captured gameplay frame over the clear, so the title
     * screen came up drawn on top of the last frame of play. */
    {
        extern int g_PsxPresentLastFrame;
        extern int g_PcFreezeReleasePending;
        g_PsxPresentLastFrame    = 0;
        g_PcFreezeReleasePending = 0;
    }
#endif

    DrawSync(SyncMode_Wait);
    Screen_RectInterlacedClear(0, 32, 512, 448, 0, 0, 0);
    func_800892A4(4);
    func_80089128();
    SD_Call(19);

    while (Sd_AudioStreamingCheck())
    {
        Sd_TaskPoolExecute();
        VSync(SyncMode_Wait);
    }

    SD_Call(20);

    while (Sd_AudioStreamingCheck())
    {
        Sd_TaskPoolExecute();
        VSync(SyncMode_Wait);
    }

    Fs_QueueReset();
    Fs_QueueWaitForEmpty();
    sd_work_init();
    Sd_AmbientSfxSet(1);

    while (Sd_AudioStreamingCheck())
    {
        Sd_TaskPoolExecute();
        VSync(SyncMode_Wait);
    }

#ifdef SH_PC_PORT
    /* The radio interference loop is a LOOPING sfx and nothing above stops it:
     * SD_Call(19/20), sd_work_init and Sd_AmbientSfxSet handle streaming and
     * ambient audio, while every screen that needs silence calls this
     * explicitly (item_screens_2, options, game_load, map5_s01). A warm boot out
     * of the attract demo therefore carried radio static onto the title screen
     * and into the new game that followed. Warm boot is the return-to-clean-
     * state path, so stop it once here rather than at each caller. */
    Game_RadioSoundStop();
#endif

    if (g_SysWork.sysFlags & SysFlag_DemoActive)
    {
        Demo_Stop();
    }

    SysWork_Clear();
    Demo_SequenceAdvance(1);
    Demo_DemoDataRead();
    GameFs_TitleGfxLoad();
    Fs_QueueWaitForEmpty();
    Joy_Update();

    prevState                = g_GameWork.gameState;
    g_GameWork.gameState = GameState_MainMenu;

    g_SysWork.counters_1C[0] = 0;
    g_SysWork.counters_1C[1] = 0;

    g_GameWork.gameStateSteps[1] = 0;
    g_GameWork.gameStateSteps[2] = 0;

    SysWork_StateSetNext(SysState_Gameplay);

    ScreenFade_Start(true, true, false);

    g_GameWork.gameStateSteps[0] = prevState;
    g_GameWork.gameStatePrev    = prevState;
    g_GameWork.gameStateSteps[0] = 0;

    g_ScreenFadeTimestep = Q12(0.0f);
}
