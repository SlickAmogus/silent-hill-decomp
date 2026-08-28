/* SPDX-License-Identifier: GPL-3.0-or-later */
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

/* A boss fight is the one place a quick save can strand a run: the save records
 * the player mid-arena, and reloading it drops them back into a fight they may
 * have entered with no health or ammo, with no way back out. The original game
 * has no save point inside a boss chamber for the same reason.
 *
 * Keyed on a LIVE boss actor rather than the map/room, so it covers every boss
 * without a hand-maintained room table, and lifts by itself the moment the boss
 * dies — the walk back out of the chamber stays saveable. Quick LOAD is left
 * alone: getting out is never the problem. */
static int Pc_QuickSave_BossActive(void)
{
    static const unsigned char s_bossCharas[] = {
        Chara_SplitHead,    /* school basement / sewer */
        Chara_Floatstinger, /* Central Silent Hill otherworld */
        Chara_Twinfeeler,   /* Old Silent Hill */
        Chara_Bloodsucker,  /* sewers */
        Chara_Incubus,      /* Nowhere, final */
        Chara_MonsterCybil, /* amusement park carousel */
        Chara_Incubator     /* Nowhere, final */
    };

    int i;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];
        int                   b;

        if (npc->health <= Q12(0.0f))
        {
            continue;
        }

        for (b = 0; b < (int)(sizeof(s_bossCharas) / sizeof(s_bossCharas[0])); b++)
        {
            if (npc->model.charaId == s_bossCharas[b])
            {
                return 1;
            }
        }
    }

    return 0;
}

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

    /* PsyCross owns Ctrl+<key> for its renderer diagnostics, and these binds
     * read the same raw key state, so a Ctrl shortcut on a key that is also
     * bound here would fire both. The edge state below still tracks the
     * physical key, so releasing Ctrl mid-hold cannot fake a new press. */
    if (SDL_GetModState() & KMOD_CTRL)
    {
        prevSave = curSave;
        prevLoad = curLoad;
        return;
    }

    /* Gameplay only: not menus, not cutscenes/map events, not while the
     * console input line is open. */
    if (g_GameWork.gameState == GameState_InGame &&
        g_SysWork.sysState == SysState_Gameplay &&
        !g_PcConsoleInputActive)
    {
        if (curSave && !prevSave && Pc_QuickSave_BossActive()) {
            SH_DBG_ECHO("Can't quick save during a boss fight");
        } else if (curSave && !prevSave) {
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
