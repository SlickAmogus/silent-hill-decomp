# TrenchBroom level editor for Silent Hill

Silent Hill maps can be edited in TrenchBroom and loaded by the PC port without
rebuilding the disc image. This page covers the port side and the data model;
the day-to-day workflow lives in `sh1/README.md` inside the editor repo.

- Editor repo: `C:/Claude/silenthill/trenchbroom`, branch `sh1-editor`
  (a fork of TrenchBroom; the only C++ change is a native `.TIM` texture loader)
- Tools: `trenchbroom/sh1/tools/` (Python)
- Working data: `C:/Claude/silenthill/sh1editor/` (converted maps + textures)
- Compiled output: `pc_port/build/gamedata/load/BG/`

## How an edit reaches the game

`ipd2map.py` converts an area's `.IPD` chunks into a `.map`; you edit it;
`map2ipd.py` writes the changed chunks into `gamedata/load/BG/`; the loose-file
override (`allow_loose_files = 1`) makes the port read those instead of the disc
image. Chunks reload on area re-entry.

Two things do NOT travel that way:

- **Entities and cameras** are C data in `src/maps/<map>/`, compiled into the map
  DLL. `entities_to_source.py` and `cameras_to_source.py` patch the source, and
  the DLL must then be rebuilt.
- **Anything larger than the original chunk** needs `pc_big_ipd.c` (2026-08-09+),
  which grows the chunk slot. Older builds fail the size gate and quietly load
  the disc version.

## Port support this relies on

| Feature | Where |
|---|---|
| Loose-file override | `src/main/fsqueue_3.c`, `allow_loose_files` |
| Oversized chunks | `pc_port/src/pc_big_ipd.c` (see Port_Fixes_Index) |
| IPD header validation | `IpdHeader_FixOffsets_PC` - a chunk that fails is skipped and retried, so a bad compile looks like "nothing happened". `sh1/tools/validate_port_compat.py` is the converter-side twin |
| Per-CLUT-row PNG textures | `gamedata/load/BG/<SHEET>.TIM.p<NN>.png`, no palette or size limit |

## What lives where in the data

**Geometry and collision** are in the `.IPD` chunk, one file per 40x40-unit cell,
and are fully editable. Collision is SEPARATE data from the visible geometry:
deleting a wall model does not remove what blocks the player, which is why the
editor exports collision as its own editable layer.

**Props are instanced.** A model appears once and is placed by a transform, so
several copies share one mesh. Moving a whole instance edits its transform and
affects only that copy; editing the vertices affects every copy. The compiler
detects a rigid move and does the former.

**Entities, triggers, cameras and spawns** are per-map-DLL, not per-area, and
several DLLs render the same area geometry (11 share the `ER` interior set).

## Triggers

A trigger is two pieces: a **MAP_POINTS** entry giving a position (`s_MapPoint2d`,
Q19.12 X/Z, no Y), and a **MAP_EVENTS** row (`s_EventData`) saying what happens
there. The event addresses the point by index, so several events can share a spot.

`s_EventData` fields (`include/bodyprog/map/map.h`):

| Field | Meaning |
|---|---|
| `triggerType` | how the player activates it, see below |
| `activationType` | what extra input is needed |
| `pointOfInterestIdx` | index into `mapPoints` - the position |
| `requiredItemId` | `e_InvItemId` the player must use, for Item activation |
| `requiredEventFlag` | only fires once this story flag is set |
| `disabledEventFlag` | retires the trigger once set - how a taken item stays taken |
| `sysState` | `e_SysState` the event switches into; `SysState_EventCallback` runs a map function |
| `eventParam` | depends on `sysState`: a `MapMsg` id, a sound effect, an index into `mapEventFuncs`, or a `mapPoints` index for area loads |
| `mapIdx` | destination map for area loads |
| `sfxPairIdx` | index into `SFX_PAIRS` |

**`e_TriggerType`**

| Value | Behaviour |
|---|---|
| `TriggerType_None` | fires on event flags alone, no position test |
| `TriggerType_TouchAabb` | player entered an axis-aligned box |
| `TriggerType_TouchFacing` | entered AND looking at it - the usual "examine" trigger |
| `TriggerType_TouchObbFacing` | oriented box, facing required |
| `TriggerType_TouchObb` | oriented box, any facing |
| `TriggerType_EndOfArray` | terminates the array; not a real trigger |

**`e_TriggerActivationType`**

| Value | Behaviour |
|---|---|
| `TriggerActivationType_None` | automatic once in range |
| `TriggerActivationType_Exclusive` | blocks other events while active |
| `TriggerActivationType_Button` | requires a button press |
| `TriggerActivationType_Item` | requires using `requiredItemId` from the inventory |

