/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_cheats.c - the Cheats and Debug pages of the quick options overlay.
 *
 * One table per page. Every row here is the SAME action the top-row debug
 * keys / console commands perform (same globals, same helpers), so a cheat
 * turned on here reads as on for the key and the console, and vice versa.
 * The overlay (pc_quick_options.c) only knows names, labels and a direction.
 */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/items.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/math/math.h"
#include "bodyprog/savegame.h"
#include "bodyprog/sound/sfx_id_enum.h"
#include "bodyprog/sound/sound_system.h"

#include "pc_cheats.h"
#include "pc_config.h"
#include "sh_log.h"

#include <stdio.h>
#include <string.h>

/* game_main.c */
extern int  g_PcGodMode;
extern int  g_DebugNoWallCollision;
extern int  g_DebugNoTarget;
extern int  g_DebugAnimKfView;
extern int  g_DebugCamEnabled;
extern int  g_DebugFogDisabled;
extern void Pc_FreeCam_Set(int on);
/* main_pc.c / dbg_overlay.c */
extern int  g_PcAllowDebugControls;
extern int  g_PcUnlimitedEnemies;
extern int  g_CollVisEnabled;
/* pc_console_cmd.c: the console SPAWN table, browsable. */
extern int         Pc_SpawnList_Count(void);
extern const char* Pc_SpawnList_Name(int i);
extern int         Pc_SpawnList_Ready(int i);
extern void        Pc_SpawnList_Spawn(int i);
/* pc_playas.c */
extern int         Pc_PlayAs_Count(void);
extern int         Pc_PlayAs_Current(void);
extern const char* Pc_PlayAs_Label(int idx);
extern const char* Pc_PlayAs_Name(int idx);
extern int         Pc_PlayAs_SetByName(const char* name, int save);

enum { CH_TOGGLE = 0, CH_ACTION, CH_PLAYAS, CH_FREECAM, CH_DEBUGKEYS, CH_SPAWN };

static int s_spawnIdx; /* CH_SPAWN: the browsed entry; confirm spawns it */

typedef struct
{
    const char* name;
    int         kind;
    int*        flag;      /* CH_TOGGLE */
    void      (*action)(void); /* CH_ACTION */
} CheatRow;

/* ---- actions (mirrors of the debug keys / console commands) ---------- */

/* Live slots only: past inventorySlotCount the table keeps stale entries
 * (the ghost-slot bug quick heal had), which would skip granting the gun. */
static int has_item(u8 id)
{
    int i;
    for (i = 0; i < g_SavegamePtr->inventorySlotCount && i < INV_ITEM_COUNT_MAX; i++)
        if (g_SavegamePtr->items[i].id_0 == id)
            return 1;
    return 0;
}

static void act_handgun_ammo(void)
{
    Inventory_AddSpecialItem(InvItemId_HandgunBullets, 15);
    Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
    SH_DBG_ECHO("[CHEAT] Added 15 handgun bullets");
}

static void act_rifle(void)
{
    int had = has_item(InvItemId_HuntingRifle);
    if (!had) Inventory_AddSpecialItem(InvItemId_HuntingRifle, 1);
    Inventory_AddSpecialItem(InvItemId_RifleShells, 30);
    Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
    SH_DBG_ECHO("[CHEAT] Added%s Rifle Shells x30", had ? "" : " Hunting Rifle +");
}

static void act_shotgun(void)
{
    int had = has_item(InvItemId_Shotgun);
    if (!had) Inventory_AddSpecialItem(InvItemId_Shotgun, 1);
    Inventory_AddSpecialItem(InvItemId_ShotgunShells, 30);
    Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
    SH_DBG_ECHO("[CHEAT] Added%s Shotgun Shells x30", had ? "" : " Shotgun +");
}

/* Same routing as debug key 6 / KILLALL: a huge damage.amount sends every
 * nearby enemy through its own real death path. */
static void act_kill_nearby(void)
{
    s_SubCharacter* hr = &g_SysWork.playerWork.player;
    int killed = 0, i;
    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        s_SubCharacter* npc = &g_SysWork.npcs[i];
        if (npc->model.charaId == Chara_None || npc->model.charaId == Chara_Harry || npc->health <= Q12(0.0f))
            continue;
        if (ABS(npc->position.vx - hr->position.vx) > Q12(50.0f) || ABS(npc->position.vz - hr->position.vz) > Q12(50.0f))
            continue;
        npc->damage.amount = Q12(99999.0f);
        killed++;
    }
    Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
    SH_DBG_ECHO("[CHEAT] Killed %d nearby enemies", killed);
}

