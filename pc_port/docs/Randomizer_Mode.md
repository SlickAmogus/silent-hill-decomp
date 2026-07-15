# Randomizer Gamemode (`randomizer`)

A roguelike mode: New Game always opens in the police station, every door leads
somewhere random, and the run ends at the final boss with a score-picked ending.
Config key `randomizer` (default 0). Launcher: the checkbox next to the Level
dropdown, which greys the dropdown out and reads "Randomizer Enabled".

With `randomizer = 0` nothing in this document is active and the game is
unchanged.

## The rules

| | |
|---|---|
| Start | Always `map2_s04` (police station). Normal custom-map loadout + 30 extra handgun rounds. |
| Areas | `map3_s03` (Hospital Otherworld), `map5_s01` (Resort), `map6_s01` (Boat), `map2_s02` (Central SH streets), `map2_s04` |
| Minibosses | `map1_s05` (Split Head), `map4_s05` (Floatstinger) |
| Final boss | `map7_s03` |
| Doors | Rerolled per area: locked / another area / another room in this map / a miniboss / (1%) the final boss. At least one is always open. |
| Entry door | Shut for 10 s behind you. |
| Monsters | 1–5 per area from grey child, puppet nurse, romper, groaner, air screamer. The area's own monsters are removed. |
| Items | Every pickup becomes a healing item, a weapon you lack, or ammo for a gun you carry. |
| Saving | Disabled. World save points are dropped from the event table; quick save / quick load are gated in `pc_quicksave.c`. |
| BGM | One track (6, Central SH streets ambient) plays across every normal area. Minibosses and the final boss keep their own battle music. |
| Ending | Bad / Bad+ / Good / Good+ by score, forced just before the boss map loads. |

Doors are weighted so a big area stays worth exploring: a door that was an
interior room door mostly stays in-map (~70%), while a map-exit door mostly
leaves. Everything is rerolled on **every** entry — backtracking into an area you
cleared gives fresh doors, fresh monsters and fresh items.

## The three facts it is built on

Everything below was verified against the map data; they are the reason the mode
is as small as it is.

**1. A door is just an event row.** There is no door object. A door is an
`s_EventData` row in `g_MapOverlayHdr.mapEvents[]` whose `sysState` is
`SysState_LoadOverlay` (change map) or `SysState_LoadRoom` (change room).
`pointOfInterestIdx` names the doorway; `eventParam` names the arrival point. For
a room transition the row's `mapIdx` field is reused as a **BGM track index**, not
a map. So randomizing doors = rewriting rows.

**2. A door's arrival record lives in the source map but is in the destination
map's coordinate space.** `SysState_LoadArea_Update` does
`D_800BCDB0 = g_MapOverlayHdr.mapPoints[g_MapEventData->eventParam]` *before*
`GameBoot_MapLoad`, so the coordinates come out of the **source** map's table yet
describe where you land in the **destination**. Verified: map2_s04's door to
map2_s02 carries `MAP_POINTS[9]` = (−99.0, 11.0), and map2_s02's own door-trigger
for that same doorway sits at (−99.0, 11.25) in its own space.

The consequence: sending the player to an arbitrary map needs an arrival record
that *some other map* authored for its real door into that map. `gen_rando_data.py`
harvests exactly those (`RANDO_ARRIVALS`). They are designer-authored and
guaranteed to be inside the geometry — no hand-placed coordinates anywhere.

**3. A locked door is a second row on the same doorway.** It has
`sysState = SysState_EventCallback` and an `eventParam` selecting the shared
`MapEvent_DoorJammed` / `MapEvent_DoorLocked`. Those sit at `mapEventFuncs[0]`
and `[1]` — but **only in 25 of 43 maps** (`RANDO_DOORFN_MASK` records which).
That is how the mode both *recognises* a vanilla locked door (so it can unlock it)
and, because it cannot rely on the map having one, *appends its own*
`Pc_Rando_DoorLocked` handler so locking works uniformly everywhere.

## Design

The mode **never writes map-DLL data**. It copies the active `s_MapOverlayHdr`
into a static `s_hdr`, points `g_pMapOverlayHeader` at it, and rewrites
`mapEvents` / `mapEventFuncs` / `mapMessages` / `charaSpawnInfos` inside the copy.
`lang_text.c` already does this exact swap for translated messages; the randomizer
runs **after** it (tail of `GameBoot_MapLoad`) and copies from whatever is live, so
it inherits translations rather than clobbering them. Because the DLL's tables
stay pristine, every entry re-rolls from the original.

