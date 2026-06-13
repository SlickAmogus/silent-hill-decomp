/* Interactive console command execution (see dbg_overlay.c for the input
 * mode itself). Commands arrive as a single uppercase line ("GIVE SHOTGUN");
 * output goes back through DbgOverlay_PushLine.
 *
 * Commands:
 *   HELP                 - command list with descriptions
 *   DEBUG [page]         - debug & cheat key reference (2 pages)
 *   QUIT                 - exit the game
 *   MAP                  - list all map names
 *   MAP <name>           - new-game warp to a map (mirrors title.c auto-start)
 *   GIVE <thing>         - HANDGUN / RIFLE / SHOTGUN / AMMO / HEALTH
 *   NOCLIP               - toggle walking through walls (player only)
 *   FMV                  - list all FMV names (numbered)
 *   FMV <name|number>    - play an FMV (fades out, plays, fades back in)
 *   FMV INTROn / ENDn    - alias for the nth intro (C*) / ending (Z*) movie
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/items.h"
#include "bodyprog/screen/screen_fade.h"
#include "sh_log.h"
#include "map_registry.h"
#include "dbg_overlay.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* fmv_player.cpp */
extern int         FMV_Play(int file_idx, int max_frames);
extern int         FMV_GetCount(void);
extern const char* FMV_GetName(int tableIdx);
extern int         FMV_GetFileIdx(int tableIdx);

/* Same toggle as debug key 0: player_control.c skips Collision_WallDetect and
 * substitutes the floor surface directly, so Harry keeps walking on ground. */
extern int g_DebugNoWallCollision;