static void act_kill_harry(void)
{
    g_SysWork.playerWork.player.health = -Q12(1.0f);
    SH_DBG_ECHO("[CHEAT] Killed Harry");
}

static void act_log_camera(void)
{
    extern void Pc_CamSnapDump(void);
    Pc_CamSnapDump();
}

static void act_log_position(void)
{
    s_SubCharacter* p = &g_SysWork.playerWork.player;
    SH_DBG_ECHO("HARRY POSITION LOGGED mapId=%d roomIdx=%d pos=(%ld,%ld,%ld) yaw=%d",
                (int)g_SavegamePtr->mapIdx, (int)g_SavegamePtr->mapRoomIdx,
                (long)p->position.vx, (long)p->position.vy, (long)p->position.vz,
                (int)p->rotation.vy);
}

/* ---- tables ----------------------------------------------------------- */

static const CheatRow s_cheats[] = {
    { "God mode",             CH_TOGGLE,  &g_PcGodMode,           NULL },
    { "Noclip",               CH_TOGGLE,  &g_DebugNoWallCollision, NULL },
    { "Enemies ignore Harry", CH_TOGGLE,  &g_DebugNoTarget,       NULL },
    { "Unlimited enemies",    CH_TOGGLE,  &g_PcUnlimitedEnemies,  NULL },
    { "Handgun bullets +15",  CH_ACTION,  NULL, act_handgun_ammo },
    { "Hunting rifle +30",    CH_ACTION,  NULL, act_rifle },
    { "Shotgun +30",          CH_ACTION,  NULL, act_shotgun },
    { "Kill nearby enemies",  CH_ACTION,  NULL, act_kill_nearby },
    { "Play as",              CH_PLAYAS,  NULL, NULL },
    { "Free camera",          CH_FREECAM, NULL, NULL },
};

static const CheatRow s_debug[] = {
    { "Debug keys (top row)", CH_DEBUGKEYS, NULL, NULL },
    { "Collision visualizer", CH_TOGGLE,  &g_CollVisEnabled,  NULL },
    { "Fog (free cam)",       CH_TOGGLE,  &g_DebugFogDisabled, NULL },
    { "Keyframe viewer (K)",  CH_TOGGLE,  &g_DebugAnimKfView, NULL },
    { "Spawn",                CH_SPAWN,   NULL, NULL },
    { "Log Harry position",   CH_ACTION,  NULL, act_log_position },
    { "Log camera shot",      CH_ACTION,  NULL, act_log_camera },
    { "Kill Harry",           CH_ACTION,  NULL, act_kill_harry },
};

static const CheatRow* row_at(int page, int idx, int* count)
{
    const CheatRow* t = (page == PC_CHEATS_PAGE_DEBUG) ? s_debug : s_cheats;
    int n = (page == PC_CHEATS_PAGE_DEBUG) ? (int)(sizeof(s_debug) / sizeof(s_debug[0]))
                                           : (int)(sizeof(s_cheats) / sizeof(s_cheats[0]));
    if (count) *count = n;
    return (idx >= 0 && idx < n) ? &t[idx] : NULL;
}

int Pc_Cheats_Count(int page)
{
    int n;
    row_at(page, 0, &n);
    return n;
}

const char* Pc_Cheats_Name(int page, int idx)
{
    const CheatRow* r = row_at(page, idx, NULL);
    return r ? r->name : "";
}

int Pc_Cheats_IsAction(int page, int idx)
{
    const CheatRow* r = row_at(page, idx, NULL);
    return r ? (r->kind == CH_ACTION) : 0;
}

const char* Pc_Cheats_Label(int page, int idx, char* buf, int bufsz)
{
    const CheatRow* r = row_at(page, idx, NULL);
    if (!r) return "";
    switch (r->kind)
    {
        case CH_TOGGLE:    return *r->flag ? "On" : "Off";
        /* Fog row stores "disabled", so present it the right way round. */
        case CH_ACTION:    return "";
        case CH_PLAYAS:    return Pc_PlayAs_Label(Pc_PlayAs_Current());
        case CH_FREECAM:   return g_DebugCamEnabled ? "On" : "Off";
        case CH_DEBUGKEYS: return g_PcAllowDebugControls ? "On" : "Off";
        case CH_SPAWN:
            snprintf(buf, bufsz, "< %s >%s", Pc_SpawnList_Name(s_spawnIdx),
                     Pc_SpawnList_Ready(s_spawnIdx) ? "" : "  (not in this map)");
            return buf;
        default:           return "";
    }
}

