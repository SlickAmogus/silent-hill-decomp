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
#include "bodyprog/game_boot/game_boot.h"
#include "bodyprog/events/player_pos_update.h"
#include "bodyprog/items.h"
#include "bodyprog/savegame.h"
#include "bodyprog/item_screens.h" /* GameEndingFlag_Ufo (HyperBlaster give-unlock) */
#include "bodyprog/screen/screen_fade.h"
#include "sh_log.h"
#include "map_registry.h"
#include "dbg_overlay.h"
#include "pc_config.h"

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

void Pc_ConsoleApplyPendingFlags(void); /* public: re-apply console-set flags after a savegame reset (cmd_map + New Game boot) */

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

        /* Don't warp mid-session — just set the map config value so the next New Game
         * starts on this map (same effect as the 4/5 debug keys). */
        (void)mapId;
        strncpy(g_PcConfig.mapName, lower, sizeof(g_PcConfig.mapName) - 1);
        g_PcConfig.mapName[sizeof(g_PcConfig.mapName) - 1] = '\0';
        cprintf("map config set to %s (loads on New Game)", lower);
    }
}

/* name -> inventory item. Guns additionally grant ammo (see cmd_give). Ammo and
 * recovery items stack (always add `count`); unique items are only added when not
 * already held, to avoid inventory duplicates. */
typedef struct { const char* name; u8 id; u8 count; } s_GiveItem;
static const s_GiveItem GIVE_ITEMS[] = {
    /* melee weapons */
    { "KNIFE",        InvItemId_KitchenKnife,   1 },
    { "PIPE",         InvItemId_SteelPipe,      1 },
    { "ROCKDRILL",    InvItemId_RockDrill,      1 },
    { "HAMMER",       InvItemId_Hammer,         1 },
    { "CHAINSAW",     InvItemId_Chainsaw,       1 },
    { "KATANA",       InvItemId_Katana,         1 },
    { "AXE",          InvItemId_Axe,            1 },
    /* firearms (ammo added in cmd_give) */
    { "HANDGUN",      InvItemId_Handgun,        1 },
    { "RIFLE",        InvItemId_HuntingRifle,   1 },
    { "SHOTGUN",      InvItemId_Shotgun,        1 },
    { "HYPERBLASTER", InvItemId_HyperBlaster,   1 },
    /* ammo */
    { "HANDGUNAMMO",  InvItemId_HandgunBullets, 30 },
    { "RIFLEAMMO",    InvItemId_RifleShells,    30 },
    { "SHOTGUNAMMO",  InvItemId_ShotgunShells,  30 },
    { "GASOLINE",     InvItemId_GasolineTank,    5 }, /* chainsaw / rock drill fuel */
    { "GAS",          InvItemId_GasolineTank,    5 },
    /* recovery */
    { "HEALTHDRINK",  InvItemId_HealthDrink,    1 },
    { "FIRSTAID",     InvItemId_FirstAidKit,    1 },
    { "AMPOULE",      InvItemId_Ampoule,        1 },
    /* story / ending items */
    { "FLAUROS",         InvItemId_Flauros,          1 },
    { "CHANNELINGSTONE", InvItemId_ChannelingStone,  1 },
    { "PLASTICBOTTLE",   InvItemId_PlasticBottle,    1 },
    { "AGLAOPHOTIS",     InvItemId_UnknownLiquid,    1 },
    { "KAUFMANNKEY",     InvItemId_KaufmannKey,      1 },
    { "RINGOFCONTRACT",  InvItemId_RingOfContract,   1 },
    { "STONEOFTIME",     InvItemId_StoneOfTime,      1 },
    { "AMULET",          InvItemId_AmuletOfSolomon,  1 },
    { "CRESTOFMERCURY",  InvItemId_CrestOfMercury,   1 },
    { "ANKH",            InvItemId_Ankh,             1 },
    { "DAGGER",          InvItemId_DaggerOfMelchior, 1 },
    { "DISK",            InvItemId_DiskOfOuroboros,  1 },
    { "GOLDMEDALLION",   InvItemId_GoldMedallion,    1 },
    { "SILVERMEDALLION", InvItemId_SilverMedallion,  1 },
    { "LIGHTER",         InvItemId_Lighter,          1 },
    { "VIDEOTAPE",       InvItemId_VideoTape,        1 },
    { "CAMERA",          InvItemId_Camera,           1 },
    { "CHEMICAL",        InvItemId_Chemical,         1 },
    { "BLOODPACK",       InvItemId_BloodPack,        1 },
};
#define N_GIVE_ITEMS ((int)(sizeof(GIVE_ITEMS) / sizeof(GIVE_ITEMS[0])))

