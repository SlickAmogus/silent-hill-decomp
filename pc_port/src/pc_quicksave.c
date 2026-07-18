/* Quick Save / Quick Load keys (default F6/F8; rebindable via config
 * key_quicksave/key_quickload). Always active — not gated behind
 * allow_debug_controls. Opens the original save/load screens through the
 * same sequence the in-game save point (game_sys_states.c) and inventory
 * save path (item_screens_2.c case 24) use, so screen assets are loaded
 * before the state switch. */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_draw.h"
#include "main/fsqueue.h"
#include "sh_log.h"
#include "pc_config.h"
#include "pc_rando.h"

#include <SDL.h>

void Pc_QuickSaveLoadUpdate(void)
{
    static SDL_Scancode scSave   = SDL_SCANCODE_UNKNOWN;
    static SDL_Scancode scLoad   = SDL_SCANCODE_UNKNOWN;
    static int          resolved = 0;
    static int          prevSave = 0;
    static int          prevLoad = 0;

    extern int g_PcConsoleInputActive;

    const Uint8* keys;
    int          curSave;
    int          curLoad;

    if (!resolved) {
        scSave   = SDL_GetScancodeFromName(g_PcConfig.keyQuickSave);
        scLoad   = SDL_GetScancodeFromName(g_PcConfig.keyQuickLoad);
        resolved = 1;
    }

    /* A randomizer run cannot be saved or reloaded. */
    if (Pc_Rando_Active())
        return;

    keys = SDL_GetKeyboardState(NULL);
    if (keys == NULL)
        return;

    curSave = (scSave != SDL_SCANCODE_UNKNOWN) ? keys[scSave] : 0;
    curLoad = (scLoad != SDL_SCANCODE_UNKNOWN) ? keys[scLoad] : 0;

    /* Gameplay only: not menus, not cutscenes/map events, not while the
     * console input line is open. */
    if (g_GameWork.gameState == GameState_InGame &&
        g_SysWork.sysState == SysState_Gameplay &&
        !g_PcConsoleInputActive)
    {
        if (curSave && !prevSave) {
            SH_DBG("[QUICK] opening save screen (%s)", g_PcConfig.keyQuickSave);
            SysWork_SavegameUpdatePlayer();
            Screen_Refresh(320, 0);
            GameFs_SaveLoadBinLoad();
            Fs_QueueWaitForEmpty();
            Game_StateSetNext(GameState_SaveScreen);
        } else if (curLoad && !prevLoad) {
            SH_DBG("[QUICK] opening load screen (%s)", g_PcConfig.keyQuickLoad);
            Screen_Refresh(320, 0);
            GameFs_SaveLoadBinLoad();
            Fs_QueueWaitForEmpty();
            Game_StateSetNext(GameState_LoadSavegameScreen);
        }
    }

    prevSave = curSave;
    prevLoad = curLoad;
}
