/* Interactive console command execution (see dbg_overlay.c for the input
 * mode itself). Commands arrive as a single uppercase line ("GIVE SHOTGUN");
 * output goes back through DbgOverlay_PushLine.
 *
 * Commands:
 *   HELP                 - list commands
 *   QUIT                 - exit the game
 *   MAP                  - list all map names
 *   MAP <name>           - new-game warp to a map (mirrors title.c auto-start)
 *   GIVE <thing>         - HANDGUN / RIFLE / SHOTGUN / AMMO / HEALTH
 *   NOCLIP               - toggle walking through walls (player only)
 *   FMV                  - list all FMV names
 *   FMV <name>           - play an FMV
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/items.h"
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

static void cmd_fmv(const char* arg)
{
    int i, count = FMV_GetCount();

    if (!arg[0]) {
        char line[64];
        int  used = 0;
        line[0] = '\0';
        for (i = 0; i < count; i++) {
            const char* nm = FMV_GetName(i);
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

    for (i = 0; i < count; i++) {
        if (strcmp(arg, FMV_GetName(i)) == 0) {
            cprintf("Playing %s...", arg);
            FMV_Play(FMV_GetFileIdx(i), 0);
            return;
        }
    }
    cprintf("Unknown FMV: %s (try 'fmv' to list)", arg);
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
        DbgOverlay_PushLine("quit map [name] give <thing> noclip fmv [name]");
    } else if (strcmp(cmd, "MAP") == 0) {
        cmd_map(arg);
    } else if (strcmp(cmd, "GIVE") == 0) {
        cmd_give(arg);
    } else if (strcmp(cmd, "NOCLIP") == 0) {
        g_DebugNoWallCollision = !g_DebugNoWallCollision;
        cprintf("noclip %s", g_DebugNoWallCollision ? "ON" : "OFF");
    } else if (strcmp(cmd, "FMV") == 0) {
        cmd_fmv(arg);
    } else {
        DbgOverlay_PushLine("Command not found!");
    }
}
