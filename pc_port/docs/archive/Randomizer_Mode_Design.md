# Doorway Randomizer Mode — Design & Effort Analysis

> Status: **SUPERSEDED — the mode shipped 2026-07-14. See `Randomizer_Mode.md`.**
>
> Kept as the historical scoping pass (2026-06-24). Several of its guesses turned
> out wrong and are corrected in the shipped doc:
>   - the per-door lock flag it could not find is a *second event row* on the same
>     doorway, not a flag;
>   - the arrival spawn point does not need harvesting "at runtime" — it is authored
>     in the source map but expressed in the destination's coordinate space, so it
>     can be harvested statically (`tools/gen_rando_data.py`);
>   - "any enemy anywhere" turned out to be free, because the global chara pool
>     (`Global_Chara_Pool.md`) landed in between and solved its Phase 3.
>
> Original text follows.

## 1. The mode (goal)

A "doorway randomizer": when enabled, every doorway/area transition sends the player
to a random *other* valid destination somewhere in the game instead of its scripted
target. The traversed door locks one-way behind the player; a room with only one door
re-unlocks after ~5 s. Enemies and item pickups are also randomized. There is a small
(~1%) chance each transition lands at the **final boss room (start of `map7_s03`)** —
the win condition is to reach and beat it by chance. The mode bootstraps from any
normal map (e.g. chasing Cheryl in `map0_s00`); the *next* door taken kicks it off.
Preferred granularity: room-by-room > map-by-map > interior-only.

## 2. How the engine actually works (findings)

### 2a. Transitions & map loading
- **Redirect choke point:** `SysState_LoadArea_Update` (`src/bodyprog/events/game_sys_states.c:822`).
  A door/area trigger sets the destination map (`g_SavegamePtr->mapIdx`) and the arrival
  spawn-point data (`D_800BCDB0`, the spawn position/rotation/index — see
  `room_transitions.md`). Overriding those two at this point redirects the player anywhere.
  This is the single best interception site for the whole mode.
- **Loader is stateless:** map overlays are swapped via `MapOverlay_Unload` → `MapOverlay_Load`
  (`pc_port/src/map_registry.c`, `map_overlay_loader.c`); the registry is a complete 1:1 set
  of all 43 overlays. **Any map can be loaded from any map** mechanically — no inherent
  ordering requirement in the loader.
- **Granularity:** the first-class unit is **map overlay (`mapN_sNN`) + spawn-point index**.
  There is no first-class "room" object. Room-by-room is achievable by **harvesting the set
  of valid spawn points** (arrival points) across all overlays and treating each as a
  destination — but that table must be built (it is not pre-collected).
- **Lock/relock:** doors have a locked state + key checks (the normal locked-door system).
  A traversed door can be made one-way by setting its locked flag on exit; a single-door
  room's 5 s re-unlock is a simple timer in the mode controller. (Implementing session:
  confirm the per-door lock flag location.)

### 2b. Enemy spawns
- Per-map data in the overlay header (`include/bodyprog/map/map.h`):
  `charaSpawnInfos[2][16]` (32 spawn points, `s_SpawnInfo` @0x24C), `charaGroupIds[4]` (@0x248),
  `charaUpdateFuncs[Chara_Count]` (@0x194, the per-`e_CharaId` AI funcptr; NULL = not hosted).
- Spawns are **map-wide, proximity-triggered** (`Game_NpcRoomInitSpawn`, `npc_main.c:66`): an
  enemy spawns when the player is within ~22 u and despawns beyond ~40 u. **Not partitioned
  by room.**
- Caps: `NPC_COUNT_MAX 6` (live cap 5), `CHARA_GROUP_COUNT 4`, only **3 NPC model/anim slots**
  (`g_CharaModelAnimsData`). So ≤4 enemy types per scene.
- **Hard constraint:** an enemy type only works in a map if (1) `charaUpdateFuncs[id]` is wired,
  (2) its model/anim/texture were `Chara_Load`ed into a slot, (3) `g_CharaAnimDataIdxs[id]` is set.
  → Default scope = **shuffle among the types a map already loads**; "any enemy anywhere" needs
  cross-map AI + asset loading (Phase 2).
