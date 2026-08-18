/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Barebones randomizer scripting layer (Lua 5.4). See pc_rando_lua.h.
 *
 * A fresh interpreter is built for each area so script state never leaks between
 * areas; persistent effects go through the `rando` settings/run API. Only the
 * safe standard libraries are opened (base/string/table/math) — no io, os or
 * require, so a script dropped in by a mod cannot read files, spawn processes or
 * load native code.
 */

#include "pc_rando_lua.h"

#ifdef SH_LUA

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "game.h"
#include "map_registry.h"
#include "pc_rando.h"
#include "pc_rando_config.h"
#include "sh_log.h"

static lua_State* s_L;
static int        s_hasUpdate;

/* Proximity pickups registered by rando.spawn_item, polled each frame. Reset per
 * area (the interpreter is rebuilt per area). Coords/radius are q19_12 world units. */
#define LUA_MAX_ITEMS 16
static struct { int itemId, x, z, radius, count, active; } s_items[LUA_MAX_ITEMS];
static int s_itemCount;

/* Private RNG for rando.chance so it never disturbs the game's Rng_* streams. */
static unsigned int s_seed;
static unsigned int lua_rng(void)
{
    if (s_seed == 0)
        s_seed = (unsigned int)time(NULL) | 1u;
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 17;
    s_seed ^= s_seed << 5;
    return s_seed;
}

/* ------------------------------------------------------------ rando.* funcs */

static int l_get(lua_State* L)
{
    const s_RandoSetting* s = Pc_RandoConfig_ByKey(luaL_checkstring(L, 1));
    if (s)
        lua_pushinteger(L, *s->value);
    else
        lua_pushnil(L);
    return 1;
}

static int l_set(lua_State* L)
{
    const char* key = luaL_checkstring(L, 1);
    int v = (int)luaL_checkinteger(L, 2);
    const s_RandoSetting* s = Pc_RandoConfig_ByKey(key);
    if (s)
        *s->value = Pc_RandoConfig_Clamp(s, v);
    return 0;
}

static int l_area(lua_State* L)
{
    lua_pushinteger(L, Pc_Rando_AreaNumber());
    return 1;
}

static int l_map(lua_State* L)
{
    int m = Pc_Rando_CurrentMapIdx();
    lua_pushstring(L, (m >= 0) ? MapRegistry_GetName((e_MapIdx)m) : "");
    return 1;
}

static int l_log(lua_State* L)
{
    SH_LOG("[SCRIPT] %s", luaL_checkstring(L, 1));
    return 0;
}

static int l_chance(lua_State* L)
{
    int pct = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, (int)(lua_rng() % 100u) < pct);
    return 1;
}

static int l_player_has(lua_State* L)
{
    lua_pushboolean(L, Pc_Rando_PlayerHasItem((int)luaL_checkinteger(L, 1)));
    return 1;
}

/* rando.spawn_monster(charaId, x, z [, stateStep]). Coords are the game's q19_12
 * world units (same space as the map's own points). */
static int l_spawn_monster(lua_State* L)
{
    int id = (int)luaL_checkinteger(L, 1);
    int x  = (int)luaL_checkinteger(L, 2);
    int z  = (int)luaL_checkinteger(L, 3);
    int st = (int)luaL_optinteger(L, 4, -1);
    lua_pushinteger(L, Pc_Rando_ScriptSpawnMonster(id, x, z, st));
    return 1;
}

/* rando.give_item(itemId [, count]) - straight into the inventory. */
static int l_give_item(lua_State* L)
{
    Pc_Rando_GiveItem((int)luaL_checkinteger(L, 1), (int)luaL_optinteger(L, 2, 1));
    return 0;
}

/* rando.spawn_item(itemId, x, z [, radius [, count]]) - a proximity pickup at
 * world coords (q19_12). When the player comes within radius it's added to the
 * inventory and, if defined, global on_item(itemId) is called. Barebones: no
 * world model yet, so pair it with a spawn_monster marker if you want it seen. */
static int l_spawn_item(lua_State* L)
{
    if (s_itemCount >= LUA_MAX_ITEMS)
    {
        lua_pushinteger(L, -1);
        return 1;
    }
    s_items[s_itemCount].itemId = (int)luaL_checkinteger(L, 1);
    s_items[s_itemCount].x      = (int)luaL_checkinteger(L, 2);
    s_items[s_itemCount].z      = (int)luaL_checkinteger(L, 3);
    s_items[s_itemCount].radius = (int)luaL_optinteger(L, 4, 8192); /* ~2 units */
    s_items[s_itemCount].count  = (int)luaL_optinteger(L, 5, 1);
    s_items[s_itemCount].active = 1;
    lua_pushinteger(L, s_itemCount);
    s_itemCount++;
    return 1;
}