### Hooks (all `#ifdef SH_PC_PORT`, all no-ops when the mode is off)

| Site | Why |
|---|---|
| `main_pc.c` boot | `Pc_Rando_Init` — force start map + `global_chara_pool` before `MapRegistry_Init` reads them |
| `title.c` New Game | `Pc_Rando_OnNewGame` — start the run after the savegame wipe |
| `game_boot.c` `GameBoot_MapLoad` tail | `Pc_Rando_OnMapLoad` — the whole per-area rebuild. Must be the **last** header writer |
| `game_boot.c` auto-equip block | extra handgun rounds, written in place (`Inventory_AddSpecialItem` would open a second stack) |
| `game_sys_states.c` `SysState_LoadArea_Update` | `Pc_Rando_ArrivalOverride` — supply the destination's arrival record |
| `events_util.c` `Event_ItemTake` | `Pc_Rando_RemapItemTake` |
| `npc_main.c` `func_80037E78` | kill counter (the one global "an enemy died" gate; `CharaFlag_Dead` latches, so once per corpse) |
| `player_control.c` `Player_ReceiveDamage` | damage counter, after difficulty scaling |
| `game_main.c` main loop | `Pc_Rando_Update` — monster placement, entry-door relock |
| `dbg_overlay.c` | the score panel |

### Monsters

Placement waits for the **first gameplay frame**, because it needs collision data
to test for walkable floor (`func_800808AC`). But `GameBoot_InGameInit` runs
`Game_NpcRoomInitSpawn` before then — so the map's authored rows are **cleared at
load time** (`clear_native_spawns`), or the area would come up with its native
monsters and then get the random ones on top.

Candidate positions are the map's own authored spawn points and its `mapPoints`,
filtered by a walkable-ground test. That filter is load-bearing: a map's
`mapPoints` table *also* holds arrival records expressed in **other maps'**
coordinate spaces (fact 2), and the ground test is what rejects them. A ring-probe
around the player is the fallback.

`ovlEnemyStates[mapIdx]` and `field_228C` are reset on entry, or a re-entered
area would come back empty (both bitmasks persist across visits).

### Minibosses

`map1_s05` and `map4_s05` each have exactly **one** spawn row and it **is** the
boss — so `clear_native_spawns` is never called on them and no random monsters are
added. Their post-death chain is left completely intact (boss dies → AI sets a
dead flag → the post-death cutscene runs and sets an exit flag → a
`TriggerType_None` + `SysState_LoadOverlay` row fires); the mode only rewrites
**where that row lands**. The cutscene supplies the "a few seconds after it dies"
beat for free.

That row is marked `scripted` and nothing else may touch it: it has no doorway,
its `requiredEventFlag` is load-bearing, and rewriting or locking it would fire it
the instant the player arrived. This is also why `event_is_door()` treats
**`TriggerType_None` as never a door** — a door is always touch-triggered.

`map4_s05` needs two extra pushes: its fight is armed by `EventFlag_347`, which
vanilla sets via a room transition deep inside the map, and its cross-map entrance
is ~290 units from the Floatstinger. So the mode lands the player on that map's own
`mapPoints[13]` — the arrival the vanilla boss-room transition uses, 6 units from
the boss spawn — and sets 347 directly.

### Items

`Event_ItemTake` is a per-frame state machine, so the roll is **cached per
pickup**: the item must not change between the frame that spins up its 3D model and
the frame that grants it.

Weapons need a prompt. The six common pickups (3 healing, 3 ammo) have universal
messages at `MapMsgIdx` 5–10, but weapons do not — each map that ships one carries
its own string at a map-local index. So the mode appends its own prompts past the
end of the map's table. `mapMessages` carries no length at runtime, hence the
generated `RANDO_MSG_COUNT` (cross-checked in the generator: every message index
any map actually references must be < its count).

Taking a pickup latches its event flag, which retires the trigger for good — so a
second visit would find the area stripped. Nothing in the event table says which
flags are pickups, so the mode **learns** them: any flag `Event_ItemTake` is
invoked with is a pickup by definition. Those are recorded and cleared on re-entry.

Excluded from the pool: chainsaw, hyper blaster, rock drill, gas tank (by request),
plus katana and axe (Next-Fear unlockables, same spirit).

### Score and endings

```
base    = kills*100 + pickups*50
penalty = min(50%, 1% per minute) + min(50%, 1% per 10 HP taken)
score   = base * (100 - penalty) / 100
```