- **Randomization hook:** `MapRegistry_Load` (`map_registry.c:215-249`, right after the header is
  assigned, it's writable on PC) or pre-`Game_NpcRoomInitSpawn` (`chara_init.c:104`). Rewrite
  `charaSpawnInfos[].charaId` / `charaGroupIds[]` (respect the PC 16-byte `s_SpawnInfo` stride).

### 2c. Item pickups
- World pickups are granted by `Event_ItemTake(itemId, count, eventFlag, msgIdx)`
  (`events_util.c:1062`); the takeable item's model streams **on demand by item id**
  (`GameFs_UniqueItemModelLoad` → `FS_BUFFER_5`), so the granted item is **nearly unconstrained**.
- **Randomization hook:** wrap `Event_ItemTake`, keyed on `eventFlag` (uniquely identifies each
  pickup) → remap `(mapId, eventFlag) → newItemId`. Single-site, low-risk.
- Cosmetic-only constraints (no crash): the inventory-grid thumbnail uses the per-map
  `loadableItems` IT-pack (out-of-pack = blank thumbnail, NULL-guarded); the on-ground "glint"
  decoration uses a named chunk model (may show the wrong mesh).

## 3. Risks / hazards (the real cost driver)
- **Cutscene / event-flag gating (HIGH):** many maps assume story state (flags, prior events,
  cutscene triggers) on entry. A cold jump into such a map can soft-lock, replay a cutscene, or
  spawn wrong content. → **Curate a cold-enter-safe target allowlist**; do not random into
  arbitrary overlays uniformly.
- **State carryover on a forced jump (MEDIUM):** the new map can inherit the previous map's NPCs
  via `charaSpawnInfos` + the shared `npcBoneCoordBuffer` ring, and inventory/weapon state. The
  mode must run a `GameBoot_NpcClear`-style reset (the boss-entry code already does this,
  `game_boot.c:108-114`) on every forced jump.
- **Bone-buffer overflow (MEDIUM):** swapping in a higher-bone enemy than the slot expects
  overflows `npcBoneCoordBuffer` (`NPC_BONE_COUNT_MAX`) — a known crash class. Restrict enemy
  swaps by bone-count compatibility.
- **`g_MapEventData` lifetime (LOW, mitigated):** known use-after-free with an existing PC
  snapshot workaround (`game_sys_states.c:880-896`) — verify it holds for forced jumps.
- **FS buffer reuse / same-frame eviction (LOW-MED):** documented teleport-door void/eviction
  issues (`project_void_eviction_and_tape.md`); the redirect must respect the load window.

## 4. Recommended phasing & effort

**Phase 0 — Target harvest + safety (foundation).** Build the destination table: enumerate valid
`{mapIdx, spawnPointIdx}` arrivals across overlays, then **curate a cold-enter-safe allowlist** by
testing each candidate (load it cold, confirm no soft-lock/cutscene/crash). This curation +
testing is the single biggest time sink. *Effort: MEDIUM-HIGH (dominated by per-target testing).*

**Phase 1 — Core randomizer (map granularity).** Mode flag + bootstrap; intercept
`SysState_LoadArea_Update` to redirect to a random allowlisted target; `NpcClear`-style reset on
jump; one-way lock-behind + single-door 5 s relock; 1% boss-room (`map7_s03` start) chance.
*Effort: MEDIUM.*

**Phase 2 — Content randomization.** Item randomizer (wrap `Event_ItemTake` by `eventFlag`) —
*LOW*. Enemy shuffle within each map's already-loaded types (header hook, restrict to non-NULL
`charaUpdateFuncs`, bone-compatible) — *LOW-MEDIUM*.

**Phase 3 — Stretch.** True room-by-room (harvest per-room spawn points + entry mapping) —
*MEDIUM*. "Any enemy anywhere" via cross-map AI+asset loading, bone-gated — *HIGH*.

**Overall:** a very achievable mod. The novel tech is small (the loader is stateless, the choke
point exists, the hooks are clean). The cost is **curation + testing of cold-enter-safe targets
and state-reset correctness**, not deep engine work. MVP (Phases 0-1 + item randomizer) is the
"reach the boss by accident" experience; enemy/room/any-enemy are incremental.

## 5. Open questions for the implementing session
- Exact per-door **lock flag** location + how a door reports "single door in this room."
- The precise contents/format of `D_800BCDB0` (spawn-point record) and how arrival index is chosen.
- How to **enumerate spawn points per map** for the destination table (static scan vs runtime).
- Whether interior-only filtering is cleanly derivable from map metadata.
- Confirm `NpcClear`/event-data reset fully sanitizes a forced jump (test cold enters).

## 6. Key entry points
- Redirect: `src/bodyprog/events/game_sys_states.c:822` (`SysState_LoadArea_Update`), `D_800BCDB0`.
- Map load: `pc_port/src/map_registry.c:215-249`, `pc_port/src/map_overlay_loader.c`.
- Enemy spawns: `include/bodyprog/map/map.h:388-397,522-526`; `src/bodyprog/events/npc_main.c:66`;
  `src/bodyprog/chara_spawn.c`; `src/bodyprog/game_boot/chara_init.c:54-124`,
  `fs_chara_anim.c`; `include/bodyprog/chara/chara.h:9-11,644-659`.
- Items: `src/bodyprog/events/events_util.c:1062` (`Event_ItemTake`);
  `src/bodyprog/items/item_screens_3.c:3182` (`GameFs_UniqueItemModelLoad`).
- State reset / hazards: `src/bodyprog/game_boot/game_boot.c:108-141`;
  `game_sys_states.c:880-896`. Related memory: `room_transitions.md`,
  `project_void_eviction_and_tape.md`, `project_goodplus_ending_bonebuffer.md`.
