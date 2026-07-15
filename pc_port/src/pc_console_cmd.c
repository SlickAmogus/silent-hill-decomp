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
 *   GOD [0|1]            - toggle/set Harry damage immunity
 *   FMV                  - list all FMV names (numbered)
 *   FMV <name|number>    - play an FMV (fades out, plays, fades back in)
 *   FMV INTROn / ENDn    - alias for the nth intro (C*) / ending (Z*) movie
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/game_boot/game_boot.h"
#include "bodyprog/game_boot/fs_chara_anim.h" /* g_CharaModelAnimsData (spawn anim-ready gate) */
#include "bodyprog/events/player_pos_update.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/collision/collision.h"
#include "bodyprog/math/math.h"
#include "bodyprog/items.h"
#include "bodyprog/savegame.h"
#include "bodyprog/item_screens.h" /* GameEndingFlag_Ufo (HyperBlaster give-unlock) */
#include "bodyprog/screen/screen_fade.h"
#include "bodyprog/sound/sound_system.h" /* AMBSFX sweep: s_VabInfo / Sd_PlaySfx */
#include "bodyprog/sound/sfx_id_enum.h"
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
    /* The overlay wraps anything wider than the window, so this only needs to be
     * big enough to hold a full wide-window line before wrapping. */
    char line[256];
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
        char line[256];
        int  used = 0;
        line[0] = '\0';
        for (i = 0; i < count; i++) {
            const char* nm = MapRegistry_GetName(i);
            if (!nm) continue;
            if (used + (int)strlen(nm) + 1 >= 200) {
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
        strncpy(g_PcConfig.mapName, lower, sizeof(g_PcConfig.mapName) - 1);
        g_PcConfig.mapName[sizeof(g_PcConfig.mapName) - 1] = '\0';
        {
            const char* desc = MapRegistry_GetDescription((e_MapIdx)mapId);
            if (desc && desc[0])
                cprintf("map config set to %s - %s (loads on New Game)", lower, desc);
            else
                cprintf("map config set to %s (loads on New Game)", lower);
        }
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
    " spawn list     list monsters loaded in this map",
    " spawn <name>   spawn a monster in front of Harry",
    " noclip         walk through walls (floor stays on)",
    " god [0|1]      toggle/set damage immunity for Harry",
    " invaspect 0|1  inventory item proportions: PSX | square",
    " invscale <pct> inventory item vertical scale (def 125)",
    " invcary <n>    carousel item Y offset (+down)",
    " inveqy <n>     equipped item Y offset (+down)",
    " invdim <pct>   off-center carousel dim strength",
    " fmv            list movies (numbered)",
    " fmv <name|#>   play a movie (also intro1-2, end1-5)",
    " kf [n]         keyframe inspector: set/show frame (K key)",
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
    " K       keyframe inspector; , . scrub (hold = faster)",
    " [ / ]   drop A/B position markers into the log",
    " ~       console open/close (game pauses; PgUp/PgDn scroll)",
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
/* Debug monster spawner. A map only keeps 3 enemy types resident at once (its
 * charaGroupIds); Chara_Spawn assumes the model+anim+update-func are already
 * loaded and does NOT load them, so spawning an off-map type gives an entity
 * the radio pings but the draw path skips (registeredCharaModels[id]==NULL) —
 * the classic "radio plays, monster invisible" bug. So SPAWN only offers types
 * actually loaded for the current map (SPAWN LIST), guaranteeing visibility.
 *
 * `state` is the initial model.stateStep (== s_SpawnInfo.flags the room spawner
 * uses): the enemy's spawn/active AI entry state. Defaults per category below;
 * overridable via `SPAWN <name> <state>` when a monster wakes in a weird pose. */
typedef struct { const char* name; u8 charaId; u8 state; } s_SpawnCharaEntry;
static const s_SpawnCharaEntry SPAWN_CHARAS[] = {
    { "AIRSCREAMER",     Chara_AirScreamer,     12 },
    { "NIGHTFLUTTER",    Chara_NightFlutter,    12 },
    { "GROANER",         Chara_Groaner,          3 }, /* Groaner_Init: st=3->Control_1 (active); any other st = stuck lying down (never re-checks) */
    { "WORMHEAD",        Chara_Wormhead,         5 },
    { "LARVALSTALKER",   Chara_LarvalStalker,    5 },
    { "STALKER",         Chara_Stalker,          3 }, /* Stalker_Update: st=3->Control_4 (active); st=5->Control_1 = unposed/invisible */
    { "GREYCHILD",       Chara_GreyChild,        3 }, /* also Stalker_Update (see STALKER) */
    { "MUMBLER",         Chara_Mumbler,         17 },
    { "HANGEDSCRATCHER", Chara_HangedScratcher,  7 },
    { "CREEPER",         Chara_Creeper,          5 },
    { "ROMPER",          Chara_Romper,           3 }, /* Romper_Init activates unconditionally; 3 matches native map spawn data */
    { "CHICKEN",         Chara_Chicken,          5 },
    { "SPLITHEAD",       Chara_SplitHead,        5 },
    { "FLOATSTINGER",    Chara_Floatstinger,    12 },
    { "PUPPETNURSE",     Chara_PuppetNurse,     17 },
    { "BETANURSE",       Chara_DummyNurse,      17 }, /* TEST/PRS2.ILM beta nurse via the pool's DummyNurse retarget; runs the real PuppetNurse AI. In hospital maps the native DUMMY stub wins the slot (invisible) — test elsewhere. */
    { "PUPPETDOCTOR",    Chara_PuppetDoctor,    17 },
    { "DUMMYDOCTOR",     Chara_DummyDoctor,     17 },
    { "TWINFEELER",      Chara_Twinfeeler,       3 },
    { "BLOODSUCKER",     Chara_Bloodsucker,     17 },
    { "INCUBUS",         Chara_Incubus,          3 },
    { "UNKNOWN23",       Chara_Unknown23,        3 },
    { "MONSTERCYBIL",    Chara_MonsterCybil,     3 },
    { "LOCKERDEADBODY",  Chara_LockerDeadBody,   3 },
    { "CYBIL",           Chara_Cybil,            3 },
    { "ENDINGCYBIL",     Chara_EndingCybil,      3 },
    { "CHERYL",          Chara_Cheryl,           1 },
    { "CAT",             Chara_Cat,              3 },
    { "DAHLIA",          Chara_Dahlia,           3 },
    { "ENDINGDAHLIA",    Chara_EndingDahlia,     3 },
    { "LISA",            Chara_Lisa,             3 },
    { "BLOODYLISA",      Chara_BloodyLisa,       3 },
    { "ALESSA",          Chara_Alessa,           3 },
    { "GHOSTCHILDALESSA",Chara_GhostChildAlessa, 3 },
    { "INCUBATOR",       Chara_Incubator,        3 },
    { "BLOODYINCUBATOR", Chara_BloodyIncubator,  3 },
    { "KAUFMANN",        Chara_Kaufmann,         3 },
    { "ENDINGKAUFMANN",  Chara_EndingKaufmann,   3 },
    { "FLAUROS",         Chara_Flauros,          3 },
    { "LITTLEINCUBUS",   Chara_LittleIncubus,    3 },
    { "GHOSTDOCTOR",     Chara_GhostDoctor,      3 },
    { "PARASITE",        Chara_Parasite,         3 },
};

static const char* spawn_chara_name(s32 charaId)
{
    int i;
    for (i = 0; i < (int)(sizeof(SPAWN_CHARAS) / sizeof(SPAWN_CHARAS[0])); i++) {
        if (SPAWN_CHARAS[i].charaId == charaId)
            return SPAWN_CHARAS[i].name;
    }
    return NULL;
}

/* A model is drawable only once its file finished streaming; bones need the
 * anim slot. Both gate visibility, so require both before offering a spawn. */
static int spawn_chara_model_ready(s32 charaId)
{
    s_CharaModel* m = g_WorldGfxWork.registeredCharaModels[charaId];
    return m != NULL && m->isLoaded;
}
static int spawn_chara_anim_ready(s32 charaId)
{
    /* idx alone can be stale (vanilla never invalidates it; a failed pool
     * load resets it, but belt-and-braces: the slot must actually hold a
     * live ANM header or the spawn renders unposed/invisible). */
    s8 idx = g_CharaAnimDataIdxs[charaId];
    return idx != (s8)NO_VALUE &&
           g_CharaModelAnimsData[idx].activeAnmHdr != NULL;
}

static void cmd_spawn(const char* arg)
{
    char nm[32];
    int  k;
    const char* rest;
    int  stateOverride;
    int  i;
    const s_SpawnCharaEntry* pick;
    s_SubCharacter* hr;
    s32  yaw, sn, cs, npcIdx;
    q19_12 dist, posX, posZ;
    s_CollisionSurface surf;
    u32  state;

    /* Global chara pool: repair mid-map evictions (cutscene Chara_Load with
     * CHARA_FORCE_FREE_ALL NULLs native registrations) before gating. */
    {
        extern void Pc_CharaPool_Refresh(void);
        Pc_CharaPool_Refresh();
    }

    if (arg[0] == '\0' || strcmp(arg, "LIST") == 0) {
        extern int Pc_CharaPool_IsPoolModel(int charaId);
        int any = 0;
        cprintf("spawnable here:");
        for (i = 0; i < (int)(sizeof(SPAWN_CHARAS) / sizeof(SPAWN_CHARAS[0])); i++) {
            s32 id = SPAWN_CHARAS[i].charaId;
            if (!spawn_chara_model_ready(id))
                continue;
            {
                int anim = spawn_chara_anim_ready(id);
                int ai   = g_MapOverlayHdr.charaUpdateFuncs[id] != NULL;
                cprintf(" %s%s%s%s", SPAWN_CHARAS[i].name,
                        Pc_CharaPool_IsPoolModel(id) ? " [pool]" : "",
                        anim ? "" : " [no-anim]", ai ? "" : " [no-ai]");
                any = 1;
            }
        }
        if (!any)
            cprintf(" (none)");
        return;
    }

    /* Split "<NAME> [state]". */
    for (k = 0; arg[k] && arg[k] != ' ' && k < (int)sizeof(nm) - 1; k++)
        nm[k] = arg[k];
    nm[k] = '\0';
    rest = arg[k] ? arg + k + 1 : arg + k;
    while (*rest == ' ') rest++;
    stateOverride = (*rest) ? atoi(rest) : -1;

    pick = NULL;
    for (i = 0; i < (int)(sizeof(SPAWN_CHARAS) / sizeof(SPAWN_CHARAS[0])); i++) {
        if (strcmp(nm, SPAWN_CHARAS[i].name) == 0) {
            pick = &SPAWN_CHARAS[i];
            break;
        }
    }
    if (pick == NULL) {
        cprintf("unknown: %s (try 'spawn list')", nm);
        return;
    }
    if (!spawn_chara_model_ready(pick->charaId) || !spawn_chara_anim_ready(pick->charaId)) {
        cprintf("%s not loaded in this map", nm);
        cprintf("use 'spawn list' to see loaded types");
        return;
    }

    /* Find a free NPC slot (mirrors Chara_Spawn's slot fill, minus the
     * concurrent cap / dedup so a debug spawn always fires when a slot exists). */
    npcIdx = -1;
    for (i = 0; i < NPC_COUNT_MAX; i++) {
        if (g_SysWork.npcs[i].model.charaId == Chara_None) {
            npcIdx = i;
            break;
        }
    }
    if (npcIdx < 0) {
        cprintf("no free NPC slot (max %d) - killall first", NPC_COUNT_MAX);
        return;
    }

    hr   = &g_SysWork.playerWork.player;
    yaw  = hr->rotation.vy;
    sn   = Math_Sin(yaw);
    cs   = Math_Cos(yaw);
    dist = Q12(4.0f); /* a few units in front of Harry (clear of his own radius) */
    posX = hr->position.vx + (s32)(((s64)dist * sn) >> 12);
    posZ = hr->position.vz + (s32)(((s64)dist * cs) >> 12);

    state = (stateOverride >= 0) ? (u32)stateOverride : pick->state;

    memset(&g_SysWork.npcs[npcIdx], 0, sizeof(s_SubCharacter));
    g_SysWork.npcs[npcIdx].model.charaId      = pick->charaId;
    g_SysWork.npcs[npcIdx].model.controlState = 0;
    g_SysWork.npcs[npcIdx].model.stateStep    = (u8)state;
    g_SysWork.npcs[npcIdx].field_40           = (s8)npcIdx;
    g_SysWork.npcs[npcIdx].position.vx        = posX;
    g_SysWork.npcs[npcIdx].position.vz        = posZ;
    Collision_SurfaceGet(&surf, posX, posZ);
    g_SysWork.npcs[npcIdx].position.vy        = surf.groundHeight;
    g_SysWork.npcs[npcIdx].rotation.vy        = (s16)((yaw + 0x800) & 0xFFF); /* face Harry (180deg) */
    g_SysWork.npcs[npcIdx].model.anim.flags  |= AnimFlag_Visible;
    SET_FLAG(&g_SysWork.npcFlags, npcIdx);
    /* Match Game_NpcRoomInitSpawn: mark the spawn slot in field_228C too, so the
     * despawn/dedup bookkeeping (CLEAR_FLAG on npc->field_40) stays consistent. */
    SET_FLAG(g_SysWork.field_228C, npcIdx);
    /* No savegame identity: killing this spawn must not dead-flag the native
     * spawn row that happens to share field_40's value (npc_main.c guard). */
    {
        extern unsigned char g_PcNpcDebugSpawned[];
        g_PcNpcDebugSpawned[npcIdx] = 1;
    }

    if (g_MapOverlayHdr.charaUpdateFuncs[pick->charaId] != NULL)
        cprintf("spawned %s (state %d)", nm, (int)state);
    else
        cprintf("spawned %s (state %d, no AI in this map)", nm, (int)state);
    /* Full diagnostic so an "invisible spawn" report is decisive: player vs spawn
     * world pos (incl. collision groundHeight), and the three visibility gates
     * (model streamed, anim slot, per-map AI update fn). */
    SH_DBG("[SPAWN] %s id=%d npc[%d] st=%u player=(%d,%d,%d) spawn=(%d,%d,%d) model=%d animIdx=%d updFn=%d",
           nm, (int)pick->charaId, (int)npcIdx, state,
           FP_FROM(hr->position.vx, Q12_SHIFT), FP_FROM(hr->position.vy, Q12_SHIFT), FP_FROM(hr->position.vz, Q12_SHIFT),
           FP_FROM(posX, Q12_SHIFT), FP_FROM(surf.groundHeight, Q12_SHIFT), FP_FROM(posZ, Q12_SHIFT),
           spawn_chara_model_ready(pick->charaId), (int)g_CharaAnimDataIdxs[pick->charaId],
           g_MapOverlayHdr.charaUpdateFuncs[pick->charaId] != NULL);
}

/* TEMP diagnostic (sewer-drip hunt): play samples from the currently-resident
 * ambient VAB (SPU slot 2). Must be run while IN the map whose ambient bank you
 * want to hear (e.g. the sewers). Slot-2 SFX ids each map to a (program, note) of
 * that bank via g_Vab_InfoTable, so sweeping them plays every ambient sample.
 *   AMBSFX          - play the NEXT slot-2 ambient id, print its id/prog/tone/note
 *   AMBSFX <n>      - play a specific id (n>=1280) or index (n<1280 -> +1280)
 *   AMBSFX PROG <p> - restart the sweep at the first id of program p
 *   AMBSFX STOP     - stop all SFX (silence a stuck loop) */
static void cmd_ambsfx(const char* arg)
{
    extern s_VabInfo g_Vab_InfoTable[];
    extern u8        Sd_PlaySfx(u16 sfxId, s8 balance, u8 vol);
    extern void      SD_Call(u32 cmd);

    static s16 s_id       = -1;
    static s16 s_sweepProg = -1; /* sticky program filter: PROG <n> sets it, bare AMBSFX keeps it */
    s16        id;
    u16        vp;

    if (strcmp(arg, "STOP") == 0) {
        SD_Call(16); /* Sd_AllSfxStop + Sd_LastSfxStop */
        cprintf("ambsfx: stopped");
        return;
    }

    /* ALL <p> — key EVERY slot-2 id of program p at once, so a multi-pitch drip
     * bank plays layered (what the sewer ambience actually sounds like). */
    if (strncmp(arg, "ALL", 3) == 0) {
        int p = atoi(arg + 3);
        int n = 0;
        for (id = (s16)Sfx_Base; id < (s16)(Sfx_Base + 420); id++) {
            vp = g_Vab_InfoTable[id - Sfx_Base].vab_progIdx_2;
            if ((vp >> 8) == 2 && (int)(vp & 0xFF) == p) {
                Sd_PlaySfx((u16)id, 0, 0);
                n++;
            }
        }
        cprintf("ambsfx: keyed %d ids of prog %d", n, p);
        SH_DBG("[DRIPSWEEP] ALL prog=%d keyed=%d", p, n);
        return;
    }

    if (arg[0] >= '0' && arg[0] <= '9') {
        id = (s16)atoi(arg);
        if (id < Sfx_Base) id = (s16)(id + Sfx_Base);
    } else {
        /* PROG <n> arms a STICKY program filter so a following bare AMBSFX walks
         * only program n, one id at a time (PROG -1 clears it back to all progs).
         * Previously the filter was local and lost on the next call, so stepping
         * wandered straight out of the program you asked for. */
        if (strncmp(arg, "PROG", 4) == 0) { s_sweepProg = (s16)atoi(arg + 4); s_id = -1; }
        id = (s_id < 0) ? (s16)Sfx_Base : (s16)(s_id + 1);
        while (id < (s16)(Sfx_Base + 420)) {
            vp = g_Vab_InfoTable[id - Sfx_Base].vab_progIdx_2;
            if ((vp >> 8) == 2 && (s_sweepProg < 0 || (int)(vp & 0xFF) == s_sweepProg))
                break;
            id++;
        }
        if (id >= (s16)(Sfx_Base + 420)) { s_id = -1; cprintf("ambsfx: end of prog %d (wrapped)", s_sweepProg); return; }
    }

    s_id = id;
    vp   = g_Vab_InfoTable[id - Sfx_Base].vab_progIdx_2;
    Sd_PlaySfx((u16)id, 0, 0);
    cprintf("ambsfx id=%d slot=%d prog=%d tone=%d note=%d", id, vp >> 8, vp & 0xFF,
            g_Vab_InfoTable[id - Sfx_Base].audioVabIdx,
            g_Vab_InfoTable[id - Sfx_Base].noteIdx_4);
    SH_DBG("[DRIPSWEEP] AMBSFX id=%d slot=%d prog=%d tone=%d note=%d", id, vp >> 8, vp & 0xFF,
           g_Vab_InfoTable[id - Sfx_Base].audioVabIdx,
           g_Vab_InfoTable[id - Sfx_Base].noteIdx_4);
}

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
    } else if (strcmp(cmd, "AMBSFX") == 0) {
        cmd_ambsfx(arg);
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
    } else if (strcmp(cmd, "SPAWN") == 0) {
        cmd_spawn(arg);
    } else if (strcmp(cmd, "UNLIMITED") == 0) {
        extern int g_PcUnlimitedEnemies;
        if (arg[0] == '1') g_PcUnlimitedEnemies = 1;
        else if (arg[0] == '0') g_PcUnlimitedEnemies = 0;
        else g_PcUnlimitedEnemies = !g_PcUnlimitedEnemies;
        cprintf("unlimited enemies %s (cap now %d)", g_PcUnlimitedEnemies ? "ON" : "OFF", NPC_COUNT_MAX);
    } else if (strcmp(cmd, "NOCLIP") == 0) {
        g_DebugNoWallCollision = !g_DebugNoWallCollision;
        cprintf("noclip %s", g_DebugNoWallCollision ? "ON" : "OFF");
    } else if (strcmp(cmd, "GOD") == 0) {
        extern int g_PcGodMode;
        if (arg[0] == '1') g_PcGodMode = 1;
        else if (arg[0] == '0') g_PcGodMode = 0;
        else g_PcGodMode = !g_PcGodMode;
        cprintf("god mode %s (no damage + health held full; same as debug key 7)", g_PcGodMode ? "ON" : "OFF");
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
    } else if (strcmp(cmd, "PGXPNEARCLIP") == 0) {
        extern int g_PsxPgxpNearClip;
        if (arg[0] == '1') g_PsxPgxpNearClip = 1;
        else if (arg[0] == '0') g_PsxPgxpNearClip = 0;
        else g_PsxPgxpNearClip = !g_PsxPgxpNearClip;
        cprintf("PGXP near-plane clipping (close-camera warp fix): %s", g_PsxPgxpNearClip ? "ON" : "OFF");
    } else if (strcmp(cmd, "PGXPNEARZ") == 0) {
        /* Clipper divides by this and uses it as the clip verts' W — keep >= 1. */
        extern float g_PgxpNearZ;
        if (arg[0]) { g_PgxpNearZ = (float)atof(arg); if (g_PgxpNearZ < 1.0f) g_PgxpNearZ = 1.0f; }
        cprintf("PGXP near-clip plane depth: %.1f gte-units", g_PgxpNearZ);
    } else if (strcmp(cmd, "ADD") == 0) {
        /* Debug-isolate the additive (BM_ADD) render layer to diagnose the map7_s03
         * pre/during/post-fight fire-and-lightning. 0 = drop all additive splits
         * (does the fire/lightning vanish? -> it's additive geometry), 1 = normal,
         * 2 = additive drawn depth-tested (does the under-floor lightning get
         * occluded by the floor? -> the bug is the disabled depth test). */
        extern int g_PsxDbgAddMode;
        if (arg[0]) g_PsxDbgAddMode = atoi(arg);
        cprintf("additive layer: mode %d (0=skip, 1=normal, 2=depth-tested)", g_PsxDbgAddMode);
    } else if (strcmp(cmd, "FMV") == 0) {
        cmd_fmv(arg);
    } else if (strcmp(cmd, "PGXP") == 0) {
        extern int g_PsxUsePgxp;
        if (arg[0] == '1') g_PsxUsePgxp = 1;
        else if (arg[0] == '0') g_PsxUsePgxp = 0;
        else g_PsxUsePgxp = !g_PsxUsePgxp; /* bare "pgxp" toggles */
        g_PcConfig.usePgxp = g_PsxUsePgxp ? 1 : 0;
        PcConfig_SaveKeyValue("use_pgxp", g_PsxUsePgxp ? "1" : "0");
        cprintf("PGXP %s (perspective-correct, WIP)", g_PsxUsePgxp ? "ON" : "OFF");
    } else if (strcmp(cmd, "FLMODE") == 0) {
        /* flmode 0..3 | classic | classicshadows | modern | modernshadows */
        int mode = g_PcConfig.flashlightMode;
        if (arg[0] >= '0' && arg[0] <= '3' && arg[1] == '\0') mode = arg[0] - '0';
        else if (strcmp(arg, "CLASSIC") == 0) mode = 0;
        else if (strcmp(arg, "CLASSICSHADOWS") == 0) mode = 1;
        else if (strcmp(arg, "MODERN") == 0) mode = 2;
        else if (strcmp(arg, "MODERNSHADOWS") == 0) mode = 3;
        else if (arg[0] != '\0') { cprintf("usage: flmode <0-3|classic|classicshadows|modern|modernshadows>"); return; }
        Pc_FlashlightModeApply(mode, 1);
        cprintf("Flashlight: %s", Pc_FlashlightModeLabel(g_PcConfig.flashlightMode));
    } else if (strcmp(cmd, "SHADOWS") == 0) {
        /* Toggles shadows WITHIN the current flashlight mode. Classic + Shadows
         * without shadows is just Classic, so 1<->0 and 3<->2. */
        int on;
        int mode = g_PcConfig.flashlightMode;
        if (arg[0] == '1') on = 1;
        else if (arg[0] == '0') on = 0;
        else on = !(mode == 1 || mode == 3); /* bare "shadows" toggles */
        if (mode == 0 || mode == 1) mode = on ? 1 : 0;
        else                        mode = on ? 3 : 2;
        Pc_FlashlightModeApply(mode, 1);
        cprintf("Flashlight: %s", Pc_FlashlightModeLabel(g_PcConfig.flashlightMode));
    } else if (strcmp(cmd, "SHADOWBIAS") == 0) {
        extern float g_PsyX_FlashlightShadowBias;
        if (arg[0] != '\0') g_PsyX_FlashlightShadowBias = (float)atof(arg);
        cprintf("shadow bias = %.5f", g_PsyX_FlashlightShadowBias);
    } else if (strcmp(cmd, "SHADOWSTRENGTH") == 0) {
        extern float g_PsyX_FlashlightShadowStrength;
        if (arg[0] != '\0') g_PsyX_FlashlightShadowStrength = (float)atof(arg);
        cprintf("shadow strength = %.3f (1=full/default, lower=softer)", g_PsyX_FlashlightShadowStrength);
    } else if (strcmp(cmd, "SHADOWFADE") == 0) {
        extern float g_PsyX_FlashlightShadowFadeDist;
        if (arg[0] != '\0') g_PsyX_FlashlightShadowFadeDist = (float)atof(arg);
        cprintf("shadow contact-fade = %.1f (0=off; >0 = view units behind occluder)", g_PsyX_FlashlightShadowFadeDist);
    } else if (strcmp(cmd, "SHADOWNORMAL") == 0) {
        extern float g_PsyX_FlashlightShadowNormalOffset;
        if (arg[0] != '\0') g_PsyX_FlashlightShadowNormalOffset = (float)atof(arg);
        cprintf("shadow normal-offset = %.5f (0=off)", g_PsyX_FlashlightShadowNormalOffset);
    } else if (strcmp(cmd, "SHADOWFPSDROP") == 0) {
        extern float g_PsyX_FlashlightShadowFpsDrop;
        if (arg[0] != '\0') g_PsyX_FlashlightShadowFpsDrop = (float)atof(arg);
        cprintf("FPS shadow-light drop = %.1f", g_PsyX_FlashlightShadowFpsDrop);
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
        cprintf("ADSR envelope %s (default on; config key adsr)", on ? "ON" : "OFF");
    } else if (strcmp(cmd, "FOV") == 0) {
        /* First-person FOV (degrees, horizontal on the 4:3 frame). Same value
         * as the launcher slider / PC options row; persists to config.cfg.
         * `fov default` restores the game's native projection (71.1 = H 224). */
        if (arg[0] != '\0') {
            float v;
            if (strcmp(arg, "DEFAULT") == 0 || strcmp(arg, "default") == 0)
                v = 71.1f;
            else
                v = (float)atof(arg);
            if (v < 55.0f)  v = 55.0f;
            if (v > 110.0f) v = 110.0f;
            g_PcConfig.fpsFov = v;
            {
                char buf[16];
                snprintf(buf, sizeof(buf), "%.1f", v);
                PcConfig_SaveKeyValue("fps_fov", buf);
            }
        }
        cprintf("first-person FOV %.1f deg (71.1 = original; applies in FPS gameplay only)",
                g_PcConfig.fpsFov);
    } else if (strcmp(cmd, "TPSFOV") == 0) {
        /* Thirdperson / Over-the-Shoulder FOV. Same value as the launcher slider;
         * persists to config.cfg. The Classic cameras are never affected. */
        if (arg[0] != '\0') {
            float v;
            if (strcmp(arg, "DEFAULT") == 0 || strcmp(arg, "default") == 0)
                v = 71.1f;
            else
                v = (float)atof(arg);
            if (v < 55.0f)  v = 55.0f;
            if (v > 110.0f) v = 110.0f;
            g_PcConfig.tpsFov = v;
            {
                char buf[16];
                snprintf(buf, sizeof(buf), "%.1f", v);
                PcConfig_SaveKeyValue("tps_fov", buf);
            }
        }
        cprintf("thirdperson FOV %.1f deg (71.1 = the game's own FOV; applies in TPS/OTS gameplay only)",
                g_PcConfig.tpsFov);
    } else if (strcmp(cmd, "TPSAIMZOOM") == 0) {
        /* How far the TPS/OTS camera dollies in while aiming, 0-200% of the zoom
         * range. 100 (default) = the old full zoom, 200 = a deeper 2x zoom, 0 =
         * no zoom (what the old tps_aim_zoom = 0 checkbox did). */
        if (arg[0] != '\0') {
            float v = (float)atof(arg);
            if (v < 0.0f)   v = 0.0f;
            if (v > 200.0f) v = 200.0f;
            g_PcConfig.tpsAimZoom = v;
            {
                char buf[16];
                snprintf(buf, sizeof(buf), "%.0f", v);
                PcConfig_SaveKeyValue("tps_aim_zoom_amount", buf);
            }
        }
        cprintf("TPS/OTS aim zoom %.0f%% (100 = original full zoom, 200 = 2x zoom, 0 = none)",
                g_PcConfig.tpsAimZoom);
    } else if (strcmp(cmd, "TPSOTSAIM") == 0) {
        int on = (arg[0] == '1') ? 1 : (arg[0] == '0') ? 0 : !g_PcConfig.tpsOtsAim;
        g_PcConfig.tpsOtsAim = on;
        PcConfig_SaveKeyValue("tps_ots_aim", on ? "1" : "0");
        cprintf("OTS aiming in thirdperson %s (default on; camera eases over the shoulder while aiming)",
                on ? "ON" : "OFF");
    } else if (strcmp(cmd, "CAMCOLLIDE") == 0) {
        int on = (arg[0] == '1') ? 1 : (arg[0] == '0') ? 0 : !g_PcConfig.tpsCameraCollision;
        g_PcConfig.tpsCameraCollision = on;
        PcConfig_SaveKeyValue("tps_camera_collision", on ? "1" : "0");
        cprintf("thirdperson camera collision %s (off = the camera may pass through walls)",
                on ? "ON" : "OFF");
    } else if (strcmp(cmd, "REVSCALE") == 0) {
        extern void  PsyX_SPUAL_SetReverbDepthScale(float scale);
        extern float PsyX_SPUAL_GetReverbDepthScale(void);
        if (arg[0] != '\0') {
            float s = (float)atof(arg);
            if (s < 0.0f) s = 0.0f;
            if (s > 8.0f) s = 8.0f;
            PsyX_SPUAL_SetReverbDepthScale(s);
        }
        cprintf("reverb depth->wet scale %.2f (persist via config reverb_scale)",
                PsyX_SPUAL_GetReverbDepthScale());
    } else if (strcmp(cmd, "KF") == 0 || strcmp(cmd, "KEYFRAME") == 0) {
        extern int g_DebugAnimKfView;
        extern int g_DebugAnimKf;
        extern int g_DebugAnimKfMax;
        int maxKf = g_DebugAnimKfMax > 0 ? g_DebugAnimKfMax - 1 : 0;
        if (arg[0] != '\0') {
            int v = atoi(arg);
            if (v < 0) v = 0;
            g_DebugAnimKf     = v;
            g_DebugAnimKfView = 1; /* setting a frame implies viewing it */
            cprintf("keyframe %d (max %d) - K toggles, , . step", g_DebugAnimKf, maxKf);
        } else {
            cprintf("keyframe %d / %d (view %s)", g_DebugAnimKf, maxKf,
                    g_DebugAnimKfView ? "ON" : "OFF");
        }
    } else if (strcmp(cmd, "FLINTENSITY") == 0 || strcmp(cmd, "FLINT") == 0) {
        extern float g_PsyX_FlashlightIntensity, g_PsyX_FlashlightIntensityFps;
        extern int   g_PcFpsCam;
        /* Tune the active set: FPS mode has its own flashlight brightness. */
        float* pv = g_PcFpsCam ? &g_PsyX_FlashlightIntensityFps : &g_PsyX_FlashlightIntensity;
        const char* pkey = g_PcFpsCam ? "flashlight_intensity_fps" : "flashlight_intensity";
        if (arg[0] != '\0') {
            char buf[16];
            float v = (float)atof(arg);
            if (v < 0.0f) v = 0.0f;
            if (v > 3.0f) v = 3.0f;
            *pv = v;
            snprintf(buf, sizeof(buf), "%.2f", v);
            PcConfig_SaveKeyValue(pkey, buf);
        }
        cprintf("flashlight intensity%s: %.2f (0..3)", g_PcFpsCam ? " (fps)" : "", *pv);
    } else if (strcmp(cmd, "POSTINTENSITY") == 0 || strcmp(cmd, "POSTINT") == 0) {
        extern float g_cfg_postProcessIntensity;
        if (arg[0] != '\0') {
            char buf[16];
            float v = (float)atof(arg);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            g_cfg_postProcessIntensity = v;
            snprintf(buf, sizeof(buf), "%.2f", v);
            PcConfig_SaveKeyValue("post_process_intensity", buf);
        }
        cprintf("post-process intensity: %.2f (0..1)", g_cfg_postProcessIntensity);
    } else if (strcmp(cmd, "TMINTENSITY") == 0 || strcmp(cmd, "TMINT") == 0) {
        extern float g_cfg_tonemapIntensity;
        if (arg[0] != '\0') {
            char buf[16];
            float v = (float)atof(arg);
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            g_cfg_tonemapIntensity = v;
            snprintf(buf, sizeof(buf), "%.2f", v);
            PcConfig_SaveKeyValue("tonemap_intensity", buf);
        }
        cprintf("tonemap intensity: %.2f (0..1)", g_cfg_tonemapIntensity);
    } else if (strcmp(cmd, "XAVOLUME") == 0 || strcmp(cmd, "XAVOL") == 0) {
        extern float g_PcXaVolume;
        if (arg[0] != '\0') {
            float pct = (float)atof(arg);
            if (pct < 0.0f)   pct = 0.0f;
            if (pct > 100.0f) pct = 100.0f;
            PcConfig_ApplyXaVolume(pct / 100.0f);
        }
        cprintf("xa (fmv/voice) volume: %.0f%% (0..100)", g_PcXaVolume * 100.0f);
    } else {
        DbgOverlay_PushLine("Command not found!");
    }
}