Thresholds: ≥3000 Good+, ≥2000 Good, ≥1000 Bad+, else Bad. All the constants are
`#define`s at the top of `pc_rando.c`.

The ending is a 2-bit truth table over `EventFlag_449` (Cybil saved) and
`EventFlag_391` (Good path), read **once** by map7_s03's `Map_WorldObjectsInit`.
`Pc_Rando_OnMapLoad` sets them when map7_s03 loads, which lands before that init
runs. 391 also selects which final boss spawns (Incubus vs Incubator), which is why
forcing the flags — rather than the ending variable — is the only correct way.
map7_s03's own events are otherwise left completely alone: the real ending chain,
ranking screen and credits all run as normal.

### BGM

The whole run plays one track (`RANDO_BGM_TRACK`, 6 = Central Silent Hill streets
ambient). BGM is sequenced SPU music that loops until something reloads it, and it
is selected two ways: the map header's `bgmIdx` at load time, and the header's
per-frame `bgmEvent` callback (which can switch tracks or duck layers by room/flag).
The mode owns both because it already installs its own header copy — for a normal
area it sets `s_hdr.bgmIdx = 6` and `s_hdr.bgmEvent = Pc_Rando_BgmEvent`, a stub
that drives track 6 on layer 1 every frame (the same layer map2_s02 uses natively)
and never switches away. **No game-code hook is needed.**

Boss maps are the deliberate exception: miniboss headers fall through with their own
`bgmEvent`/`bgmIdx`, and the final boss map returns from `Pc_Rando_OnMapLoad` before
the header is ever copied — so all three keep their battle music. The track restarts
briefly at each map load (the PC `g_GameWork.bgmIdx = None` fix forces a reload), but
that is masked by the transition; it is an ambient loop with no strong downbeat. To
change the track, edit the one `RANDO_BGM_TRACK` define.

## Regenerating the data

```
python3 pc_port/tools/gen_rando_data.py     # writes pc_port/include/pc_rando_data.h
```

Self-validating: it fails if any map's referenced message index is >= its counted
message table size. It also drops the one degenerate arrival record in the game —
map4_s05's post-Floatstinger exit to map2_s02 is a (0,0) placeholder, because
vanilla repositions the player with the death cutscene instead. Teleporting to it
would drop Harry at the world origin.

## Traps this hit (kept here so they are not reintroduced)

**A locked door must be button-activated.** `Event_Update` only demands a fresh
button edge when `activationType == TriggerActivationType_Button`. About a dozen
vanilla door rows in the pool are `TriggerType_TouchAabb` +
`TriggerActivationType_None` — they open on contact. Writing the locked-door
handler onto one of those (and clearing its flags, as a randomized door does) makes
it re-match *every frame* the player stands in the volume, and the handler freezes
player control: an unbreakable "It's locked." loop. `door_write_event` therefore
forces `Button` on `DOOR_LOCKED` and restores the row's original activation type
otherwise (`s_RandoDoor.origActivation`). `lock_entry_door` goes through
`door_write_event` for exactly this reason.

**The spawn-state reset belongs at map-load time, not on the placement frame.**
`GameBoot_InGameInit` runs its own `Game_NpcRoomInitSpawn` pass between
`Pc_Rando_OnMapLoad` and the first gameplay frame. On a miniboss map that pass
spawns the boss and sets its session bit in `field_228C`; clearing that bit
afterwards re-arms the row and spawns a *second* boss.

**Not every `GameBoot_MapLoad` is a new area.** A post-death Continue reloads the
same map in-process. Counting it would burn an area off the run and re-arm every
pickup in it (farmable score). `s_run.entryPending` — set by `Pc_Rando_OnNewGame`
and by `Pc_Rando_ArrivalOverride` when a door actually fires — is what separates
the two.

**`headerInstalled` is cleared before every early return in `Pc_Rando_OnMapLoad`.**
A stale 1 would let `Pc_Rando_Update` place monsters and lock doors using the
previous area's tables while the new map's own DLL header is live.

## Known gaps

- Off-map monster SFX are wrong or silent — a pre-existing limitation of the global
  chara pool (per-map ambient VAB, see `Global_Chara_Pool.md`).
- The appended weapon prompts are English only.
- A miniboss arena's other doors are still randomized, so `map4_s05` can be left
  without fighting. `map1_s05` has no doors at all — the boss is the only way out.