static void give_item(u8 id, u8 count)
{
    int stackable = (id >= InvItemId_HealthDrink && id <= InvItemId_Ampoule) ||
                    (id >= InvItemId_HandgunBullets);
    if (stackable || !inventory_has(id))
        Inventory_AddSpecialItem(id, count ? count : 1);
}

static void cmd_give(const char* arg)
{
    int k;

    if (arg[0] == '\0') {
        cprintf("give <item> - see 'help give' for the list");
        return;
    }
    if (strcmp(arg, "HEALTH") == 0) {
        g_SysWork.playerWork.player.health = Q12(100.0f);
        cprintf("Health restored");
        return;
    }
    if (strcmp(arg, "AMMO") == 0) {
        give_item(InvItemId_HandgunBullets, 30);
        give_item(InvItemId_RifleShells, 30);
        give_item(InvItemId_ShotgunShells, 30);
        cprintf("Given 30 of each ammo");
        return;
    }
    if (strcmp(arg, "ALLWEAPONS") == 0) {
        for (k = 0; k < N_GIVE_ITEMS; k++)
            if (GIVE_ITEMS[k].id >= InvItemId_KitchenKnife &&
                GIVE_ITEMS[k].id <= InvItemId_HyperBlaster)
                give_item(GIVE_ITEMS[k].id, 1);
        give_item(InvItemId_HandgunBullets, 60);
        give_item(InvItemId_RifleShells, 60);
        give_item(InvItemId_ShotgunShells, 60);
        give_item(InvItemId_GasolineTank, 5); /* chainsaw / drill fuel */
        g_SavegamePtr->clearGameEndings |= GameEndingFlag_Ufo; /* unlock HyperBlaster fire gate */
        cprintf("Given all weapons + ammo + gas");
        return;
    }
    for (k = 0; k < N_GIVE_ITEMS; k++) {
        if (strcmp(arg, GIVE_ITEMS[k].name) == 0) {
            give_item(GIVE_ITEMS[k].id, GIVE_ITEMS[k].count);
            if (GIVE_ITEMS[k].id == InvItemId_Handgun)           give_item(InvItemId_HandgunBullets, 15);
            else if (GIVE_ITEMS[k].id == InvItemId_HuntingRifle) give_item(InvItemId_RifleShells, 30);
            else if (GIVE_ITEMS[k].id == InvItemId_Shotgun)      give_item(InvItemId_ShotgunShells, 30);
            else if (GIVE_ITEMS[k].id == InvItemId_HyperBlaster) {
                give_item(InvItemId_HandgunBullets, 30);
                /* The HyperBlaster's aim/fire is hard-gated by
                 * Inventory_HyperBlasterFunctionalTest: without the UFO-ending
                 * unlock (or a Konami gun controller on port 2) it force-disables
                 * aiming, so a console-given blaster can't fire at all. Grant the
                 * unlock so it actually works. */
                g_SavegamePtr->clearGameEndings |= GameEndingFlag_Ufo;
            }
            cprintf("Given %s", GIVE_ITEMS[k].name);
            return;
        }
    }
    cprintf("unknown item '%s' - see 'help give'", arg);
}

/* Ending-relevant event flags. The exact ending matrix isn't fully labelled in
 * the decomp; these are the confirmed/strong candidates (Cybil saved = 445 from
 * monster_cybil.c, Kaufmann key = 394, plus the 395-403 cluster read by the
 * hospital/ending code). The ending is chosen when the FINAL BOSS is beaten, so
 * set these BEFORE that fight, not during the ending cutscene. setflag accepts
 * any flag number so nothing is locked out for experimentation. */
