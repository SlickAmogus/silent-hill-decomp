# Randomizer settings & scripting

Two ways to shape a randomizer run: the in-game **settings panel** (quick, visual)
and **Lua scripts** (per-level logic). Both drive the same tunables, and both are
inert unless the randomizer mode is on.

## Settings panel

During a run, **tap the Map button** to open the settings panel; **hold** it to open
the real paper map (if you've found one). Navigate with the arrow keys / D-pad /
left stick, adjust with Left/Right, and it also takes the mouse (hover to select,
wheel to adjust, click an action). Closing saves any change.

Settings persist in **`gamedata/randomizer.cfg`** — a small file the game only
writes once you change something (an untouched install has none, and deleting it
restores every default). You can also edit it by hand:

| key | default | range | effect |
|-----|---------|-------|--------|
| `spawn_density`  | 100 | 0–100   | % of viable spots that get a monster (applies on the next area) |
| `monster_max`    | 30  | 1–32    | per-area monster cap (32 is the engine's hard limit) |
| `enemy_health`   | 100 | 10–10000| enemy HP scale % — applies to newly-spawned enemies |
| `weapon_damage`  | 100 | 10–10000| your weapon damage scale % — live, per hit |
| `extra_ammo`     | 30  | 0–999   | bonus handgun rounds at run start |
| `areas_to_boss`  | 10  | 1–99    | areas entered before the run ends at the boss |
| `entry_lock_sec` | 10  | 0–120   | how long the door you came in stays shut |

Weapon damage takes effect immediately; enemy health applies to enemies that spawn
after the change; spawn density/count apply when you next enter an area.

## Scripting (Lua)

Drop a script at **`gamedata/scripts/<levelname>.lua`** — e.g. `map5_s01.lua` for the
resort. It runs when that area is entered during a run. Level names match the map
folders (`map0_s00` … `map7_s03`).

A fresh interpreter is created per area (script state doesn't leak between areas;
persistent changes go through `rando.set`). Only `base`, `string`, `table` and `math`
are available — no file, OS or native-library access, so a downloaded script can't
touch your machine.

### The `rando` table

| call | returns | notes |
|------|---------|-------|
| `rando.get(key)` | number | a settings value (keys above) |
| `rando.set(key, value)` | — | set a settings value (clamped to its range) |
| `rando.area()` | number | how many areas have been entered so far |
| `rando.map()` | string | the current level name |
| `rando.chance(percent)` | bool | true `percent`% of the time |
| `rando.log(msg)` | — | write to the game log |
| `rando.player_has(itemId)` | bool | is that item in the inventory |
| `rando.spawn_monster(id, x, z [, state])` | slot / -1 | live-spawn a monster at world coords (q19_12, the map's own units) |

A script may define two globals the engine calls for you:

- `on_load()` — once, right after the script loads.
- `on_update()` — once per gameplay frame while you're in that area.

### Example — the resort gets meaner

`gamedata/scripts/map5_s01.lua`:

```lua
-- 30% of the time the resort becomes a longer, harder detour, and drops a
-- shotgun by the fountain; grabbing it wakes something up.
local armed = false

function on_load()
  if rando.chance(30) then
    rando.set("areas_to_boss", rando.get("areas_to_boss") + 3)   -- longer run
    rando.set("enemy_health",  rando.get("enemy_health") + 100)  -- tougher
    rando.log("resort event: the long way round")
    armed = true
  end
end

function on_update()
  -- when the player picks up the shotgun, spawn a Split Head at the arena.
  if armed and rando.player_has(9 --[[ shotgun ]]) then
    armed = false
    rando.spawn_monster(20 --[[ Split Head ]], 0, 0)   -- coords in q19_12 units
    rando.log("resort event: the guardian wakes")
  end
end
```

### Roadmap

This is the first cut. `spawn_item(id, x, z)` (place a pickup at coordinates) and
cutscene/event hooks are the next additions — the pieces are structured so those
slot in without changing what's here. Chara and item ids come from
`include/bodyprog/chara/chara.h` (`Chara_*`) and the item enums; the TrenchBroom
SH1 editor is the easiest way to read exact world coordinates off a map.
