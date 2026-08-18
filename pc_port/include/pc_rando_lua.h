/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_RANDO_LUA_H
#define PC_RANDO_LUA_H

/* Barebones randomizer scripting (Lua 5.4).
 *
 * On each area entry, if a run is live, runs gamedata/scripts/<levelname>.lua
 * (e.g. map5_s01.lua) when present, exposing a `rando` table:
 *
 *   rando.get(key)                  -> current value of a settings key
 *   rando.set(key, value)           -> set a settings key (clamped)
 *   rando.area()                    -> areas entered so far
 *   rando.map()                     -> current level name string
 *   rando.chance(percent)           -> true `percent`% of the time
 *   rando.log(msg)                  -> write to the game log
 *   rando.player_has(itemId)        -> true if the item is in the inventory
 *   rando.spawn_monster(id, x, z [, state]) -> live-spawn a monster; slot or -1
 *
 * A script may define global on_load() (called once after it loads) and
 * on_update() (called once per gameplay frame). Only base/string/table/math are
 * available — no io/os/require, so a downloaded script can't touch the machine.
 *
 * Everything here is a no-op when built without Lua (SH_LUA) or when the
 * randomizer mode is off.
 */

void Pc_RandoLua_OnMapLoad(int mapIdx);
void Pc_RandoLua_OnUpdate(void);
void Pc_RandoLua_Shutdown(void);

#endif /* PC_RANDO_LUA_H */