/* `map` warps via GameBoot_SavegameInitialize, which bzero's the whole savegame
 * (all event flags). To let "setending / setflag in the menu, then map to the
 * ending" work, every flag the user sets is also remembered here and re-applied
 * by cmd_map AFTER the savegame reset (and after MapLoad), just before gameplay
 * starts — so the ending cutscene reads the intended flags. */
#define MAX_PENDING_FLAGS 24
static struct { int flag; int val; } s_pendingFlags[MAX_PENDING_FLAGS];
static int s_pendingFlagCount = 0;

static void pending_flag_set(int flag, int val)
{
    int i;
    for (i = 0; i < s_pendingFlagCount; i++)
        if (s_pendingFlags[i].flag == flag) { s_pendingFlags[i].val = val; return; }
    if (s_pendingFlagCount < MAX_PENDING_FLAGS) {
        s_pendingFlags[s_pendingFlagCount].flag = flag;
        s_pendingFlags[s_pendingFlagCount].val  = val;
        s_pendingFlagCount++;
    }
}

void Pc_ConsoleApplyPendingFlags(void)
{
    int i;
    for (i = 0; i < s_pendingFlagCount; i++) {
        if (s_pendingFlags[i].val) Savegame_EventFlagSet(s_pendingFlags[i].flag);
        else                       Savegame_EventFlagClear(s_pendingFlags[i].flag);
    }
}

/* The SH1 ending is selected from two binary flags read by the map7_s03 ending
 * code: 449 = Cybil saved (Aglaophotis on her in the map6_s04 boss) and 391 =
 * "good path" (the map5_s03 Kaufmann subplot completed). The four combinations
 * are Bad / Bad+ / Good / Good+ — see cmd_setending. The others below are the
 * supporting/in-fight flags shown for reference. */
#define ENDFLAG_CYBIL 449
#define ENDFLAG_GOOD  391
typedef struct { const char* label; int flag; } s_EndFlag;
static const s_EndFlag ENDING_FLAGS[] = {
    { "Cybil saved",   ENDFLAG_CYBIL },
    { "Good path",     ENDFLAG_GOOD  },
    { "Cybil(infight)", 445 },
    { "Kaufmann key",  394 },
    { "flag 397", 397 }, { "flag 398", 398 },
};

static void cmd_getflags(void)
{
    int k;
    cprintf("Ending = Cybil(449) + Good(391). set BEFORE ending:");
    for (k = 0; k < (int)(sizeof(ENDING_FLAGS) / sizeof(ENDING_FLAGS[0])); k++)
        cprintf(" %3d %-13s = %d", ENDING_FLAGS[k].flag, ENDING_FLAGS[k].label,
                Savegame_EventFlagGet(ENDING_FLAGS[k].flag) ? 1 : 0);
    cprintf("setending bad|bad+|good|good+  | setflag <n> 0|1");
}

/* Set the two ending flags for a target ending. Must be done BEFORE the ending
 * cutscene triggers (the ending reads them at the final boss / cutscene start;
 * changing them mid-cutscene is too late). Does NOT advance story progress —
 * you still have to reach the ending. */
/* Forget the flags recorded by setflag/setending so they stop riding along on
 * the next console `map` warp. Does NOT revert flags already written to the live
 * savegame — load a save for that. */
static void cmd_clearflags(void)
{
    s_pendingFlagCount = 0;
    cprintf("cleared pending flags (live flags unchanged; load a save to reset)");
}

static void cmd_setending(const char* arg)
{
    int cybil, good;
    if      (strcmp(arg, "BAD") == 0)                                 { cybil = 0; good = 0; }
    else if (strcmp(arg, "BAD+") == 0  || strcmp(arg, "BADPLUS") == 0)  { cybil = 1; good = 0; }
    else if (strcmp(arg, "GOOD") == 0)                                { cybil = 0; good = 1; }
    else if (strcmp(arg, "GOOD+") == 0 || strcmp(arg, "GOODPLUS") == 0) { cybil = 1; good = 1; }
    else { cprintf("usage: setending bad | bad+ | good | good+"); return; }

    if (cybil) Savegame_EventFlagSet(ENDFLAG_CYBIL); else Savegame_EventFlagClear(ENDFLAG_CYBIL);
    if (good)  Savegame_EventFlagSet(ENDFLAG_GOOD);  else Savegame_EventFlagClear(ENDFLAG_GOOD);
    pending_flag_set(ENDFLAG_CYBIL, cybil);
    pending_flag_set(ENDFLAG_GOOD,  good);
    cprintf("ending '%s': Cybil(449)=%d Good(391)=%d", arg, cybil, good);
    cprintf("persists across 'map' warp; set before the ending");
}

