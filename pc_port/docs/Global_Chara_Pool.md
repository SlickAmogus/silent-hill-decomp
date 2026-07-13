# Global Character/Asset Pool (`global_chara_pool`)

QOL feature (2026-07-13): every map can spawn every monster. Vanilla PSX loads
only ~3 monster types per map (`charaGroupIds`, `CHARA_GROUP_COUNT=4` incl.
Harry); the pool makes ALL chara assets resident PC-side and provides AI update
funcs from a shared pseudo-map DLL. Gameplay is untouched: native types keep
their native slots/AI/VRAM parcels, natural spawns still come only from each
map's authored spawn tables. With `global_chara_pool = 0` behavior is
byte-identical to before.

## Why a monster needs three registries (the visibility contract)

1. **Model+texture** — `g_WorldGfxWork.registeredCharaModels[charaId]`
   (+`isLoaded`): draw bails at `func_8003DA9C` if NULL.
2. **Anim data** — `g_CharaAnimDataIdxs[charaId] != 0xFF` →
   `g_CharaModelAnimsData[idx]` (skeleton/bone coords).
3. **AI** — `g_MapOverlayHdr.charaUpdateFuncs[charaId]`, compiled into the map
   DLL. NULL = render-only statue (PC fallback).

## Design

### Registry 1+2: PC-side asset pool (`pc_port/src/pc_chara_pool.c`)

- Per charaId 2..43: malloc'd ILM + ANM buffers (region-correct sizes via
  `Fs_GetFileSectorAlignedSize` at runtime), a pool-owned `s_CharaModel`, and a
  static 57-entry `GsCOORDINATE2` bone array. Loaded once on first map load
  (game_load case 6, queue idle), ~1.3 MB total. The vanilla PSX-RAM regions
  (~199 KB anim window, ~32 KB LM window) cannot hold this — all monster ANMs
  alone are ~1.02 MB.