### What `eventParam` actually means

`eventParam` is a bare 8-bit field that each system state reads differently, so
on its own it says nothing. Confirmed against `game_sys_states.c`:

| `sysState` | `eventParam` is |
|---|---|
| `SysState_Fmv` | FMV to play, file `BASE_AUDIO_FILE_IDX - eventParam` |
| `SysState_LoadOverlay`, `SysState_LoadRoom` | destination `mapPoints` index |
| `SysState_ReadMessage` | `MapMsg` id passed to `Gfx_MapMsg_Draw` |
| `SysState_SaveMenu0`, `SysState_SaveMenu1` | `SaveLocationId` |
| `SysState_EventCallback` | index into the map's own `g_MapEventFuncs` |
| `SysState_EventSetFlag` | unused - the state just sets `disabledEventFlag` |
| `SysState_EventPlaySound` | sound id, added to `Sfx_Base` |

`g_MapEventFuncs` is declared per map in `src/maps/<map>/<map>_header.c` and is
sparse - `NULL` entries are real. 43 of the maps have one, from 1 entry
(`map2_s03`) to 47 (`map7_s01`). It is the map's own list of scripted actions:
`MapEvent_CommonItemTake`, `MapEvent_DoorLocked`, `MapEvent_KeyOfOphielUse`,
`MapEvent_CutsceneCybilDeath`, and a long tail still named `func_800D84EC`.

### Editing triggers

`ipd2map` resolves all of this and writes it onto each `sh_trigger`:

- every `s_EventData` field, as a dropdown wherever the decomp declares an enum
- `requiredEventFlag` / `disabledEventFlag` pick from all 1066 `e_EventFlag`
  values, the 578 named ones listed first (`M6S03: Health Drink0  (1121)`)
- `event_param_is` - read-only, what `eventParam` means for the current state
- `event_func` - read-only, the `g_MapEventFuncs` entry this trigger calls
- `event_index` - the `MAP_EVENTS` row, which is how write-back finds it

`entities_to_source.py` writes changed fields back into
`src/maps/<map>/<map>_events_data.c`. Those rows are sparse designated
initializers, so it substitutes a field in place, inserts a missing one in
struct order, or deletes one set back to zero - and it rewrites the
`// \`MapEvent_X\`` comment beside `eventParam` so it never goes stale. An
unedited map produces a zero-byte diff.

It edits the decomp repository, not a loose game file, so it is dry-run by
default and the map DLLs must be rebuilt afterwards:

```
Run > Compile Map > "Entities + triggers: preview changes"
Run > Compile Map > "Entities + triggers: WRITE to decomp source"
cmake --build <pc_port build dir>          # with -DSH_BUILD_MAP_DLLS=ON
```

## Cameras

`vc_road_data.h` holds an array of `VC_ROAD_DATA` (capacity
`CAMERA_PATH_COUNT_MAX` = 100), terminated by an entry flagged
`VC_RD_END_DATA_F`. Each has two AABBs in Q4 (16 = 1 world unit):

- `lim_sw` - the volume that switches this camera on
- `lim_rd` - the rail the camera itself travels along

plus `cam_mv_type` (fixed angle / chase / self view), height limits and a
look-at offset. The editor shows both boxes and can create new zones, which are
inserted before the terminator.

## Coordinates

| Space | Unit |
|---|---|
| Geometry (IPD) | Q23.8, 256 = 1 world unit, **+Y is down** |
| Entities (MAP_POINTS) | Q19.12, 4096 = 1 world unit |
| Cameras (VC_ROAD_DATA) | Q4, 16 = 1 world unit |
| TrenchBroom | 64 units = 1 world unit, +Z up |

So TB = `(sh.x, sh.z, -sh.y) / 4` for geometry, `q12 / 64` for entities and
`q4 * 4` for cameras. One map cell is 2560 TB units; the collision grid is
20x20 cells of 512 TB.

## Known limits

- Collision on a MOVED wall is not recomputed automatically - move its collision
  brush too (they are in the Collision layer).
- Adding a brand new map cell needs the file table regenerating, or an extension
  to `Map_MakeIpdGrid` to scan `gamedata/load/BG/`.
- Editing a shared area (`ER`, `THR`) changes every map DLL that renders it.
- Trigger and camera edits reach the game through a map-DLL rebuild, not through
  a loose file, so they cannot be hot-loaded the way geometry can.
- `eventParam` is only validated against `g_MapEventFuncs` for
  `SysState_EventCallback`; the game clamps it to 0-63 and skips NULL entries,
  so a bad index is inert rather than a crash.
- Lighting is computed at runtime from baked normals and a world tint; there are
  no light entities to place.
