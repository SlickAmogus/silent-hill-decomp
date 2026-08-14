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

In the editor these appear as `sh_trigger` entities with dropdowns for the two
type fields; the rest are text, because their valid values are large enums
(`MapEvent_*`, `EventFlag_*`, `e_InvItemId`) that live in the decomp headers.

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
- Lighting is computed at runtime from baked normals and a world tint; there are
  no light entities to place.