static void cmd_setflag(const char* arg)
{
    int n = -1, v = -1;
    if (sscanf(arg, "%d %d", &n, &v) != 2 || n < 0 || n >= 52 * 32 ||
        (v != 0 && v != 1)) {
        cprintf("usage: setflag <number> 0|1");
        return;
    }
    if (v) Savegame_EventFlagSet(n);
    else   Savegame_EventFlagClear(n);
    pending_flag_set(n, v);
    cprintf("flag %d = %d", n, v);
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
    " give <item>    see 'help give' for the full list",
    " getflags       show ending flags",
    " setending <e>  bad | bad+ | good | good+",
    " setflag <n> 0|1  set any event flag",
    " kill           kill Harry (death animation)",
    " killall        kill all nearby enemies",
    " noclip         walk through walls (floor stays on)",
    " invaspect 0|1  inventory item proportions: PSX | square",
    " invscale <pct> inventory item vertical scale (def 125)",
    " invcary <n>    carousel item Y offset (+down)",
    " inveqy <n>     equipped item Y offset (+down)",
    " invdim <pct>   off-center carousel dim strength",
    " fmv            list movies (numbered)",
    " fmv <name|#>   play a movie (also intro1-2, end1-5)",
    "Quick Save: F6   Quick Load: F8 (work outside console)",
};

static const char* const HELP_GIVE_PAGE1[] = {
    "give <item> (page 1/2) - weapons, ammo, recovery:",
    " knife pipe rockdrill hammer chainsaw katana axe",
    " handgun rifle shotgun hyperblaster (guns add ammo)",
    " ammo  handgunammo rifleammo shotgunammo",
    " gasoline (chainsaw/drill fuel)",
    " allweapons    all melee + guns + ammo + gas",
    " health healthdrink firstaid ampoule",
    "type 'help give 2' for story / ending items",
};

static const char* const HELP_GIVE_PAGE2[] = {
    "give <item> (page 2/2) - story / ending items:",
    " flauros channelingstone plasticbottle aglaophotis",
    " kaufmannkey ringofcontract stoneoftime amulet",
    " crestofmercury ankh dagger disk",
    " goldmedallion silvermedallion lighter videotape",
    " camera chemical bloodpack",
};

/* Up-shift (psx-units) for bottom-anchored message boxes. Was 35 to lift subtitles
 * out of the 3D-world vertical-FOV bottom crop, but the UI now draws at full vertical
 * ortho (g_PsxUIOrthoPass) so no compensation is needed — default 0. Console MSGSHIFT
 * (read by text_draw.c) stays for fine-tuning. */
int g_PsxMsgVShift = 0;

/* Cutscene letterbox bar Y (centered coords): bars span +/-Outer (screen edge) to
 * +/-Inner. Tunable via console BARY while the interlaced-buffer mapping is dialed in.
 * Read by cutscene_border.c. */
int g_PsxBarOuter = 112;
int g_PsxBarInner = 96;