static const luaL_Reg RANDO_FUNCS[] = {
    { "get",           l_get },
    { "set",           l_set },
    { "area",          l_area },
    { "map",           l_map },
    { "log",           l_log },
    { "chance",        l_chance },
    { "player_has",    l_player_has },
    { "spawn_monster", l_spawn_monster },
    { "give_item",     l_give_item },
    { "spawn_item",    l_spawn_item },
    { NULL, NULL }
};

/* ------------------------------------------------------------- lifecycle */

static void lua_teardown(void)
{
    if (s_L)
    {
        lua_close(s_L);
        s_L = NULL;
    }
    s_hasUpdate = 0;
    s_itemCount = 0;
}

static void open_safe_libs(lua_State* L)
{
    luaL_requiref(L, LUA_GNAME,       luaopen_base,   1);
    luaL_requiref(L, LUA_TABLIBNAME,  luaopen_table,  1);
    luaL_requiref(L, LUA_STRLIBNAME,  luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math,   1);
    lua_pop(L, 4);
}

void Pc_RandoLua_OnMapLoad(int mapIdx)
{
    char path[128];
    const char* name;
    FILE* probe;

    lua_teardown();

    if (!Pc_Rando_Active() || mapIdx < 0)
        return;

    name = MapRegistry_GetName((e_MapIdx)mapIdx);
    if (name == NULL || name[0] == '\0')
        return;

    snprintf(path, sizeof(path), "gamedata/scripts/%s.lua", name);
    probe = fopen(path, "rb");
    if (probe == NULL)
        return; /* no script for this level - the common case, stay silent */
    fclose(probe);

    s_L = luaL_newstate();
    if (s_L == NULL)
        return;
    open_safe_libs(s_L);

    luaL_newlib(s_L, RANDO_FUNCS);
    lua_setglobal(s_L, "rando");

    if (luaL_dofile(s_L, path) != LUA_OK)
    {
        SH_LOG("[SCRIPT] %s error: %s", path, lua_tostring(s_L, -1));
        lua_teardown();
        return;
    }
    SH_LOG("[SCRIPT] ran %s", path);

    lua_getglobal(s_L, "on_load");
    if (lua_isfunction(s_L, -1))
    {
        if (lua_pcall(s_L, 0, 0, 0) != LUA_OK)
        {
            SH_LOG("[SCRIPT] on_load error: %s", lua_tostring(s_L, -1));
            lua_pop(s_L, 1);
        }
    }
    else
    {
        lua_pop(s_L, 1);
    }

    lua_getglobal(s_L, "on_update");
    s_hasUpdate = lua_isfunction(s_L, -1);
    lua_pop(s_L, 1);
}

void Pc_RandoLua_OnUpdate(void)
{
    if (s_L == NULL)
        return;
    if (!Pc_Rando_Active() || g_GameWork.gameState != GameState_InGame)
        return;

    /* Proximity pickups (rando.spawn_item): give the item + fire on_item() when
     * the player reaches one. */
    if (s_itemCount > 0)
    {
        int px = g_SysWork.playerWork.player.position.vx;
        int pz = g_SysWork.playerWork.player.position.vz;
        int i;
        for (i = 0; i < s_itemCount; i++)
        {
            long long dx, dz;
            if (!s_items[i].active)
                continue;
            dx = (long long)px - s_items[i].x;
            dz = (long long)pz - s_items[i].z;
            if (dx * dx + dz * dz > (long long)s_items[i].radius * s_items[i].radius)
                continue;

            s_items[i].active = 0;
            Pc_Rando_GiveItem(s_items[i].itemId, s_items[i].count);
            lua_getglobal(s_L, "on_item");
            if (lua_isfunction(s_L, -1))
            {
                lua_pushinteger(s_L, s_items[i].itemId);
                if (lua_pcall(s_L, 1, 0, 0) != LUA_OK)
                {
                    SH_LOG("[SCRIPT] on_item error: %s", lua_tostring(s_L, -1));
                    lua_pop(s_L, 1);
                }
            }
            else
            {
                lua_pop(s_L, 1);
            }
        }
    }

    if (!s_hasUpdate)
        return;

    lua_getglobal(s_L, "on_update");
    if (lua_pcall(s_L, 0, 0, 0) != LUA_OK)
    {
        SH_LOG("[SCRIPT] on_update error: %s", lua_tostring(s_L, -1));
        lua_pop(s_L, 1);
        s_hasUpdate = 0; /* stop hammering a broken update */
    }
}

void Pc_RandoLua_Shutdown(void)
{
    lua_teardown();
}

#else /* !SH_LUA */

void Pc_RandoLua_OnMapLoad(int mapIdx) { (void)mapIdx; }
void Pc_RandoLua_OnUpdate(void) {}
void Pc_RandoLua_Shutdown(void) {}

#endif /* SH_LUA */