void Pc_Cheats_Adjust(int page, int idx, int dir)
{
    const CheatRow* r = row_at(page, idx, NULL);
    if (!r) return;
    switch (r->kind)
    {
        case CH_TOGGLE:
            *r->flag = !*r->flag;
            Sd_PlaySfx(*r->flag ? Sfx_MenuConfirm : Sfx_MenuCancel, 0, 64);
            SH_DBG_ECHO("[CHEAT] %s: %s", r->name, *r->flag ? "ON" : "OFF");
            break;
        case CH_ACTION:
            if (r->action) r->action();
            break;
        case CH_PLAYAS:
        {
            int n = Pc_PlayAs_Count();
            int i = (Pc_PlayAs_Current() + (dir < 0 ? -1 : 1) + n) % n;
            if (Pc_PlayAs_SetByName(Pc_PlayAs_Name(i), 1))
            {
                Sd_PlaySfx(Sfx_MenuMove, 0, 64);
                SH_DBG_ECHO("[CHEAT] Playing as %s", Pc_PlayAs_Label(Pc_PlayAs_Current()));
            }
            break;
        }
        case CH_FREECAM:
            Pc_FreeCam_Set(!g_DebugCamEnabled);
            Sd_PlaySfx(g_DebugCamEnabled ? Sfx_MenuConfirm : Sfx_MenuCancel, 0, 64);
            break;
        case CH_SPAWN:
        {
            int n = Pc_SpawnList_Count();
            if (n > 0) s_spawnIdx = (s_spawnIdx + (dir < 0 ? -1 : 1) + n) % n;
            Sd_PlaySfx(Sfx_MenuMove, 0, 64);
            break;
        }
        case CH_DEBUGKEYS:
            g_PcAllowDebugControls = !g_PcAllowDebugControls;
            g_PcConfig.allowDebugControls = g_PcAllowDebugControls;
            PcConfig_SaveKeyValue("allow_debug_controls", g_PcAllowDebugControls ? "1" : "0");
            Sd_PlaySfx(g_PcAllowDebugControls ? Sfx_MenuConfirm : Sfx_MenuCancel, 0, 64);
            SH_DBG_ECHO("[CHEAT] Debug keys: %s", g_PcAllowDebugControls ? "ON" : "OFF");
            break;
        default:
            break;
    }
}

/* Confirm on a row: the Spawn row fires its browsed entry; every other row
 * behaves like a step up. */
void Pc_Cheats_Confirm(int page, int idx)
{
    const CheatRow* r = row_at(page, idx, NULL);
    if (!r) return;
    if (r->kind == CH_SPAWN)
    {
        if (!Pc_SpawnList_Ready(s_spawnIdx))
        {
            Sd_PlaySfx(Sfx_MenuError, 0, 64);
            SH_DBG_ECHO("[CHEAT] %s is not loaded in this map (see 'spawn list')", Pc_SpawnList_Name(s_spawnIdx));
            return;
        }
        Pc_SpawnList_Spawn(s_spawnIdx);
        Sd_PlaySfx(Sfx_MenuConfirm, 0, 64);
        SH_DBG_ECHO("[CHEAT] Spawned %s", Pc_SpawnList_Name(s_spawnIdx));
        return;
    }
    Pc_Cheats_Adjust(page, idx, +1);
}

/* List rows (Spawn): the overlay draws these as a button on the left and a
 * browsable value on the right, and can open the list as a dropdown. */
int Pc_Cheats_ListCount(int page, int idx)
{
    const CheatRow* r = row_at(page, idx, NULL);
    return (r && r->kind == CH_SPAWN) ? Pc_SpawnList_Count() : 0;
}

const char* Pc_Cheats_ListName(int page, int idx, int i)
{
    const CheatRow* r = row_at(page, idx, NULL);
    return (r && r->kind == CH_SPAWN) ? Pc_SpawnList_Name(i) : "";
}

int Pc_Cheats_ListGet(int page, int idx)
{
    const CheatRow* r = row_at(page, idx, NULL);
    return (r && r->kind == CH_SPAWN) ? s_spawnIdx : 0;
}

void Pc_Cheats_ListSet(int page, int idx, int i)
{
    const CheatRow* r = row_at(page, idx, NULL);
    if (r && r->kind == CH_SPAWN && i >= 0 && i < Pc_SpawnList_Count())
    {
        s_spawnIdx = i;
        Sd_PlaySfx(Sfx_MenuMove, 0, 64);
    }
}