- Anim slots: `g_CharaModelAnimsData` grows from 4 to `4+Chara_Count` entries
  (PC only). Pool slot for charaId = `PC_CHARA_ANIM_SLOT(id) = 4+id`. Loads go
  through the vanilla `Fs_CharaAnimDataAlloc(idx, id, explicitBuf,
  explicitCoords)` path (same mechanism map7_s03's boss rush uses), so
  `Fs_CharaAnimDataUpdate` fills the entry and `g_CharaAnimDataIdxs`. The
  vanilla overlap sweep and bump-chain only ever touch slots 1..3 — pool slots
  are invisible to them.
- Textures: monster TIMs cannot be VRAM-resident together (only 4 slot-keyed
  parcels exist, tpages 28/29; all monsters collide positionally). Instead each
  pool chara gets a **virtual GL slot** (existing resident-textures machinery):
  synthetic desc `clutX=((256+id)%64)*16, clutY=512+((256+id)/64)*16` routes
  `Fs_QueuePostLoadTim` to `HiresOverride_PoolSlotRegister` — no VRAM bytes,
  no draw-path changes (`ApplyHiresOverride` decodes bit-15 cluts for every
  textured prim already, independent of `resident_textures`).
  - `HIRES_POOL_SLOT_MAX` 256→512; chara ids live at `256+charaId`. CLUT rows
    ≥16 spill into slot `+64*(row/16)` (encoding-inherent); registration now
    fills the spill slots for chara-range ids (PuppetNurse PRS.TIM ships 48
    rows). `HiresOverride_PoolSlotsReset` (map init) preserves the chara range
    — chara slots persist for the process lifetime.
- Per-map refresh (`Pc_CharaPool_Refresh`): for every pooled id, if
  `registeredCharaModels[id]` is NULL → point at the pool model; if
  `g_CharaAnimDataIdxs[id]` is stale (0xFF or a 1..3 slot now owned by another
  chara) → point at the pool anim slot. Native registrations always win (maps
  keep PSX-exact behavior + per-map AI variants). Re-checks `CHARA_FILE_INFOS`
  file idxs and reloads on mismatch (JPN per-map GreyChild/Mumbler swap, PAL
  censorship patch, ending *_LAST.ANM retargets).

### Registry 3: `chara_global.dll` (pseudo-map with every portable monster AI)

- `src/maps/chara_global/`: one wrapper TU per family (same
  `#include "../src/maps/characters/X.c"` pattern as real maps), compiled with
  NO `MAPx_Sxx` define → every `#if` picks the generic/"wild" variant (stalker
  full variant, air screamer wild, groaner standard tuning).
- Covers: AirScreamer/NightFlutter, Groaner/Wormhead, LarvalStalker,
  Stalker/GreyChild/Mumbler, HangedScratcher, Creeper, Romper, SplitHead,
  Floatstinger, PuppetNurse/DummyNurse, PuppetDoctor/DummyDoctor, Bloodsucker,
  MonsterCybil. Support TUs: particle_acid.c, unk_draw.c, plus
  `chara_global_data.c` with single hand-picked copies of the DLL-local
  extracted tables (air screamer sharedData_800EEAC4_2_s00, romper
  sharedData_800EC*_2_s02 set, monster_cybil D_800EA7xx, split_head
  sharedData_800D5xxx_1_s05, floatstinger D_800D780C family). NEVER bulk-
  include multiple maps' _extracted_data.c (multiple definitions).
- **Excluded (map-bound, v1)**: Twinfeeler (live code inline in map4_s03.c with
  runtime hot-swap), Incubus + Unknown23 (map7_s03 boss-FX function family),
  LockerDeadBody (map1_s03-local). Chicken(13) has complete disc assets but no
  AI anywhere — pool makes it visible as a statue; real AI = separate feature.
  Cutscene actors (26+) get assets pooled but no AI backfill in v1.
- Loaded once at boot with its OWN `DllLoader_Open` handle, never closed
  (`MapOverlay_Load`'s single-slot handle would FreeLibrary it on the next map
  transition, killing live function pointers).
- **Backfill**: at the end of `MapRegistry_Load`, every `charaUpdateFuncs[id]`
  slot the active map leaves NULL is filled from the global header. NULL-only:
  native maps keep their native variant; the map4_s03/map7_s03 runtime
  hot-swaps happen after load into non-NULL slots and are untouched.

### Guards (bugs the pool would otherwise amplify)

- `Chara_SpawnFlagsSet/PositionSet` index `charaSpawnInfos[idx-1]` with
  `idx = g_CharaAnimDataIdxs[charaId]`; valid rows are 0..1. Pool idxs (4+)
  and the latent vanilla slot-3 case would write OOB into `cameraPaths` → PC
  guard skips row > 1.
- Killing a console-spawned NPC ran `Savegame_EnemyStateUpdate` with
  `field_40 = npcIdx`, permanently clearing an unrelated NATIVE spawn slot's
  alive-bit in the save (and its `field_228C` session bit). Debug/pool spawns
  are now tracked (`g_PcNpcDebugSpawned[]`) and skip the savegame bookkeeping.

## Post-review notes (2026-07-13 adversarial review)

- **pool=0 byte-exactness has ONE intentional exception**: killing a console
  `SPAWN`ed monster no longer runs the savegame enemy-state update (it reused
  `field_40` as a spawn-row and permanently dead-flagged an unrelated native
  spawn — a real corruption bug). This guard is console-only and active
  regardless of `global_chara_pool`; gameplay without console spawns is
  untouched.
- The no-map-define stalker build pairs Control_3's street-map notice radii
  (12.0/4.5/6.0) with Control_4/8/12's school-map distances (6.0/16.0/8.0) — a
  combination no retail host compiled. Foreign GreyChild/Mumbler notice and
  disengage at slightly different radii than in their home maps (dark/
  flashlight lighting state only).
- A single-palette loose/PNG replacement of a >16-row chara TIM fills only the
  base slot; prims on CLUT rows >=16 fall back to the base slot's row 0 at
  lookup (encoding spill walks back down slot-64 steps).
- chara_global.dll must never import a `sharedData_*`/`D_8*` symbol whose exe
  definition is a data_stubs.c zero stub when host maps carry real extracted
  values — that shadowing zeroed the Romper's lunge constants and the wild
  air-screamer tables until the audit copied them into chara_global_data.c.
  Re-run the audit after adding characters: dump the DLL's import table and
  cross-reference against host-map DLL exports.

## Known limitations (v1)

- **Foreign monster SFX**: monster sounds live in the per-map ambient VAB
  (SPU slot 2) and SFX ids map to bank-relative program numbers that collide
  across maps — an off-map monster plays the wrong sample or silence (shared
  BASE.VAB hits still work). Fix = extra PC-side VAB slot + per-monster bank
  load; deferred.
- No AI for Twinfeeler/Incubus/Unknown23/LockerDeadBody/actors outside their
  home maps (spawn list marks `[no-ai]`; they render as posed statues).
- `WorldGfx_CharaModelTransparentSet` (VRAM CLUT RMW ghost transparency —
  Incubus, Kaufmann, Dahlia etc.) no-ops on virtual-slot textures; only ever
  called by their home maps, where the native physical-slot load is active.

## Console

- `SPAWN <name>` now works for every pooled type in any map; `SPAWN LIST`
  shows native types plus pooled ones tagged `[pool]`. Readiness gates
  unchanged (model+anim registries) — the pool simply satisfies them.