static void cprintf(const char* fmt, ...)
{
    char line[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    DbgOverlay_PushLine(line);
}

static int inventory_has(s32 itemId)
{
    int i;
    for (i = 0; i < INV_ITEM_COUNT_MAX; i++) {
        if (g_SavegamePtr->items[i].id_0 == itemId)
            return 1;
    }
    return 0;
}

static void cmd_map(const char* arg)
{
    int i, count = MapRegistry_Count();

    if (!arg[0]) {
        char line[64];
        int  used = 0;
        line[0] = '\0';
        for (i = 0; i < count; i++) {
            const char* nm = MapRegistry_GetName(i);
            if (!nm) continue;
            if (used + (int)strlen(nm) + 1 >= 60) {
                DbgOverlay_PushLine(line);
                line[0] = '\0';
                used    = 0;
            }
            used += snprintf(line + used, sizeof(line) - used, "%s%s", used ? " " : "", nm);
        }
        if (used) DbgOverlay_PushLine(line);
        return;
    }

    {
        /* Registry names are lowercase; console input is uppercase. */
        char lower[32];
        int  mapId;
        for (i = 0; arg[i] && i < (int)sizeof(lower) - 1; i++)
            lower[i] = (char)tolower((unsigned char)arg[i]);
        lower[i] = '\0';

        mapId = MapRegistry_FindByName(lower);
        if (mapId < 0) {
            cprintf("Unknown map: %s", lower);
            return;
        }

        /* New-game warp — same recipe as title.c's config-map auto-start. */
        cprintf("Warping to %s...", lower);
        GameBoot_SavegameInitialize(mapId, 0); /* Normal difficulty */
        GameBoot_PlayerInit();
        g_SysWork.processFlags = ProcessFlag_NewGame;
        GameBoot_MapLoad(g_SavegamePtr->mapIdx);
        GameFs_StreamBinLoad();
        Fs_QueueWaitForEmpty();
        Chara_PositionSet(&g_MapOverlayHdr.mapPoints[0]);
        g_SysWork.counters_1C[0]     = 0;
        g_SysWork.counters_1C[1]     = 0;
        g_GameWork.gameStateSteps[0] = 0;
        g_GameWork.gameStateSteps[1] = 0;
        g_GameWork.gameStateSteps[2] = 0;
        SysWork_StateSetNext(SysState_Gameplay);
    }
}

static void cmd_give(const char* arg)
{
    if (strcmp(arg, "HANDGUN") == 0) {
        if (!inventory_has(InvItemId_Handgun))
            Inventory_AddSpecialItem(InvItemId_Handgun, 1);
        Inventory_AddSpecialItem(InvItemId_HandgunBullets, 15);
        cprintf("Given handgun + 15 bullets");
    } else if (strcmp(arg, "RIFLE") == 0) {
        if (!inventory_has(InvItemId_HuntingRifle))
            Inventory_AddSpecialItem(InvItemId_HuntingRifle, 1);
        Inventory_AddSpecialItem(InvItemId_RifleShells, 30);
        cprintf("Given rifle + 30 shells");
    } else if (strcmp(arg, "SHOTGUN") == 0) {
        if (!inventory_has(InvItemId_Shotgun))
            Inventory_AddSpecialItem(InvItemId_Shotgun, 1);
        Inventory_AddSpecialItem(InvItemId_ShotgunShells, 30);
        cprintf("Given shotgun + 30 shells");
    } else if (strcmp(arg, "AMMO") == 0) {
        Inventory_AddSpecialItem(InvItemId_HandgunBullets, 30);
        Inventory_AddSpecialItem(InvItemId_RifleShells, 30);
        Inventory_AddSpecialItem(InvItemId_ShotgunShells, 30);
        cprintf("Given 30 of each ammo");
    } else if (strcmp(arg, "HEALTH") == 0) {
        g_SysWork.playerWork.player.health = Q12(100.0f);
        cprintf("Health restored");
    } else {
        cprintf("give what? HANDGUN RIFLE SHOTGUN AMMO HEALTH");
    }
}

/* help / debug reference pages. The in-game console viewport shows
 * MAX_CONSOLE (20) lines and each line buffer is LINE_LEN (64) chars, so
 * pages stay under ~16 lines of <=63 chars and longer lists split into
 * numbered pages ("debug 2"). */
static const char* const HELP_LINES[] = {
    "Commands:",
    " help [n]       command list",
    " debug [n]      debug & cheat key reference",
    " quit           exit the game",
    " map            list all map names",
    " map <name>     new-game warp to a map",
    " give <thing>   HANDGUN RIFLE SHOTGUN AMMO HEALTH",
    " noclip         walk through walls (floor stays on)",
    " fmv            list movies (numbered)",
    " fmv <name|#>   play a movie (also intro1-2, end1-5)",
    "Quick Save: F6   Quick Load: F8 (work outside console)",
};

static const char* const DEBUG_PAGE1[] = {
    "Debug keys (page 1/2) - cheats & tools:",
    " Esc     warm reset to the title screen",
    " 0       noclip toggle (walk through walls)",
    " 1       kill Harry",
    " 4 / 5   map config prev / next (loads on New Game)",
    " 6       spawn Grey Child in front of Harry",
    " 7       invincibility toggle",
    " 8       +15 handgun bullets",
    " 9       no-target toggle (enemies ignore Harry)",
    " -       give Hunting Rifle + 30 shells",
    " =       give Shotgun + 30 shells",
    " '       collision visualizer panel",
    " [ / ]   drop A/B position markers into the log",
    " ~       tap: console open/close, hold: command input",
    "type DEBUG 2 for the camera keys",
};

static const char* const DEBUG_PAGE2[] = {
    "Debug keys (page 2/2) - camera:",
    " Num *        free debug camera on/off",
    " Num 2        third-person chase cam (mouse look)",
    " Num 8/5/4/6  fly forward / back / strafe left / right",
    " Num 7 / 9    turn left / right",
    " Num + / -    tilt up / down",
    " PgUp / PgDn  move up / down",
    " Num /        print camera coordinates to the log",
    " (with debug cam OFF the same numpad keys nudge the",
    "  normal game camera - live camera tuning aid)",
    " Num 3        reset cam nudge / in-game rescue teleport",
    " Num 0        raw cam mode (zero all nudges)",
    " Num .        log Harry position (+fog toggle in cam)",
};

static void push_lines(const char* const* lines, int count)
{
    int i;
    for (i = 0; i < count; i++)
        DbgOverlay_PushLine(lines[i]);
}

/* FMV start is deferred so the screen can fade to black first, like the
 * game's own movie transitions: cmd_fmv arms the pending index and starts a
 * fade-out; Pc_ConsoleFmvUpdate (called every frame from MainLoop) blocks in
 * FMV_Play once the fade lands, then fades back in. */
static int s_pendingFmvFileIdx = -1;

void Pc_ConsoleFmvUpdate(void)
{
    int fileIdx;

    if (s_pendingFmvFileIdx < 0 || !ScreenFade_IsFinished())
        return;

    fileIdx             = s_pendingFmvFileIdx;
    s_pendingFmvFileIdx = -1;
    FMV_Play(fileIdx, 0);
    ScreenFade_Start(true, true, false);
}

static void cmd_fmv(const char* arg)
{
    int i, count = FMV_GetCount();
    int pick = -1;

    if (!arg[0]) {
        char line[64];
        int  used = 0;
        line[0] = '\0';
        for (i = 0; i < count; i++) {
            char entry[24];
            snprintf(entry, sizeof(entry), "%d=%s", i + 1, FMV_GetName(i));
            if (used + (int)strlen(entry) + 1 >= 60) {
                DbgOverlay_PushLine(line);
                line[0] = '\0';
                used    = 0;
            }
            used += snprintf(line + used, sizeof(line) - used, "%s%s", used ? " " : "", entry);
        }
        if (used) DbgOverlay_PushLine(line);
        DbgOverlay_PushLine("also: fmv <number>, intro1-2, end1-5");
        return;
    }

    /* Plain number: 1-based position in the list above. */
    {
        int digits = 1;
        for (i = 0; arg[i]; i++) {
            if (!isdigit((unsigned char)arg[i])) {
                digits = 0;
                break;
            }
        }
        if (digits)
            pick = atoi(arg) - 1;
    }

    /* INTROn / ENDn aliases: nth movie whose filename starts with C (the
     * intros) or Z (the endings block), in disc order. */
    if (pick < 0 && (strncmp(arg, "INTRO", 5) == 0 || strncmp(arg, "END", 3) == 0)) {
        char lead = (arg[0] == 'I') ? 'C' : 'Z';
        int  n    = atoi(arg + ((lead == 'C') ? 5 : 3));
        int  seen = 0;

        for (i = 0; i < count && pick < 0; i++) {
            if (FMV_GetName(i)[0] == lead && ++seen == n)
                pick = i;
        }
        if (pick < 0) {
            cprintf("No such %s", (lead == 'C') ? "intro" : "ending");
            return;
        }
    }

    /* Full filename. */
    if (pick < 0) {
        for (i = 0; i < count; i++) {
            if (strcmp(arg, FMV_GetName(i)) == 0) {
                pick = i;
                break;
            }
        }
    }

    if (pick < 0 || pick >= count) {
        cprintf("Unknown FMV: %s (try 'fmv' to list)", arg);
        return;
    }

    cprintf("Playing %s...", FMV_GetName(pick));
    s_pendingFmvFileIdx = FMV_GetFileIdx(pick);
    ScreenFade_Start(true, false, false);
}

/* line is the uppercase console input ('_' typed via the - key). */
void Pc_ConsoleExec(const char* line)
{
    char cmd[48];
    const char* arg;
    int i;

    /* Split first word / remainder. */
    for (i = 0; line[i] && line[i] != ' ' && i < (int)sizeof(cmd) - 1; i++)
        cmd[i] = line[i];
    cmd[i] = '\0';
    arg = line[i] ? line + i + 1 : line + i;
    while (*arg == ' ') arg++;

    if (cmd[0] == '\0') {
        return;
    } else if (strcmp(cmd, "QUIT") == 0) {
        SH_DBG("[CONSOLE] quit");
        exit(0);
    } else if (strcmp(cmd, "HELP") == 0) {
        push_lines(HELP_LINES, (int)(sizeof(HELP_LINES) / sizeof(HELP_LINES[0])));
    } else if (strcmp(cmd, "DEBUG") == 0) {
        if (strcmp(arg, "2") == 0)
            push_lines(DEBUG_PAGE2, (int)(sizeof(DEBUG_PAGE2) / sizeof(DEBUG_PAGE2[0])));
        else
            push_lines(DEBUG_PAGE1, (int)(sizeof(DEBUG_PAGE1) / sizeof(DEBUG_PAGE1[0])));
    } else if (strcmp(cmd, "MAP") == 0) {
        cmd_map(arg);
    } else if (strcmp(cmd, "GIVE") == 0) {
        cmd_give(arg);
    } else if (strcmp(cmd, "NOCLIP") == 0) {
        g_DebugNoWallCollision = !g_DebugNoWallCollision;
        cprintf("noclip %s", g_DebugNoWallCollision ? "ON" : "OFF");
    } else if (strcmp(cmd, "FMV") == 0) {
        cmd_fmv(arg);
    } else if (strcmp(cmd, "PGXP") == 0) {
        extern int g_PsxUsePgxp;
        if (arg[0] == '1') g_PsxUsePgxp = 1;
        else if (arg[0] == '0') g_PsxUsePgxp = 0;
        else g_PsxUsePgxp = !g_PsxUsePgxp; /* bare "pgxp" toggles */
        cprintf("PGXP %s (perspective-correct, WIP)", g_PsxUsePgxp ? "ON" : "OFF");
    } else if (strcmp(cmd, "ADSR") == 0) {
        extern void PsyX_SPUAL_SetAdsrEnabled(int on);
        extern int  PsyX_SPUAL_GetAdsrEnabled(void);
        int on = (arg[0] == '1') ? 1 : (arg[0] == '0') ? 0 : !PsyX_SPUAL_GetAdsrEnabled();
        PsyX_SPUAL_SetAdsrEnabled(on);
        cprintf("ADSR envelope %s (looping-SFX ring-out, WIP)", on ? "ON" : "OFF");
    } else {
        DbgOverlay_PushLine("Command not found!");
    }
}