static const char* const DEBUG_PAGE1[] = {
    "Debug keys (page 1/2) - cheats & tools:",
    " Esc     warm reset to the title screen",
    " 0       noclip toggle (walk through walls)",
    " 4 / 5   map config prev / next (loads on New Game)",
    " 6       kill nearby enemies",
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
        if (strncmp(arg, "GIVE", 4) == 0) {
            if (strstr(arg, "2"))
                push_lines(HELP_GIVE_PAGE2, (int)(sizeof(HELP_GIVE_PAGE2) / sizeof(HELP_GIVE_PAGE2[0])));
            else
                push_lines(HELP_GIVE_PAGE1, (int)(sizeof(HELP_GIVE_PAGE1) / sizeof(HELP_GIVE_PAGE1[0])));
        } else {
            push_lines(HELP_LINES, (int)(sizeof(HELP_LINES) / sizeof(HELP_LINES[0])));
        }
    } else if (strcmp(cmd, "GETFLAGS") == 0) {
        cmd_getflags();
    } else if (strcmp(cmd, "SETFLAG") == 0) {
        cmd_setflag(arg);
    } else if (strcmp(cmd, "SETENDING") == 0) {
        cmd_setending(arg);
    } else if (strcmp(cmd, "CLEARFLAGS") == 0) {
        cmd_clearflags();
    } else if (strcmp(cmd, "DEBUG") == 0) {
        if (strcmp(arg, "2") == 0)
            push_lines(DEBUG_PAGE2, (int)(sizeof(DEBUG_PAGE2) / sizeof(DEBUG_PAGE2[0])));
        else
            push_lines(DEBUG_PAGE1, (int)(sizeof(DEBUG_PAGE1) / sizeof(DEBUG_PAGE1[0])));
    } else if (strcmp(cmd, "MAP") == 0) {
        cmd_map(arg);
    } else if (strcmp(cmd, "GIVE") == 0) {
        cmd_give(arg);
    } else if (strcmp(cmd, "KILL") == 0) {
        g_SysWork.playerWork.player.health = -Q12(1.0f);
        cprintf("killed Harry");
    } else if (strcmp(cmd, "KILLALL") == 0) {
        s_SubCharacter* hr   = &g_SysWork.playerWork.player;
        int             killed = 0;
        int             i;
        for (i = 0; i < NPC_COUNT_MAX; i++) {
            s_SubCharacter* npc = &g_SysWork.npcs[i];
            if (npc->model.charaId == Chara_None || npc->model.charaId == Chara_Harry ||
                npc->health <= Q12(0.0f)) {
                continue;
            }
            if (ABS(npc->position.vx - hr->position.vx) > Q12(50.0f) ||
                ABS(npc->position.vz - hr->position.vz) > Q12(50.0f)) {
                continue;
            }
            npc->damage.amount = Q12(99999.0f);
            killed++;
        }
        cprintf("killed %d nearby enemies", killed);
    } else if (strcmp(cmd, "NOCLIP") == 0) {
        g_DebugNoWallCollision = !g_DebugNoWallCollision;
        cprintf("noclip %s", g_DebugNoWallCollision ? "ON" : "OFF");
    } else if (strcmp(cmd, "INVASPECT") == 0) {
        extern int g_PcInvAspectSquare;
        if (arg[0] == '1') g_PcInvAspectSquare = 1;
        else if (arg[0] == '0') g_PcInvAspectSquare = 0;
        else g_PcInvAspectSquare = !g_PcInvAspectSquare;
        cprintf("inventory item aspect: %s", g_PcInvAspectSquare ? "SQUARE (true proportions)" : "PSX-faithful");
    } else if (strcmp(cmd, "INVSCALE") == 0) {
        extern int g_PcInvAspectPct;
        int v = atoi(arg);
        if (v >= 50 && v <= 200) g_PcInvAspectPct = v;
        cprintf("inventory item vertical scale: %d%% of square", g_PcInvAspectPct);
    } else if (strcmp(cmd, "INVCARY") == 0) {
        /* the console minus key types '_', so accept a leading '_' as '-'. */
        extern int g_PcInvCarouselYOff;
        if (arg[0]) g_PcInvCarouselYOff = (arg[0] == '_') ? -atoi(arg + 1) : atoi(arg);
        cprintf("carousel item Y offset: %d (+ down)", g_PcInvCarouselYOff);
    } else if (strcmp(cmd, "INVEQY") == 0) {
        extern int g_PcInvEquipYOff;
        if (arg[0]) g_PcInvEquipYOff = (arg[0] == '_') ? -atoi(arg + 1) : atoi(arg);
        cprintf("equipped item Y offset: %d (+ down)", g_PcInvEquipYOff);
    } else if (strcmp(cmd, "INVDIM") == 0) {
        extern int g_PcInvDimStrength;
        int v = atoi(arg);
        if (v >= 0 && v <= 100) g_PcInvDimStrength = v;
        cprintf("off-center carousel dim: %d%%", g_PcInvDimStrength);
    } else if (strcmp(cmd, "OBST") == 0) {
        extern int g_PcObstacleCollision;
        if (arg[0]) g_PcObstacleCollision = atoi(arg) ? 1 : 0;
        cprintf("round-obstacle (ptr_18) collision: %s", g_PcObstacleCollision ? "ON" : "OFF (sprint-through)");
    } else if (strcmp(cmd, "COLLSCOPE") == 0) {
        extern int g_PcChunkCollisionLocalScope;
        if (arg[0]) g_PcChunkCollisionLocalScope = atoi(arg) ? 1 : 0;
        cprintf("preload collision local-cell scope: %s", g_PcChunkCollisionLocalScope ? "ON (vanilla window)" : "OFF (all chunks)");
    } else if (strcmp(cmd, "ALPHA") == 0) {
        extern int g_PcSlopeAlphaFix;
        if (arg[0]) g_PcSlopeAlphaFix = atoi(arg) ? 1 : 0;
        cprintf("slope-alpha invisible-wall fix: %s", g_PcSlopeAlphaFix ? "ON (capped)" : "OFF (original)");
    } else if (strcmp(cmd, "VFOV") == 0) {
        extern float g_PsxWorldVScale;
        if (arg[0]) g_PsxWorldVScale = (float)atof(arg);
        cprintf("world vertical FOV scale: %.3f (1.0=off; ~0.872 matches DuckStation)", g_PsxWorldVScale);
    } else if (strcmp(cmd, "HFOV") == 0) {
        /* 3D-world horizontal scale (Hor+ only). 1.0 = current behaviour; >1 = wider
         * models, <1 = narrower. Pure tuning/preference knob, default neutral. */
        extern float g_PsxWorldHScale;
        if (arg[0]) g_PsxWorldHScale = (float)atof(arg);
        cprintf("world horizontal scale: %.3f (1.0=off; >1 wider models, <1 narrower)", g_PsxWorldHScale);
    } else if (strcmp(cmd, "VSHIFT") == 0) {
        extern float g_PsxWorldVShift;
        if (arg[0]) g_PsxWorldVShift = (float)atof(arg);
        cprintf("world vertical view shift: %.1f psx-units (+ = view up; 0=off)", g_PsxWorldVShift);
    } else if (strcmp(cmd, "MSGSHIFT") == 0) {
        extern int g_PsxMsgVShift;
        if (arg[0]) g_PsxMsgVShift = atoi(arg);
        cprintf("message box up-shift: %d psx-units (compensates the VFOV bottom crop)", g_PsxMsgVShift);
    } else if (strcmp(cmd, "BARY") == 0) {
        extern int g_PsxBarOuter, g_PsxBarInner;
        if (arg[0]) { g_PsxBarOuter = atoi(arg); g_PsxBarInner = g_PsxBarOuter - 16; }
        cprintf("letterbox bar Y: outer=%d inner=%d (raise until bars hit the screen edges)", g_PsxBarOuter, g_PsxBarInner);
    } else if (strcmp(cmd, "FOGSTR") == 0) {
        /* World fog density multiplier. The PSX layered a 2nd semi-transparent fog
         * poly the PC port drops, so the single-pass shader fog reads thinner ("filter"
         * look); >1.0 deepens it toward the oppressive PSX wall. 1.0 = native (default).
         * Live-tune vs DuckStation, then we can bake a value as the default. */
        extern float g_PsyX_FogStrength;
        if (arg[0]) g_PsyX_FogStrength = (float)atof(arg);
        cprintf("world fog strength: %.2f (1.0=native PC fog; >1 deepens toward PSX)", g_PsyX_FogStrength);
    } else if (strcmp(cmd, "WELD") == 0) {
        extern float g_pgxpWeldPx;
        if (arg[0]) g_pgxpWeldPx = (float)atof(arg);
        cprintf("PGXP seam weld radius: %.2f px (0=off)", g_pgxpWeldPx);
    } else if (strcmp(cmd, "WELDW") == 0) {
        extern float g_pgxpWeldWRatio;
        if (arg[0]) g_pgxpWeldWRatio = (float)atof(arg);
        cprintf("PGXP weld depth ratio: %.3f", g_pgxpWeldWRatio);
    } else if (strcmp(cmd, "PGXPEDGE") == 0) {
        extern float g_PgxpEdgeMax;
        if (arg[0]) g_PgxpEdgeMax = (float)atof(arg);
        cprintf("PGXP off-screen position clamp: %.0f psx-units (higher = less edge warp)", g_PgxpEdgeMax);
    } else if (strcmp(cmd, "PGXPDEPTH") == 0) {
        extern int g_PgxpUseUnquantizedDepth;
        if (arg[0] == '1') g_PgxpUseUnquantizedDepth = 1;
        else if (arg[0] == '0') g_PgxpUseUnquantizedDepth = 0;
        else g_PgxpUseUnquantizedDepth = !g_PgxpUseUnquantizedDepth;
        cprintf("PGXP unquantized-depth W (distance-seam fix): %s", g_PgxpUseUnquantizedDepth ? "ON" : "OFF");
    } else if (strcmp(cmd, "FMV") == 0) {
        cmd_fmv(arg);
    } else if (strcmp(cmd, "PGXP") == 0) {
        extern int g_PsxUsePgxp;
        if (arg[0] == '1') g_PsxUsePgxp = 1;
        else if (arg[0] == '0') g_PsxUsePgxp = 0;
        else g_PsxUsePgxp = !g_PsxUsePgxp; /* bare "pgxp" toggles */
        cprintf("PGXP %s (perspective-correct, WIP)", g_PsxUsePgxp ? "ON" : "OFF");
    } else if (strcmp(cmd, "FLASHLIGHT") == 0 || strcmp(cmd, "FL") == 0 ||
               strcmp(cmd, "WORLDLIGHT") == 0 || strcmp(cmd, "WL") == 0) {
        extern int g_PcFlashlightColorActive, g_PcWorldLightColorActive;
        extern unsigned char g_PcFlashlightColorR, g_PcFlashlightColorG, g_PcFlashlightColorB;
        extern unsigned char g_PcWorldLightColorR, g_PcWorldLightColorG, g_PcWorldLightColorB;
        int isWorld = (cmd[0] == 'W');
        const char* label = isWorld ? "world light" : "flashlight";
        int* active = isWorld ? &g_PcWorldLightColorActive : &g_PcFlashlightColorActive;
        unsigned char* cr = isWorld ? &g_PcWorldLightColorR : &g_PcFlashlightColorR;
        unsigned char* cg = isWorld ? &g_PcWorldLightColorG : &g_PcFlashlightColorG;
        unsigned char* cb = isWorld ? &g_PcWorldLightColorB : &g_PcFlashlightColorB;
        struct { const char* name; unsigned char r, g, b; } presets[] = {
            { "RED",    255,   0,   0 },
            { "GREEN",    0, 255,   0 },
            { "BLUE",     0,   0, 255 },
            { "YELLOW", 255, 255,   0 },
            { "CYAN",     0, 255, 255 },
            { "PURPLE", 255,   0, 255 },
            { "MAGENTA",255,   0, 255 },
            { "ORANGE", 255, 128,   0 },
            { "PINK",   255, 128, 192 },
            { "WHITE",  255, 255, 255 },
        };
        if (strcmp(arg, "DEFAULT") == 0 || strcmp(arg, "OFF") == 0 || arg[0] == '\0') {
            *active = 0;
            cprintf("%s color: default", label);
        } else {
            int found = 0, k;
            for (k = 0; k < (int)(sizeof(presets) / sizeof(presets[0])); k++) {
                if (strcmp(arg, presets[k].name) == 0) {
                    *cr = presets[k].r; *cg = presets[k].g; *cb = presets[k].b;
                    *active = 1;
                    cprintf("%s color: %s", label, presets[k].name);
                    found = 1;
                    break;
                }
            }
            if (!found)
                cprintf("unknown color '%s' (red/green/blue/yellow/cyan/purple/orange/pink/white/default)", arg);
        }
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
