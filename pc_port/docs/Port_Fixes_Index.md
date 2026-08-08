# PC Port — Game-Code Fixes Index

Curated index of places where running the PSX decomp on x86-64 / PsyCross
required **patching the game code** (`src/`) or substituting a game-data symbol —
i.e. deviations from the clean byte-matching decomp. PsyCross-side fixes
(`pc_port/PsyCross/`) are tracked separately.

The goal is to keep this list **short and honest**: every entry is a spot where
the clean decomp wasn't enough. Some are faithful ports (correct and permanent);
a few are band-aids that mask a deeper cause and should be revisited — those are
marked **⚠ band-aid**. Fixes of the same root cause are grouped together.

Links point at the current `pc-port` branch; line numbers may drift, so the
function name is the stable anchor. Each entry cites the commit with the full diff.

Repo: `https://github.com/SlickAmogus/silent-hill-decomp` (branch `pc-port`)

## Index
- [1. Faults PSX hardware tolerated that x86 traps on (NULL deref, div-by-zero)](#1-faults-psx-hardware-tolerated-that-x86-traps-on)
- [2. Zero-stubbed data tables → real tables](#2-zero-stubbed-data-tables--real-tables)
- [3. IPD chunk streaming & buffer sizing](#3-ipd-chunk-streaming--buffer-sizing)
- [4. Fixed-point overflow](#4-fixed-point-overflow)
- [5. 64-bit pointer width & struct layout](#5-64-bit-pointer-width--struct-layout)
- [6. High-FPS keyframe / frame-count timing (combat + cutscenes)](#6-high-fps-keyframe--frame-count-timing-combat--cutscenes)
- [7. Cutscene-specific regressions](#7-cutscene-specific-regressions)

---

## 1. Faults PSX hardware tolerated that x86 traps on

The PSX CPU silently tolerated two things that x86 turns into a hard exception:
(a) a NULL / small-integer pointer dereference reads valid low RAM and does
harmless garbage work — x86 access-violates; (b) MIPS integer `div`/rem by zero
returns garbage in LO/HI without trapping — x86 `idiv` raises `#DE`. Each guard
skips the work the bad value can't produce, matching the PSX "garbage but no
crash" outcome.

- **`Anim_BoneInit` — cat locker scene-end crash.** The cat scene's end cleanup
  (`func_800D87C0` → `Chara_BonesInit(0)`) passes `g_CharaModelAnimsData[1].activeAnmHdr`,
  which is NULL for that unloaded slot; reading `anmHdr->boneCount` (offset 6) AV'd.
  WinDbg-confirmed. Guard skips the bone loop after the root coord is initialised.
  The slot was NULL because of the duplicated chara-anim array (§7, `0d57ece35`);
  with that fixed the guard is a defensive no-op.
  [`Anim_BoneInit`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/bodyprog/gfx/bodyprog_anim_800445A4.c#L41) ·
  commit [`1a1bdda6e`](https://github.com/SlickAmogus/silent-hill-decomp/commit/1a1bdda6e)
- **NPC `playbackFunc` NULL guards.** Several `*_ANIM_INFOS[status].playbackFunc`
  entries are NULL on PC (see §2); calling through them crashed Cheryl and the cat.
  ⚠ band-aid where it hides a zero-stub table — the real fix is §2.
  [`cat.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/maps/characters/cat.c#L46) ·
  [`cheryl.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/maps/characters/cheryl.c#L59) ·
  commit [`4f24526d1`](https://github.com/SlickAmogus/silent-hill-decomp/commit/4f24526d1)
- **MIPS argument-eval-order NULL deref** in `Collision_CharaCollisionSetup` /
  `func_8006A42C`. The merge reordered an expression whose side effect must run
  before the deref; restored MIPS left-to-right order. Caused an in-game NULL deref.
  commit [`c49602177`](https://github.com/SlickAmogus/silent-hill-decomp/commit/c49602177)
- **Integer divide-by-zero — chemical-on-hand cutscene crash.** The melting-smoke
  particle updater `sharedFunc_800CBB30_1_s01` does `Rng_Rand16() % temp_s1`; a
  freshly-spawned particle has ~zero velocity so `temp_s1` (its speed) is 0 → `#DE`.
  WinDbg-confirmed. Guard the `%` and the sibling growth-term `/` (its denominator
  reaches 0 as the cos input sweeps); also fixes map6_s04 (shares the file).
  [`unk_draw_m1s01.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/maps/unk_draw_m1s01.c#L136) ·
  commit [`f7cd4ff5c`](https://github.com/SlickAmogus/silent-hill-decomp/commit/f7cd4ff5c)
- **Drain-valve cutscene crash — GTE SZ saturates to 0.** `func_800CE164`
  (map1_s03 drip drawer) divides the quad size by the projected depth; a drip
  on/behind the camera plane stores SZ 0. Skip the quad that frame.
  [`unk_draw_800CDCE0.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/maps/map1_s03/unk_draw_800CDCE0.c) ·
  commit [`ace3bc504`](https://github.com/SlickAmogus/silent-hill-decomp/commit/ace3bc504)
- **Classroom-key crash + camera-warp crash (user crash dumps).** The school
  water-drip drawer `sharedFunc_800CBDA8_1_s02` divides by emitter duration
  fields that can be 0; `vcAutoRenewalCamTgtPos` divides the warp delta by
  `g_DeltaTime`, which is 0 on pause/console/sub-hblank frames on PC.
  commit [`773bc4f29`](https://github.com/SlickAmogus/silent-hill-decomp/commit/773bc4f29)
- **Character fog vertex-color corruption — negative fogRamp index.** The
  per-vertex depths live in s16 slots; a GTE SZ ≥ 32768 reads back negative,
  passes the `< (1 << depthShift)` range test and indexes `fogRamp[]` out of
  bounds (PSX read harmless low RAM). This corruption is why Harry's gameplay
  render used to disable fog entirely — that bypass is now removed and all
  characters fog like the world. `PC_FOG_VTX_RAMP` reads the depth as u16;
  same clamp in `func_80055B74`.
  [`bodyprog_80055028.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/bodyprog/gfx/bodyprog_80055028.c) ·
  commit [`74dbdbdbd`](https://github.com/SlickAmogus/silent-hill-decomp/commit/74dbdbdbd)
- **Systematic div/rem-by-zero sweep (13 files).** Audited every division and
  modulo by a runtime value that can be zero. Same-class guards: the two
  remaining `/ g_DeltaTime` sites (`stalker.c`, `air_screamer.c`); two more
  unguarded GTE-SZ divisions (`func_80064FC0`, `bodyprog_800652F4.c`); glass
  shard spin `% (mag >> 2)` which faults once a settling shard's lateral speed
  drops below 4 (M0S01 alley / M7S01); spawner/ribbon divisions by emitter
  config fields in `particle_water.c` (plus a scratch-overrun cap on the
  ribbon's unbounded row loop), `unk_draw.c`, `unk_draw_m1s01.c`,
  `unk_draw_800CDCE0.c`, `particle_acid.c`; collision ray walk `/ subcellSize`
  and point-ray slope in `ray.c`; LOS `% spanAngleStep` in `los.c`; snow
  spawn-box corner `% temp_s3` in `particle.c`.
  commit [`0aa67ef1e`](https://github.com/SlickAmogus/silent-hill-decomp/commit/0aa67ef1e)

## 2. Zero-stubbed data tables → real tables

During the port, data symbols with no decompiled definition were emitted as
`u8 X[256] = {0}` zero-stubs in [`pc_port/src/stubs/data_stubs.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/pc_port/src/stubs/data_stubs.c).
For animation-info tables that means every `playbackFunc` is NULL → the character's
animation never advances (crash, or a cutscene that waits forever). Fix: extract the
real table into `pc_port/src/<name>_anim_infos.c` and init it at startup
(MinGW rejects cross-module function pointers in static initialisers, so an
`*_Init()` runs from `main_pc.c`).

- **`CAT_ANIM_INFOS` — cat locker cutscene froze.** Was a zero-stub; the cat never
  animated so the scene never ended. Real table from `cat.h` (PSX `0x800DC924`).
  [`cat_anim_infos.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/pc_port/src/cat_anim_infos.c) ·
  commit [`f3f5d354a`](https://github.com/SlickAmogus/silent-hill-decomp/commit/f3f5d354a)
- **Earlier members of the same family** (already real): `DAHLIA_`, `GROANER_`,
  `CREEPER_`, `BLOODSUCKER_`, `BLOODY_LISA_`, `ALESSA_`, `KAUFMANN_`,
  `HANGED_SCRATCHER_`, `LARVAL_STALKER_`, … in `pc_port/src/*_anim_infos.c`.
  Pattern to spot the next one: an invisible / frozen / crashing NPC whose symbol
  is still a `u8[256]={0}` in `data_stubs.c`.
- **BGM layer tables — layered BGM silent on most maps.** `Bgm_Update` applies
  per-room layer-limit caps as `(vol * limit) >> 7`; the limit tables
  (`s_BgmLayerLimits`, `u8[8]`) and per-room layer-flag tables (`u16[rooms]`)
  for ~25 maps were zero-stubs, so every layer (including the always-on base
  layer 0) multiplied to 0 and the player muted itself two frames after
  `SdSeqPlay` (school: seq 786 started then `SD_Call(18)` stop). Extracted real
  tables from the PSX overlay binaries via
  [`extract_map_data.py`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/pc_port/tools/extract_map_data.py)
  (`TARGETS` + `EXTRA_SYMBOLS`; sizes tiling-verified against neighbour symbols).
  commit [`9aa8d8326`](https://github.com/SlickAmogus/silent-hill-decomp/commit/9aa8d8326)
- **Enemy melee collision data** — same zero-stub class, different data: each
  enemy's per-keyframe hitbox/collision rodata was `{0}`, so melee passed straight
  through them (un-hittable). Extracted into `src/maps/characters/*_rodata.inc`
  ([`stalker_rodata.inc`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/maps/characters/stalker_rodata.inc),
  creeper/hanged_scratcher/larval_stalker/romper/split_head/groaner). Grey-child/
  Stalker family commit [`299ccf311`](https://github.com/SlickAmogus/silent-hill-decomp/commit/299ccf311),
  Larval [`554b38e90`](https://github.com/SlickAmogus/silent-hill-decomp/commit/554b38e90),
  Creeper+Hanged [`675fdbaf7`](https://github.com/SlickAmogus/silent-hill-decomp/commit/675fdbaf7).

## 3. IPD chunk streaming & buffer sizing

The map chunk pipeline was reworked for PC (synchronous reads, wider Hor+ view,
256 active slots). Several fixes address buffers sized for PSX assumptions.

- **Interior chunk-buffer overrun — school void / exploded geometry / thrash.**
  The widescreen residency bump (activeIpdCount→4) made `Ipd_ActiveChunksClear`
  slice the fixed `0x2C000` buffer into 45 KB slots; interior chunks (sized for the
  map's original 1–2 count → 90/180 KB) overran into the neighbour, corrupting its
  header and model data (thousands of "invalid magic" reloads). Now slices by the
  map's **original** count; residency slots get their own buffers.
  [`Ipd_ActiveChunksClear`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/bodyprog/gfx/bodyprog_80040B74.c#L744) ·
  commit [`cc821b60b`](https://github.com/SlickAmogus/silent-hill-decomp/commit/cc821b60b)
- **School black-void — stale IPD buffer drawn.** After a map round-trip a chunk's
  buffer holds another map's data (bad magic) while `isLoaded` is stale-true; the
  reformat-fail path didn't clear it, so the renderer drew garbage. Force
  `isLoaded=false` on reformat fail. ⚠ partial — see §3 overrun for the real root of
  the broader thrash.
  [`IpdHeader_FixOffsets`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/bodyprog/gfx/bodyprog_80040B74.c#L1942) ·
  commit [`b87e51c14`](https://github.com/SlickAmogus/silent-hill-decomp/commit/b87e51c14)
- **`isLoaded` byte trusted before reformat** (the IPD file's byte 1 lands on
  `isLoaded` and is unreliable on PC) — the registry-based check that replaced it.
  commit [`eb819f8b6`](https://github.com/SlickAmogus/silent-hill-decomp/commit/eb819f8b6)
- **`s_MapTerrain.activeChunks[256]` / `g_Map_ActiveChunksCollisionData[256]`
  overrun** — PC uses up to 256 active chunks vs PSX 4; arrays widened.
  commits [`02d8d9ebe`](https://github.com/SlickAmogus/silent-hill-decomp/commit/02d8d9ebe),
  [`fe4b6ec76`](https://github.com/SlickAmogus/silent-hill-decomp/commit/fe4b6ec76)

## 4. Fixed-point overflow

- **`Math_Vector2MagCalc` / `Math_Vector3MagCalc` — Cheryl fence warp.** Squaring a
  Q12 directly overflows s32 for magnitudes >~11 units, breaking distance checks (a
  merge regression broke *all* long-range checks, not just the chase). Applied the
  overflow-safe Q6-intermediate form **only at the affected Cheryl chase gates**, not
  globally (the global form rounds tiny distances to 0 and breaks collision divides).
  [`math.h`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/include/bodyprog/math/math.h) ·
  [`map0_s00_2.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/maps/map0_s00/map0_s00_2.c) ·
  commits [`fc59e57de`](https://github.com/SlickAmogus/silent-hill-decomp/commit/fc59e57de),
  [`6a2fa2995`](https://github.com/SlickAmogus/silent-hill-decomp/commit/6a2fa2995)

## 5. 64-bit pointer width & struct layout

PSX `s32` fields that hold pointers truncate 64-bit addresses; merges occasionally
reverted fork pointer-width fixes back to `s32`. Symptom: faulting address like
`0xffffffff########`.

- **`s_CharaAnimData.allocAddr` — school save-load crash.** Field used as a pointer
  was `s32`; widened on PC.
  commit [`301061e86`](https://github.com/SlickAmogus/silent-hill-decomp/commit/301061e86)
- **Split Head boss crash — decompiled PSX stack-frame aliasing.** The matched
  idiom `ptr = &sp18[i * 16] + 32` encodes "sp38 sits 0x20 past sp18 in the PSX
  frame"; PC local layout differs, so the alias write smashed the stack and a
  garbage index AV'd (`map1_s05.dll+0x586E`, two user dumps, deterministic).
  PC path uses `&sp38[i]` directly. Swept all of `src/` for the
  `&local[idx * stride] + offset` signature — this was the only instance.
  [`split_head.c` `sharedFunc_800D3388_1_s05`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/maps/characters/split_head.c#L1437) ·
  commit [`e14be74b7`](https://github.com/SlickAmogus/silent-hill-decomp/commit/e14be74b7)
- General guidance on finding these lives in
  [`struct_offset_portability.md`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/pc_port/docs/struct_offset_portability.md).

## 6. High-FPS keyframe / frame-count timing (combat + cutscenes)

**The single most recurring root cause.** The game was authored for a fixed 30 FPS
tick; PC runs uncapped/faster. Any logic that gates on an *exact* keyframe
(`anim.keyframeIdx == N`) or counts raw frames silently breaks: at >30 FPS the
keyframe index steps past `N` without ever equalling it, so the gate never fires —
a shot never dispatches, an animation never ends, a cutscene waits forever.
**The fix is always one of:** use a range/`>=` check instead of `==`, or scale the
quantity by [`TIMESTEP_SCALE_30_FPS`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/include/bodyprog/math/math.h#L89)
/ [`TIMESTEP_30_FPS`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/include/game.h#L20).
Never re-introduce an `== exactKf` or a hardcoded frame counter. ⚠ Several of these
started life as frame-count band-aids that were later replaced by the proper
range/timestep form — watch for regressions reverting them.

- **Handgun/weapon fire never dispatches.** The aim→fire gate compared
  `keyframeIdx == aimKf`; on PC the pose blew past it. Now
  `SH_AIM_KF_REACHED(kf)` is `>=` ([`player_control.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/bodyprog/player_control.c#L3204)),
  fire allowed across the whole aim-HOLD window, and auto-aim target-switch
  transitions are FPS-proof. commits
  [`c14f09683`](https://github.com/SlickAmogus/silent-hill-decomp/commit/c14f09683),
  [`a0357b3dc`](https://github.com/SlickAmogus/silent-hill-decomp/commit/a0357b3dc),
  [`3b0eaf275`](https://github.com/SlickAmogus/silent-hill-decomp/commit/3b0eaf275)
- **Death / grab / get-up freeze in non-map0 maps.** A frame-count `DEATH_STALL`
  band-aid never elapsed at high FPS (and read the wrong overlay field). Replaced
  with the real `field_38` overlay-read; band-aid removed.
  ([`player_control.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/bodyprog/player_control.c) +
  `bodyprog_anim_800445A4.c`) commit
  [`8838b989c`](https://github.com/SlickAmogus/silent-hill-decomp/commit/8838b989c)
- **Cutscene timers never advance.** Events run a tick with `g_DeltaTime == 0` on
  PSX (compensated by event cadence); on PC that stalls every timer-based cutscene
  step. Feed `g_DeltaTimeRaw` during the event path instead.
  [`game_sys_states.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/bodyprog/events/game_sys_states.c#L160)
- **Melee phantom swing on release.** PSX never drains the attack shift register
  at swing end — the full hold history is what makes the tap detector reject a
  hold-release (`!(hold & 0x11)`). The PC swing-end drain zeroed it, so one refill
  tick of a continuing hold + release read as a fresh TAP → phantom slash. Now the
  register is set to 0x1F (full history) when the button is still held at swing end.
  [`player_control.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/bodyprog/player_control.c) ·
  commit [`1c5f7835e`](https://github.com/SlickAmogus/silent-hill-decomp/commit/1c5f7835e)
- **Cutscene desync from anim-stuck detectors (Cybil scene).**
  `Player_AnimPlaybackStateGet` (polled by map-event scripts) compared
  `kf == endKeyframeIdx` (skippable at PC delta-time — the real stuck root, now
  `>=`/`<=` range form on PC), and its stuck-bypass detectors counted FRAMES
  (90/120/240 ≈ 0.4–1 s at 240 fps) and never reset across anim changes — during
  long cutscenes they force-returned "finished" early, desyncing every later step.
  Detectors are now real-time (3 s/4 s/10 s) and reset per anim status. ⚠ detector
  (C) remains a band-aid for the still-open "nothing drives Harry into the pickup
  pose" root (KeyOfWoodman class).
  [`player.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/maps/characters/player.c#L659) ·
  commit [`00e1b3f3d`](https://github.com/SlickAmogus/silent-hill-decomp/commit/00e1b3f3d)

## 7. Cutscene-specific regressions

Beyond timing (§6), cutscenes hit a cluster of 64-bit / merge issues:

- **Letterbox bars don't render.** The bars were built with a *positional static
  initializer* that assumed a PSX 1-word `P_TAG`; on 64-bit the tag is wider, so
  the prim was malformed. Build them at runtime (`setcode`/`setlen`/`setXY4`),
  like `screen_fade.c`. Also keep the cinematic FOV locked during the zoom-hold.
  [`cutscene_border.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/bodyprog/screen/cutscene_border.c) ·
  commits [`b43fdb5fd`](https://github.com/SlickAmogus/silent-hill-decomp/commit/b43fdb5fd),
  [`4ce60bbc9`](https://github.com/SlickAmogus/silent-hill-decomp/commit/4ce60bbc9)
- **Harry runs/walks in place during cutscene walks.** `sharedData_800D32A0_0_s02`
  (cutscene move speed) was declared `u8`, truncating the Q12 speed to 0. Widened
  the extern + the `data_stubs.c` storage. commit
  [`bedd134b6`](https://github.com/SlickAmogus/silent-hill-decomp/commit/bedd134b6)
- **Turn-in-place during cutscenes.** The merge dropped/renamed the `HAS_PlayerState`
  defines several map headers rely on; restored. commit
  [`89fe373df`](https://github.com/SlickAmogus/silent-hill-decomp/commit/89fe373df)
- **Cutscene walk player-state corruption.** A merge reverted a fork fix in
  `player.c`; re-applied. commit
  [`81ab45503`](https://github.com/SlickAmogus/silent-hill-decomp/commit/81ab45503)
- **Lighter-hold flame detaches from the hand.** The raised-arm bone coord wasn't
  invalidated so the flame tracked a stale transform; force the arm-bone `flg`.
  commit [`393faf46d`](https://github.com/SlickAmogus/silent-hill-decomp/commit/393faf46d)
- **Walk/sidestep in place + no wall smack — fused speed/distance fields.** The
  fork predated upstream's rename of the player speed field: upstream uses
  `playerProps.moveSpeed` (persistent) for every speed ramp and `runDistance`
  ONLY as the continuous-run odometer that `Player_PositionUpdate` zeroes
  outside Run states (it gates the `RUN_STUMBLE_TRIGGER_DIST` wall-smack
  checks). A merge fused both under `runDistance`, so walk/backward/sidestep
  speeds wrote a per-frame-zeroed field (walk-in-place at high FPS) and the
  wall-smack thresholds read the wrong quantity. Renamed 196 sites to match
  upstream 1:1. Found while bringing up `movement_original` (the original
  PSX lower-body machine, now the default — the legacy PC movement shim
  remains for the TPS debug camera).
  [`player_control.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/bodyprog/player_control.c) ·
  commits [`3cd9a5e8d`](https://github.com/SlickAmogus/silent-hill-decomp/commit/3cd9a5e8d),
  [`e6aa45e0d`](https://github.com/SlickAmogus/silent-hill-decomp/commit/e6aa45e0d)
- **Invisible school cat — duplicated chara anim array.** The Jun 2026 merge left
  the same PSX array under two names: fork `g_CharaTypeAnimInfo` (written by the
  loader) and upstream `g_CharaModelAnimsData` (read by `Chara_BonesInit`, never
  written past slot 0). The cat — the only cutscene character driven through
  `Chara_BonesInit` — got `activeAnmHdr=NULL`, the §1 `Anim_BoneInit` guard
  silently skipped bone init, and the model rendered as a degenerate point.
  Unified on the upstream name (single definition). Also fixed the
  `Anim_CharaTypeAnimInfoClear` PSX byte-count `bzero(…, 72)` that only cleared
  1.8 of 3 slots with 64-bit pointers (stale anim headers across map loads —
  prime suspect for the "warped gray polygon blob" in school hallways).
  [`fs_chara_anim.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/bodyprog/game_boot/fs_chara_anim.c) ·
  commit [`0d57ece35`](https://github.com/SlickAmogus/silent-hill-decomp/commit/0d57ece35)

> Already covered above and also cutscene-relevant: `CAT_ANIM_INFOS` zero-stub
> (§2) and the `Anim_BoneInit` / `playbackFunc` NULL guards (§1) — both of which
> turned out to be masking the duplicated-array bug above.

## Positional-SFX / world-object zero-stub batch (2026-07-06 audio audit)

Seven map-overlay symbols with real disc data were still `u8[256]={0}` exe
stubs (which shadow map-DLL data under `--export-all-symbols`). All are now
real extracted data (`extract_map_data.py` EXTRA_SYMBOLS + hand-appended to the
tracked `*_extracted_data.c`):

- `D_800ED938` (map2_s02) street SFX position — played from world origin
- `D_800D2530` (map3_s00) / `D_800D26F8` (map3_s06) door SFX positions
- `D_800D4CE4` (map3_s01) hospital generator hum position — origin was outside
  the Q12(16) falloff, so the generator loop was **silent**
- `D_800CB364` (map3_s04) stinger SFX position
- `D_800DAAD0` / `D_800DAAE4` (map5_s00) sewer pickup `s_Pose`s — pickup
  objects rendered at the world origin

Found by the new census tool `pc_port/tools/audit_map_sound_data.py` (scans all
maps for sound-adjacent `D_8*` symbols, classifies extracted / zero-stub /
missing, and diffs against the disc overlay bytes).

## HyperBlaster free-aim fix (2026-07-15, commit `7c939d88b`)

Adapted from SlickAmogus PR #36 (unmergeable — the fork branch diverged
~4700 commits from a far-back base; the three fixes were extracted and
re-applied against our current tree, keeping our newer wall-time refire FSM).
All three are real bugs in our tree, in the TPS/OTS free-aim gun path:

- **Invisible upper body while aiming**: `Pc_AimHoldKf` pinned the aim pose at
  the shared kf 591, but the HyperBlaster's WEP53 block only spans kf 568-579
  (`D_80028B94[132..149]`), so the torso posed from out-of-range keyframe data
  and collapsed. Now holds 574 (`player_control.c`).
- **Fire lockout / no full-auto**: the free-aim FSM treated it as a semi-auto
  magazine gun. Its clip count is 0 and never refills, so `fireEdge && ammo>0`
  locked firing out and the reload branch could latch. HyperBlaster is PSX
  full-auto with no ammo/reload — now fires while held once per recoil cycle
  (`isHyperBlaster ? (fireHeld && s_refireT <= 0) : (fireEdge && ammo > 0)`),
  reload skipped (`!isHyperBlaster`). Kept our wall-time `s_refireT`; did NOT
  regress to the PR's frame-based `s_refireCd`.
- **OOB anim-info copy on equip**: the equip path copies a fixed 20 entries but
  the HyperBlaster block is the last one and only 18 long, reading 2 past
  `D_80028B94`. Bounded behind `SH_PC_PORT` via a new `D_80028B94_COUNT`
  (`bodyprog_data_80028B94.c`).

## Controls / free-aim batch (2026-07-06, commit `003cd4cec` + PsyCross `90f0d9e`)

- **altcam_button_sprint** (new config, default 0): alt cameras walk by
  default, sprint only on the bound Run control (`player_control.c`, both the
  TPS/OTS/FPS path and 2D-control-under-alt-cam path). Launcher checkbox +
  sensitivity sliders second-column rework (`ControlsForm.cs`).
- **Double-fire / zoom-exit on trigger mash** (alt modes):
  `Pc_FreeAimGunUpperBody` refire cooldown wall-time 0.2s + 50ms release
  debounce (was 4 rendered frames); FSM gate + TPS zoom now also hold on the
  raw aim input so 1-frame `isAiming` blips can't drop to the PSX gun path
  (extra ungated shot) or pop the zoom.
- **Free-aim accuracy**: PSX random shot deviation capped for player shots in
  alt cameras (`bodyprog_combat_8008A058.c`; handgun/rifle ~exact, shotgun
  fixed 4° cone). Classic camera keeps the original accuracy model.
- **Firing-pose shadow glitch** (per-pixel shadows): PsyCross flashlight
  shadow FIFO entries value-validated like PGXP (`PsyX_GPU.cpp`) — CPU-built
  quads (muzzle flash) no longer inherit stale arm/gun view-space verts.
- **PGXP near-camera warp**: root-caused; fix = GL near-plane clipping
  (`PgxpNearClipEmit`), documented in `PGXP_Architecture.md` §9.

## BGM reverb + ADSR batch (2026-07-06, commit `4a37582ca` + PsyCross `12a5ab5`)

User side-by-side vs PSX: layer fade-ins/outs abrupt, echo/reverb character
missing; `adsr 1` confirmed improving fade-ins. Root causes + fixes:

- **`SpuSetReverbModeParam` was unimplemented** — but SH1 drives reverb
  through it constantly: per-track wet depth on every BGM bank load
  (`g_Sd_ReverbDepths[35]`, sd_call.c VabLoad), and a per-tick 0→target depth
  RAMP on sequence (re)start (`replay_reverb_set`, the "fades in with an
  echo"). Depth now maps to the OpenAL aux-slot gain (live). `revscale`
  console + `reverb_scale` config calibrate the mapping (default 2.0).
- **ADSR envelopes default ON** (`adsr` config key, default 1; console
  toggle). The deadlock + key-status hangs that justified default-off are
  fixed; envelopes are required for the music's instrument fades.

## FPS-camera polish + FIX_ANG framing rework (2026-07-06, commits `696fdde61`, `1cb6b4bdb` + PsyCross `8c34600`)

- **Melee-swing mesh flash in first person** (`02280a0cc`; hide attempt
  `696fdde61` REVERTED `79b685a51`): swings lean Harry's head (and the
  eye riding it) through his own torso/shoulder/arm models. Final fix is
  the arm-clearance camera dolly in `Pc_TpsCamera_Apply`: while a melee
  `weaponAttack` is active, the eye dollies back along the view axis by
  how much the nearest forearm/hand bone crowds it (proximity-driven, so
  it self-times per weapon anim; wall-clamped by ray trace). The interim
  torso/shoulder hide was reverted once the dolly landed — the body
  stays visible through swings; only the head-only FPS hide remains.
- **Faded letterbox band at top of screen** (`1cb6b4bdb` + PsyCross
  `8c34600`): the FIX_ANG `g_PsxWorldVShift` framing band-aid (843b3a58c)
  shifted the ortho window up, revealing rows above the 224-line frame
  that screen-space overlay prims never cover — a faded band toggling
  with FIX_ANG camera zones, in every camera style (an interim alt-cam
  gate `a42e02d0a` only masked it for FPS/TPS/OTS). Rework: MainLoop
  (game_main.c) applies the shift at the GTE projection center
  (`SetGeomOffset(0, vshift)`) during classic-camera gameplay only —
  same visible world window, full overlay coverage, no band. Asserted
  every frame in every state so item screens/menus keep the clean (0,0)
  baseline. `vshift` console command still live-tunes the amount.
- **Eclipse-door key-insert grey flash** (`e05613a3f`):
  `Screen_BackgroundImgTransition` (the ~1s dissolve between DOOR_*.TIM
  lock states) was the only fullscreen-2D-background draw path not
  pinging `g_Pc2dBackgroundActive`; mid-fade the 300ms `bg2dHeld` hold
  expired → fog-grey clear (dithered = black specks) leaked through the
  semi-trans fade quads and Hor+ re-engaged. Rule: EVERY fullscreen 2D
  background draw path must ping the counter.

## Interior flat-texture regression (2026-07-06, revert `cec9ef496`)

- **"Texture corruption" in interiors (school most often)** — most wall/
  door materials rendering FLAT with a few textured survivors — was the
  Jul 1 `Lm_UntextureVramCollisions` flattener (`0fa6cbb10`) over-firing:
  it untextured BOTH materials of any keep-set pair sharing a CLUT
  column, every frame (the school's SC2FT1 legitimately sits at clut
  column (0,0)). It had been shipped for the map3_s02 Alessa rainbow,
  later proven to be a cutscene P_TAG overlay bug (fix still pending).
  Reverted; the Jun 29 stolen-page untexture (`g_PcInteriorMatSync`,
  the real rainbow guard) is kept. Reported MSAA-8x correlation was a
  red herring.

## Alt-camera melee parity (2026-07-06, commit `86578da27`)

- **Multi-tap combos impossible in TPS/OTS/FPS**: the combo click queue
  counted only pad action-mask edges; alt cameras read fire via SDL
  (mouse), so it stayed empty. `g_PcAltFireHeld` publish feeds it now.
- **Every click dispatched the wide swipe** (combo window only opens
  from tap-class swings): the shim now mirrors the PSX shift-register
  semantics on a wall clock — hold >= 130ms = swipe (`IsAttacking`),
  completed shorter click = tap pulse (`IsShooting`, dispatch on
  release, like classic).
- **Katana lunge missing in alt cameras**: the aim shim forced
  `lowerBodyState = Aim` every frame, stomping the swing's `Attack`
  lower-body state that carries the attack root motion; no longer
  overwritten mid-swing (also stops movement fighting active swings).

## Custom textures: hi-res PNG/TIM overrides render for real (2026-07-08)

- The hi-res override registry (`hires_override.c`) existed but nothing
  consulted it at draw time, so registered overrides never rendered.
  PsyCross (`7ffe8b9`, port of PsyCross PR #5) now routes every textured
  prim through `HiresOverride_LookupByTpageClut` and binds the override
  via the existing `TF_32_BIT_RGBA` path, with a `u_texOffset` uniform so
  surfaces wider than one tpage sample the right region per prim.
- PNG input: `HiresOverride_RegisterFromTim` sniffs the PNG magic and
  decodes via vendored `stb_image.h` (v2.30, byte-identical to upstream,
  PNG-only/memory-only). PNG carries true 8-bit alpha: 0 = hole (shader
  discards < 0.5), ~128 = STP-style blend on semi-trans prims, 255 =
  opaque. TIMs keep 1-bit colour-0 transparency.
- Discovery (`fsqueue_3.c`): with `allow_loose_files = 1`, a loose
  `gamedata/load/<FOLDER>/<NAME>.png` (e.g. `1ST/KONAMI2.TIM.png`)
  always registers as an override (never a byte-replace) regardless of
  size; the disc file still loads so the engine picks the native VRAM
  rect, then `PostLoadTim` registers the PNG against it. Any resolution
  works — native UVs map 0..1 over the original, so uniform upscales
  (2x/4x/8x) just work.
- Salvaged by hand from unmergeable PR #38 (its branch diffs the whole
  tree as added; merging would clobber current work — reference only).
  v1 limitation: tpage/clut-keyed overrides fit single-tpage assets
  (items, HUD, sprites, character textures); packed world/interior
  atlases need the per-material residency rework.
- Loose/hires diagnostics moved from stderr to SH_DBG so they land in
  SilentHill.log. Byte-identical with `allow_loose_files = 0` (weak
  stub, zero registrations, override state stays 0).

## VRAM residency: expanded chunk-texture pool (2026-07-08)

- The 10-slot VRAM chunk-texture pool is the root of the recurring interior
  flat/rainbow class and the open exterior APU cross-area rainbow. With
  `resident_textures = 1` (default) the pool grows by 128 full + 48 half
  VIRTUAL slots (`terrain.h PC_TEXPOOL_*`): each is backed by a persistent
  per-slot GL texture instead of a VRAM page, keyed by a synthetic prim CLUT
  with bit 15 set (encoding in `hires_override.h`), delivered per prim
  through the Phase-0 override path. `PostLoadTim` skips the VRAM upload for
  virtual slots and decodes the TIM (or a loose PNG/TIM replacement — custom
  WORLD textures now work) into the slot texture.
- Interiors texture EVERY loaded chunk (whole map resident); the PC
  keep-4-nearest + page-steal loop and both `g_PcInteriorMatSync` shims are
  bypassed (kept only for `resident_textures = 0`). Exteriors keep the
  vanilla distance loop; the expanded capacity alone removes slot collisions.
- `SPUM602F` (map4_s03 Twinfeeler) is pinned to physical slots: map code
  StoreImages its CLUT row and derives palette-animation rows from its
  imageDesc, which requires a real VRAM CLUT (`Pc_MaterialNeedsVramSlot`).
- Missing-TIM materials swap back to a physical slot so the PSX degraded
  look (stale page) is preserved; `[POOLTEX]` log lines cover registration,
  loose replacement, and pool-exhaustion sizing.
- PsyCross 2nd commit: the FT4 clutY>511 garbage-prim guard exempts prims
  the override table claims (bit-15 keys); lookup fast path requires the low
  6 clut bits clear so garbage cluts still reject 63/64.
- Full design + survey findings: docs/Texture_Residency_And_Custom_Textures_Task.md §10.

## DuckStation texture-pack support (2026-07-08)

- Drop existing DuckStation texture packs into `gamedata/texturemods/` —
  loose folders (e.g. a `replacements/` dir) or whole `.zip` archives — and
  with `texture_packs = 1` (default) they apply automatically. Matching is by
  content hash exactly like DuckStation: `texupload-P4-<srcHash>-<palHash>-
  <WxH>-<ox>-<oy>-<wxh>-P<min>-<max>.png` where srcHash = XXH3-64 of the VRAM
  upload payload (our TIM pixel block) and palHash = XXH3-64 of the CLUT
  (including DuckStation's partial-range quirk: the FIRST max-min+1 entries).
  P4/P8/STP4/STP8/C16/STC16 names supported.
- At `PostLoadTim`, every TIM upload is hashed; matching sub-rect PNGs are
  composited over a nearest-upscaled base at the pack's scale (mirroring
  DuckStation's compositor), then registered as a per-slot GL texture
  (virtual pool slots = world/interior textures) or a rect-keyed override
  (VRAM TIMs: items, HUD, charas, 2D backgrounds). Loose `gamedata/load/`
  replacements keep working unchanged and take priority over packs.
- `pc_port/src/tex_pack.c`; vendored `xxhash.h` (v0.8.3, BSD) +
  `miniz` (3.0.2, MIT) for hashing and zip reading. `[TEXPACK]` log lines
  cover indexing and per-upload composition. Validated against a real 12k-
  file pack (index parity with ground truth; end-to-end compose test with
  loose + zip + partial-palette entries).
- Loose folders and `.zip` archives are both read IN PLACE (miniz) — nothing is
  extracted to disk by the GAME. `.rar` has no in-place path (solid/RAR5 make
  random access impractical) so the GAME never touches rar; instead the launcher's
  Mod Manager extracts a `.rar` to `<rar>.extracted/`, which the game then reads as
  an ordinary loose folder (see below). The standard DuckStation layout —
  `<GAMEID>/replacements/...` with a `config.yaml` — works as-is at any nesting
  depth (scan is recursive; names matched by basename). A `texturemods` entry
  (folder or `.zip`) renamed `<name>.disabled` is skipped (`Name_IsDisabled`);
  this is how the Mod Manager toggles a pack off. `texpage-*` entries
  (page-mode dumps) are unsupported and counted in the log.

## Texture dumper (2026-07-28)

- `dump_textures = 1` writes every decoded texture upload to `gamedata/dump/`
  as a PNG named the way the pack loader matches it, so the dump folder IS a
  texture pack: copy it into `gamedata/texturemods/<pack>/`, repaint the files,
  done. Default 0; off it hashes/allocates/writes nothing.
- Why it exists: a SH1 TIM is ONE 4-bit index sheet plus MANY CLUT rows, and a
  model draws different regions through different palettes at once, so a raw
  per-palette export is the whole sheet tinted one way and none of them looks
  right. `clut_tool.py` / `ClutComposer.cs` recover the texel→palette map by
  parsing the `.ILM`, which only exists for CHARACTERS. The dumper takes
  DuckStation's approach instead — write what the engine actually uploads — so
  BG/world textures are covered too.
- **Per-object sub-rects (2026-07-28, revised).** An entry is no longer the whole
  sheet: like DuckStation, it is the piece of the sheet ONE OBJECT draws, through
  the palette row that object selects. The piece is the UV bounding box of one
  model over the sheet, and it comes out of the model file — the same
  `s_Primitive` UV/CLUT words `pc_port/tools/clut_tool.py` parses offline.
- Two hooks:
  - `TexPack_DumpScanLm(raw)` from `LmHeader_FixOffsets_PC`
    (`pc_port/src/lm_reformat.c`), BEFORE the header is reformatted, so the
    material/primitive CLUT words are still the values the file baked
    (`Material_FsImageApply` rewrites them against VRAM immediately after).
    Walks the raw PSX-layout bytes, so .ILM, .PLM and an .IPD's embedded LM are
    all the same code, and registers one box per (model, material, CLUT row).
    Verified over the disc corpus: material `field_14`/`field_16` are 0 in every
    file, i.e. baked UVs are sheet-relative — no page-offset correction needed.
  - `TexPack_DumpUpload(name, …)` from `Fs_QueuePostLoadTim`
    (`src/main/fsqueue_3.c`), still above the virtual-pool / VRAM-resident split
    — the single point every texture class passes through with the exact
    `(pixels, palette, bpp)` triple the pack matcher hashes. The TIM's file name
    is what ties it to the boxes: the material name IS the TIM stem.
- Entry name is unchanged except the sub-rect fields now carry the box:
  `texupload-P4-<srcHash>-<palHash>-<w16>x<h>-<ox>-<oy>-<w>x<h>-P0-15.png`
  (`P8` → `P0-255`, 16bpp → `texupload-C16-…`). WxH is in VRAM 16-bit WORDS,
  the sub-rect in native texels. The crops blit back over the native base layer
  at their offsets, which is how `TexPack_Compose` already works.
- Load order: an LM is what queues its own materials' TIM reads, so geometry
  normally registers FIRST. A sheet whose model turns up later (shared BG sheets
  are referenced by up to 118 chunks) is served from a held copy of the upload
  (64 MB LRU, dev-mode only) and gets its crops the moment that chunk loads. A
  sheet still unclaimed 96 uploads later is written whole instead — never wrong,
  just less concise.
- Rows no primitive names statically are dumped through the UNION of the rows
  that do — a material's CLUT row is bumped at runtime for palette/lighting
  variants, which moves the same geometry onto another row. So row coverage is
  exactly what it was before the change; only the extent shrank.
- Whole-sheet entries remain for sheets NO model references (fonts, HUD, 2D
  screens) — for those the whole upload *is* the object. They are written once
  the load batch that could have claimed them is 96 uploads past, or at exit
  (`TexPack_DumpFlush`, also on `atexit`).
- Regions per (sheet, row) are capped at 16 (compose stops collecting at 64
  matches/upload); over the corpus a row averages 1.1 objects (BG) / 2.1
  (characters), and the cap merges the two boxes whose union wastes least rather
  than dropping one.
- Dedupe: a sorted set of emitted names plus an on-disk existence check, so a
  file is written once per install, never per re-upload. Two objects with the
  same box, or two CLUT rows holding the same palette, collapse to one file.
- Skipped by design: 24bpp uploads and CLUT-less 4/8bpp uploads (e.g.
  `FONT8NOC.TIM`). Neither can be expressed in the pack name grammar, and the
  matcher can never replace them either, so a dump would be dead weight.
  Palette rows past 16 are skipped, same cap as the pack compose loops.
- Verified offline against the real `tex_pack.c` (harness feeds real disc models
  and TIMs to the two entry points, then indexes the result with the real
  `Scan_Once`/`ParseName`): whole disc, 996 TIMs + 577 models → 10097 entries
  (9345 object crops + 752 whole sheets); **10097/10097 names parsed (100%)** and
  **6737/6738 rows composed back BYTE-IDENTICAL** to an independently written
  native decode (the 1 miss is the CLUT-less TIM above). Crops carry 119.4 Mpx
  where whole-cover entries would carry 609.0 Mpx — 19.6%.
  - Feeding each model one step BEHIND its upload (the held path): 10121 files /
    9369 crops / 19.6%, same 100% parse and same 6737/6738 byte-identical.
  - Degenerate order, every upload before any model: falls back to 6308 whole
    sheets — still 100% parse and 6737/6738 byte-identical.
  - Model scanning costs 8-9 ms for all 577 models. With `dump_textures=0` both
    entry points measure 0.0 ms and write nothing.
  - Cross-checked against `clut_tool.py`: the C walker's boxes for `HERO.ILM`
    are exactly the ones the Python model derives, rect for rect.
- Consequence for BC7: a whole-cover `.dds` takes `TexPack_Compose`'s compressed
  fast path, but a sub-rect `.dds` cannot (the compositor blits 32-bit pixels),
  so converting a crop-shaped dump costs a CPU BC7 decode per entry.

## Texture-pack load order (2026-07-11)

- `gamedata/texturemods/loadorder.txt` (one top-level pack folder per line,
  HIGHEST priority first) ranks packs when two replace the SAME sub-rect of the
  same source texture. `tex_pack.c` reads it in `Scan_Once`, tags each
  `PackEntry` with a `priority` (from the file) + insertion `seq`, and the
  compositor's `qsort` is now a TOTAL order `(srcHash, priority, seq)` so the
  higher-priority pack blits LAST (wins). Absent file = every priority 0 =
  deterministic insertion order (was qsort-by-srcHash alone = undefined order
  for same-hash entries). No change when `texture_packs=0` (zero entries).
- Written by the launcher's Mod Manager (see below); different sub-rects still
  composite/augment as before — order only decides same-sub-rect conflicts.

## Bullet-decal depth split (2026-07-11)

- `pc_port/src/pc_decals.c`: decals were painting over objects standing in FRONT
  of the wall they sit on. Root: the world is painter's OT-ordered by default
  (GL depth test only under `pgxpZBuffer`), and decals are added to the world OT
  AFTER the walls, so a decal needs a NEARER bucket just to be visible over its
  host — but the single `DECAL_SZ_BIAS=96` pulled the bucket 3 buckets nearer,
  over-drawing any foreground geometry within ~3 buckets. Fix: split the one
  bias into two — `DECAL_SZ_BIAS=96` stays for the per-vertex GL depth (the
  pgxpZBuffer path still needs a full 64-SZ quantum + margin vs the wall's
  quantized depth), and `DECAL_BUCKET_BIAS=32` is the painter's OT bucket bias =
  exactly ONE bucket nearer (minimum to draw over the coplanar wall). Lever if
  decals ever vanish on steeply-angled walls: raise `DECAL_BUCKET_BIAS` toward
  48-64 (visibility-vs-overdraw tradeoff; 32 is the aggressive end).

## Launcher Mod Manager (2026-07-11)

- Replaces the launcher's Level dropdown with a `manager.png` button (Form1)
  that opens `ModManagerForm`. Two homes (both additive — nothing touches the
  disc image): TEXTURE mods in `gamedata/texturemods/`, managed IN PLACE; LOAD /
  FMV mods in a self-owned `mods/` library, deployed on Apply.
- **Texture mods** = a loose folder, a `.zip`, or a `.rar` in
  `gamedata/texturemods/`. Folder/`.zip` are read in place by the game; the
  checkbox enable/disable renames the entry to/from a trailing `.disabled`. A
  `.rar` is extracted by the launcher (see rar note) to `<rar>.extracted/`, which
  the game reads as a loose folder; enable/disable renames THAT folder
  (`<rar>.extracted` ↔ `.disabled`, kept so re-enable needn't re-extract), the rar
  file itself stays put (the game ignores `.rar`). Delete removes the mod (rar +
  its extracted folder), prompted. Active packs' names are written to
  `gamedata/texturemods/loadorder.txt`, highest first (a rar contributes
  `<rar>.extracted`).
- **RAR extraction** (`RarExtractor.cs`): the vendored unrar built as an x64
  `UnRAR.dll` is EMBEDDED in the launcher exe (`<EmbeddedResource>`), materialized
  to `%TEMP%\SilentHillPC_Launcher\UnRAR.dll` and `LoadLibrary`'d on first use — so
  extraction never depends on the DLL sitting next to wherever the launcher runs
  (the failure mode of the earlier "ship it beside the exe" attempt: `IsAvailable`
  returned false → nothing extracted). The launcher is forced 64-bit
  (`<Prefer32Bit>false`) so the x64 DLL loads. P/Invoke of the standard UnRAR C
  API; struct layouts mirror unrar's `dll.hpp` (`#pragma pack(1)`); validated
  end-to-end (open + extract a real nested `.rar`). Rars auto-extract on manager
  open / drop under the progress dialog (`ModManager.Prepare` /
  `PendingRars`).
- **Load / FMV mods** live in `mods/`: load-folder mods (a `load/` or
  disc-structured tree) → merged into `gamedata/load/` (forces
  `allow_loose_files=1`); FMV mods (`.avi`) → flattened into `gamedata/FMV/`.
  Deployed via a `mods/deployed.txt` manifest (deploy = copy, undeploy = delete
  tracked files); higher priority copied last so it overwrites. A `.zip` dropped
  as a load/FMV mod auto-extracts into the library once.
- Drag-and-drop onto the window auto-detects type and routes it (texture →
  `texturemods/`, load/FMV → `mods/`). Right-click a mod for a friendly display
  name + notes (stored in `mods/modmanager.json`, DataContract JSON; the folder /
  `.zip` name stays the identity). The Form1 button swaps to `manager_clicked.png`
  while pressed. Logic in `ModManager.cs`, UI in `ModManagerForm.cs`,
  progress in `ProgressDialog.cs`.

## PAL (SLES-01514) fonts + languages + FMV batch (2026-07-08, commits `52582ca4b`..`f97055547`)

- **Region-aware FONT16** (`f5dff3a48`): PAL's 21x6 glyph grid lives at VRAM
  (768,128) tpage 12, CLUT (816,255) — all values from the decrypted EUR
  BODYPROG (`pc_port/tools/decrypt_eur_overlay.py`, `33b74e812`; the disc
  "scrambling" is the game's own `Fs_DecryptOverlay` LCG). New
  `pc_port/src/font_region.c` layout table + `Font_MapChar()` accent scheme
  (retail-exact: Latin-1 at byte-0x8B, combining marks 114/119 for
  uppercase); text_draw.c's three draw sites compute UV/tpage/clut from
  `g_FontLayout` — USA output bit-identical (originals under `#else`).
  Co-tenant patches: BG_ETC reslice (IMAGE_ETC desc u=32,v=64 on EUR),
  FLAME → tpage 13 (832,0), particle dust/ember UV remap + rain v-clamp.
  FONT16 requeued at `GameFs_TitleGfxLoad` + per map load (Konami logo/
  BG_ETC stomp the font home during boot; retail SLES reloads it too).
- **Languages de/fr/es/it** (`f97055547`): config `language` redirects the 45
  VIN map overlays (name char 6 digit → VIN2-5) and 15 TIPS_E TIMs (letter
  char 5 → G/R/S/T) inside `Fs_InitFileTableForRegion`;
  `pc_port/src/lang_text.c` parses `ITEM_<lang>.BIN` (195 pairs, base
  0x800C8B68) through the s_ItemName/s_ItemDesc chokepoints and extracts +
  translates each map's message table from the loaded overlay (PAL `{X}`
  dialect → US `~X`, EUR link base 0x800CB370, universal index-3 join + 7
  probe-verified page-split joins). Also fixes a live PAL bug: the map-anim
  header patch in player_control.c now rebases EUR-linked overlay pointers
  (-0x1DF8).
- **FMV on PAL** (`270574235`): fmv_player opens the resolved disc
  (`PcPort_GetGameDiscPath`) and takes base sectors from the region-remapped
  `g_FileTable` (new `PcPort_FileTableStartSector`); all 30 movies verified
  byte-identical across discs (+0x1E88 sector shift only).
- **Launcher** (`52582ca4b`): any `gamedata/*.bin`, ISO-serial region probe
  (`DiscProbe.cs`), disc label in UI, Language dropdown; game-side
  `g_PcConfig.language`. Full status/reference:
  `pc_port/docs/PAL_Language_Support_Task.md`.

## Ambient SFX batch — severed-alias class + silent positions (2026-07-09)

23-map multi-agent audit of missing rain/water ambience (spec + full findings:
`pc_port/docs/Ambient_SFX_Task.md`). Discovered a NEW bug class beyond
zero-stubs: **severed PSX aliases** — two symbol names that shared one address
on PSX (sym tables show them 2–4 bytes apart, one inside the other's extent)
became separate PC objects, so writes through one name never reach reads
through the other. Invisible to zero-stub audits because both halves hold
"real" data.

- **Rain sound restored** (`bbcae1642`): `sharedData_800DD58C_0_s00` IS
  `g_ParticlesAddedCount[1]` on PSX; split on PC, so the rain-particle count
  never reached `Particle_SoundUpdate`'s ramp and `SD_Call(Sfx_Unk1360)` was
  unreachable — rain visuals, no rain sound, on map0_s00 / map1_s02 (otherworld
  school courtyard) / map1_s03 (roof) / map4_s02. Fixed with an SH_PC_PORT
  macro alias in `particle.c`. Same commit: map5_s03 defined
  `g_ParticlesAddedCount` as a scalar while `particle.c` zeroes `[1]` — real
  4-byte OOB write, now `s32[2]`.
- **map6_s04/s05 water fade** (`eb9d3f484`): `sharedData_800EB74A_6_s04` is
  byte `[2]` of the `sharedData_800EB748_6_s04` limits table on PSX; split on
  PC, so the fountain-room water layer played at constant full volume (frozen
  0x80 cap). SH_PC_PORT write-through into the table.
- **Courtyard ghost rooms / interior exact-cell draw** (`3b96cdc03`,
  2026-07-10): retail interior maps are isolated room islands (one per 40u
  cell, 16-28u dead gaps, zero cross-cell geometry; verified against US-disc
  IPDs) and retail drew ONLY the player's exact cell. The PC ±2/±1 interior
  window + 4-nearest pcInDrawSet drew neighbor islands floating over open
  areas (otherworld courtyard SUFFFE showed SU00FE/SUFFFF mesh-ceiling
  corridors). Interiors now draw exact-cell; removes pcInDrawSet and
  MapRegistry_IsExactCellArena. Widescreen needs no window: everything
  visible from a room lives in that room's chunk.
- **whole_map_exteriors draw path** (`286157766`, 2026-07-10): the flag only
  textured chunks; the visible square was set by per-poly far culls
  (min(fog.farDistance, ~61u OT cap)), baked subcell PVS (viewer ≤ ±3.2
  cells), and s16 view-Z wrap at 128u — fogstr never affected geometry. In
  whole-map mode the caps lift, wrapped depths bucket into the last OT slot,
  and previously fog-bounded OT inserts gain SH_CLAMP_OT_DEPTH.
- **Rain-path div-by-zero crash** (`800ac4ab1`, follow-up 2026-07-09): the
  restored rain path crashed 0xC0000094 one frame after `Sd_PlaySfx(1360)` —
  `Sfx_WithFalloffAndPitchPlay`'s `AttenuationCalc` divides by `falloff`, and
  the rain call in `particle.c` is the game's only `falloff=Q12(0.0f)` site.
  MIPS div-by-zero doesn't trap (quotient -1, which retail relied on); the
  guard returns -1 so rain volume matches PSX. Confirmed via objdump:
  exe+0xECA5A = the `idivl` in `AttenuationCalc`. Lesson: newly-reachable
  retail code was never covered by the Jun-11 div-zero sweep — re-audit
  divisions whenever a dead path is resurrected.
- **Elevator chime + pickup poses** (`b2e4adecf`): hospital elevator arrival
  ding (`sharedData_800CB094_3_s01`) silent on map3_s01/s03/s04/s05 — the exe
  stub hardcodes map7_s01's position ~170+ units away (attenuates to exactly
  0); per-overlay VECTOR3s appended. Plus map3_s01 elevator-stop clunk pos,
  map1_s03 valve-grip SVECTOR, and six map5_s00 sewer pickup poses (items at
  world origin).
- **Negative results that matter**: map5_s00 (main sewers) water data verified
  byte-exact — remaining silence reports need runtime `[SH_BGM]` probes, not
  data. map2_s01 is the CHURCH, not "Old SH sewers" (spec corrected); its
  pre-Dahlia silence is authentic. Shared Sd_* chain verified healthy
  end-to-end. Still open: map3_s05 vine-fire loop constant volume
  (`D_800DD190` = `sharedData_800D8568_1_s05+0x10`, another severed alias).

## NTSC-J (SLPM-86192 Rev 1/2) support (2026-07-10, commits `4af7db1c7` + `1ac340612`)

Full reference: `pc_port/docs/NTSC_J_Support.md`.

- **Region plumbing** (`4af7db1c7`): `Region_JPN` + SLPM/SLPS/SIPS serial
  probe (with first-print exe-t_size guard); `s_FileTable_JAP` from the
  in-tree JAP1 table — verified index-aligned with USA (2074 entries), so it
  memcpys straight in; JAP XA bases (US+5); audio sector remap generalized to
  every non-USA region; JAP overlay pointer rebase (link base `0x800CBBD0`,
  disc-verified); per-region disc buckets with auto priority USA>PAL>NTSC-J;
  `region = jap`; launcher NTSC-J playable (2026.7.10.1). TIM sweep ground
  truth: 996/996 shapes identical to US, only TIPS_E*/MEMO_INR differ in
  content (Japanese images, same names — zero code).
- **Japanese story text** (`1ac340612`): JP overlays carry SJIS messages with
  the SAME `~` code grammar as US → verbatim extraction at the JAP base;
  explicit US→JAP index tables for the 14 maps the US localization
  split/added lines in (`lang_jpn_msgmap.inc`, DP skeleton alignment);
  embedded public-domain Shinonome JIS X 0208 font replaces the BIOS kanji
  ROM (`pc_kanji.c`, `kanji_font.inc`, `tools/make_kanji_font.py`); on-demand
  12×16 4bpp atlas cells in the framebuffer margin strips (rows 16..31 /
  480..495 — retail JP's own band); SJIS branches in both string drawers +
  width calc. Latent EUR fix: `MSG_COUNT_MAX` 96→176 (MAP7_S02 has 159
  messages; the replaced pointer array under-covered 3 maps on PAL).
- **NTSC-J title + school Mumblers + 2D right-edge line** (`38bc60ea8`,
  `9c90980d3`): JP title = load TIM/TITLE.TIM at the GameFs_TitleGfx
  chokepoint (retail JP title code is instruction-identical to US — verified
  by decrypted-overlay structural diff); JP school maps (map1_s00..s03 +
  map6_s04) swap Grey Child->Mumbler models per map load (disc-verified
  spawn-group bytes); Screen_BackgroundImgDraw tiles clamped to the image's
  content extent — the PC-only -1 shift (f6354a417) dragged one foreign VRAM
  texel into the last visible column = full-height tinted line on 2D screens
  in all regions. Also flashlight mode labels shortened + page-2 value column
  204 so they fit the 320px clip.
- **PAL black item previews FIXED** (`534b12d6b`): the PAL discs' item packs
  (IT_00x/UNQxx TMDs) are NOT byte-identical to US — every textured prim's
  clut word is baked for the EUR retail palette homes (896..944, 480..495;
  retail moved them because PAL's 256-line framebuffers cover the US homes).
  Our port uploaded item palettes at the US homes, so PAL prims sampled empty
  palettes → black previews (center "discolored" via the carousel dim). Fix:
  Font_ApplyRegionPatches retargets the five item texture descs to the EUR
  homes (byte-verified against the decrypted EUR BODYPROG desc cluster). All
  item diagnostic probes removed. Lesson recorded: re-verify inherited
  "byte-identical" claims by hash — a wrong one steered this hunt for days.

## Mouse cursor — puzzles + clickable main menu (2026-07-13, commit `153fc7fc8`)

QoL feature, not a fix (config `mouse_cursor`, default on; inert while the
TPS/OTS/FPS camera captures the pointer). Game-code touch points:

- **`Gfx_CursorDraw`** (`bodyprog_800881B8.c`): SH_PC_PORT hook — the shared
  chokepoint every free-cursor puzzle (piano, plate, door panels, map pan)
  draws through. Hands `pc_mouse_cursor.c` the puzzle's current cursor position
  (framebuffer centre-origin px) and left/right click into enter/cancel on
  `g_Controller0`, so all puzzles gain mouse control with no per-puzzle code.
  Cursor control is an **absolute servo** (commit `18a49bc42`): the injector
  deflects the left stick proportional to the error between the puzzle's own
  cursor and the framebuffer point the mouse is over. The earlier delta-velocity
  injection lost range to the stick's ±127 clamp and the height scaling — at
  sensitivity 1 a full mouse sweep only covered ~62% of the puzzle's cursor
  range (e.g. piano X∈[-89,85]/Y∈[-71,84]px, moved by `leftX*16384/75`), so the
  cursor stopped ~2/3 down. The servo is closed-loop on the puzzle's own cursor,
  so it converges on exactly where the mouse points regardless of framebuffer
  height, stick scale, or the clamp; it engages on a mouse move and releases on
  arrival so an idle mouse leaves a real pad's stick alone.
- **`GameState_MainMenu_Update`** (`events/title.c`): hover selects / click
  confirms on the entry list and difficulty rows (hit-tests the authored row
  bands: y=184+i*20 and y=204+i*20); draws the pointer after the text. The
  pointer sprite is the game's own 32x32 arrow from BG_ETC.TIM (UV(0,64),
  CLUT (192,0), tpage 12) — resident at the menu in every region/boot path.
- **`MainLoop`** (`sys/game_main.c`): per-frame `Pc_MouseCursor_FrameUpdate`
  after the controller is built, before the state update reads it.

Window→viewport mapping comes from PsyCross `PsyX_MapWindowToViewport`
(submodule commit `63334e0`).

Extended to every menu screen in `87fdb4fad` (options main/extra/PC pages,
brightness, controller config, load/save incl. the Yes/No prompt): each
screen hit-tests its own authored row layout and injects the controller
bits its stock input code reads, so step/clamp/SFX/state logic is
untouched. Two invariants to preserve: (1) the controller-config Actions
pane must NEVER receive injected button bits — `ConfigUpdate` binds any
clicked button to the hovered action; (2) hover-select snaps
(`Prev = Selected`, no highlight-timer reset) so the click that follows
isn't swallowed by the `LINE_CURSOR_TIMER_MAX` input gate.

## Global chara/asset pool — spawn any monster in any map (2026-07-13, commits `b34fd5702`, `512762429`, `bfc5ad965`, review fixes `c0fd6785c`)

Config `global_chara_pool` (default 1); design doc `docs/Global_Chara_Pool.md`.
Vanilla loads only ~3 monster types per map (charaGroupIds); the pool keeps
EVERY chara's ILM/ANM resident PC-side (malloc'd, loaded once via the vanilla
FS queue at map-load case 6), gives each a dedicated anim slot (4+charaId in
the PC-grown `g_CharaModelAnimsData`) through the explicit-buffer
`Fs_CharaAnimDataAlloc` path map7_s03's boss rush already used, and routes
textures to persistent virtual GL slots 256+charaId (synthetic clutY>=512
desc; no VRAM bytes; CLUT rows >=16 spill to slot+64k for the 48-row PRS TIM).
`chara_global.dll` — a pseudo-map compiling every portable monster's AI with
no map define — backfills NULL `charaUpdateFuncs` slots per map load.
Native maps always win every registry (NULL/stale-only refresh), so native
gameplay is untouched; `global_chara_pool=0` is byte-identical to before.

- **Files**: `pc_port/src/pc_chara_pool.c(+h)`, `src/maps/chara_global/*`,
  hooks in `game_load.c` case 6 + `map_registry.c` + `main_pc.c`,
  hires_override slot-space growth (256→512, chara range persists reset).
- **Guards landed with it**: `Chara_SpawnFlagsSet/PositionSet` skip rows >1
  (pool idxs + latent vanilla slot-3 OOB into cameraPaths); console spawns
  are flagged (`g_PcNpcDebugSpawned`) so killing one no longer corrupts the
  savegame's native spawn-row alive-bits (`Savegame_EnemyStateUpdate` guard).
- **Excluded from AI backfill (map-bound)**: Twinfeeler, Incubus, Unknown23,
  LockerDeadBody, Chicken (no AI on disc), cutscene actors — they spawn as
  posed statues, tagged `[no-ai]`/`[pool]` in `SPAWN LIST`.
- **Known limitation**: foreign monsters play wrong/silent SFX (per-map
  ambient VAB program collisions); fix = extra PC VAB slot, deferred.

## Fan-translation support — disc-authoritative text on modified discs (2026-07-13, commits `db055e972` + `eaf93dce7`)

Fan patches (probe-verified against the Spanish fandub, USA + PAL variants)
edit the retail discs **in place** — ISO layout unchanged, file-table sectors
stay valid. The XA voice dub (~49k sectors in HILL.), repainted FONT16 accent
glyphs, TIMs (TIPS, map screens, memcard prompts, UFO) and VABs all stream
from the disc already; **text was the only thing the port compiles in**.

- **Disc selection**: config `disc_image` (exact filename in `gamedata/`)
  beats the region auto-pick — how a `-patched.bin` gets chosen next to the
  vanilla image. Launcher Disc dropdown writes it; `DiscProbe` flags
  `[modified]` when BODYPROG's first sector hash differs from retail (the
  overlay is LCG-XOR'd, so any in-place edit scrambles that sector).
- **Disc-authoritative USA text** (`lang_text.c`): on USA discs,
  `FanTextInit` reads + `Fs_DecryptOverlay`s BODYPROG off the disc and
  adopts its kerning table (0x80025D6C; the fandub repaints the glyphs for
  bytes `; < = > W X ' -` into full-width `á é í ó ú ñ ¿ ¡`) and its `INVENTORY_ITEM_NAMES` /
  `g_ItemDescriptions` pointer arrays (0x800ADB60/0x800ADE6C) when they
  differ from the compiled originals; `Pc_LangPatchMapMessages` grew a USA
  branch (link base 0x800C9578, identity indices, verbatim copies — fan text
  is already US markup dialect, pure ASCII). A **matching decompile means
  compiled text == vanilla disc text**, so vanilla discs compare equal and
  are a guaranteed no-op; per-map self-detection, no config gate. After the
  adversarial review, the USA branch reads the overlay itself off the disc
  image (`ReadDiscFile`) instead of consuming `g_OvlDynamic` — no
  `Fs_QueueWaitForEmpty` on USA (vanilla load timing untouched, no
  stale-buffer risk from the queue-drain timeout valve), NUL-validated
  string walk, malloc checks + pool caps, and the modified-vs-vanilla
  compare bounded to the 15 shared `map_msg_common.h` entries every
  compiled table starts with (the compiled arrays carry no count, so
  deeper indexing could run past a shorter table).
- **Menus**: the port renders menus from compiled strings a disc patch can't
  reach — `language = es` (etc.) now unlocks the existing `lang_menu.c`
  translations on fan-USA discs, and the title-options Language row shows
  there too.
- **PAL fandub needs zero engine changes**: it only edits the Spanish assets
  (VIN4 overlays, ITEM_SPN.BIN, TIPS_S) with identical structure/encoding —
  the whole thing rides the existing `language = es` pipeline.

## Randomizer gamemode (2026-07-14)

Config `randomizer` (default 0); design doc `docs/Randomizer_Mode.md`. New Game
always opens in map2_s04; every door is rerolled into locked / another area /
another room in this map / a miniboss / (1%) the final boss; each area gets 1-5
pooled monsters and rerolled item pickups; after 10 areas the run ends at the
map7_s03 boss with a score-picked ending. `randomizer = 0` changes nothing.

Rests on three facts about the engine, all verified against the map data:

1. **A door is just an `s_EventData` row** (`sysState` = LoadOverlay/LoadRoom).
   Randomizing doors = rewriting rows. On a LoadRoom row the `mapIdx` field is a
   **BGM track**, not a map.
2. **A door's arrival mapPoint lives in the SOURCE map but holds DESTINATION-space
   coordinates** (`D_800BCDB0 = mapPoints[eventParam]` is read *before*
   `GameBoot_MapLoad`). So a teleport into map X must reuse a record that some
   other map authored for its real door into X — `tools/gen_rando_data.py` harvests
   them (`RANDO_ARRIVALS`). No hand-placed coordinates anywhere.
3. **A locked door is a SECOND row on the same doorway** selecting the shared
   `MapEvent_DoorLocked`/`DoorJammed` — which exist at `mapEventFuncs[0]/[1]` in
   only **25 of 43 maps** (`RANDO_DOORFN_MASK`). Hence the mode appends its own
   handler so locking works uniformly.

- **Files**: `pc_port/src/pc_rando.c`, `include/pc_rando.h`,
  generated `include/pc_rando_data.h`, `tools/gen_rando_data.py`.
- **No map-DLL data is ever written**: the mode installs its own copy of
  `s_MapOverlayHdr` (the same swap `lang_text.c` uses) and rewrites the event /
  event-func / message / spawn tables inside the copy. It runs at the **tail of
  `GameBoot_MapLoad`, after the language patch**, and copies from whatever header
  is live — so it inherits translations instead of clobbering them.
- **Gotchas paid for during implementation**:
  - `TriggerType_None` is **never a door**. The miniboss post-death exits are
    `TriggerType_None` + LoadOverlay rows; treating one as a door strips its
    `requiredEventFlag` and teleports the player out of the arena on arrival.
  - map1_s05 / map4_s05 have exactly **one** spawn row and it **is** the boss —
    clearing native spawns there deletes the boss.
  - Monster placement must wait for the first gameplay frame (collision data), but
    `GameBoot_InGameInit` spawns before then, so native rows are cleared at load.
  - `ovlEnemyStates[mapIdx]` + `field_228C` persist across visits; both must be
    reset or a re-entered area is empty. Pickup event flags likewise retire a
    trigger for good — the mode learns which flags are pickups (any flag
    `Event_ItemTake` is called with) and clears them on re-entry.
  - The one degenerate arrival record in the game (map4_s05 -> map2_s02 is a (0,0)
    placeholder; vanilla repositions via the death cutscene) is filtered by the
    generator — teleporting to it drops Harry at the world origin.

## PGXP reached everything except the item models (2026-07-14, commit `6a9cb2c05` + PsyCross `638dc9e`)

**Symptom**: `use_pgxp` had no effect on the inventory carousel or the world item
pickup. Those models rendered affine (wobbling/swimming textures) whether PGXP was
on or off, while the rest of the scene responded to the setting normally.

**Root cause — a broken link in the shadow-propagation chain, not a disable.** PGXP
is address-keyed (`PsyX_GPU.cpp`): a vertex is precise only if a shadow entry exists
at the *prim field address* the GPU reads. Coverage is built by propagation along the
data path:

```
GTE store (gte_stsxy*)      -> Shadow_Store(destAddr, ...)
drawer copy (poly->xN = ..) -> Shadow_Copy(&poly->xN, &src)
GPU draw   (MakeVertex)     -> GetPreciseVertex(primFieldAddr, ...)
```

Both of the first two hops were missing on the item path:

1. The item models are the **only** users of `GsSortObject4J`
   (`item_screens_cam.c`), whose TMD drawers (`GsTMDfast*` in
   `pc_port/src/stubs/libgs_stub.c`) project via the PsyCross wrappers
   `RotTransPers`/`RotTransPers3`/`RotTransPers4`. Those wrappers use PsyCross's own
   `gte_stsxy*` macros (`psx/inline_c.h`), which — unlike the game's `gte_stsxy3c`
   in `pc_port/include` — **never call `PGXP_StoreAddr`**. Nothing was recorded.
2. The drawers then copy each projected word from a **stack temporary** into the prim
   field with a plain C store, so even a recorded shadow would not have followed the
   copy.

Result: `GetPreciseVertex` missed at every item vertex, returned `ppw = 0`, and the
vertex cleanly degraded to affine. Structurally off, regardless of the setting.

**Fix** (both halves gated on `g_PsxUsePgxp`; off = byte-identical):

- PsyCross `src/psx/libgte.c`: `RotTransPers`/`3`/`4` now `PGXP_StoreAddr` the
  addresses they write. `slot` = position in the 3-deep SXY FIFO at store time
  (2 = newest). The quad path captures v0..v2 **before** its follow-up RTPS, which
  otherwise shifts v0 out of the FIFO beyond recovery.
- `pc_port/src/stubs/libgs_stub.c`: new `TMD_PGXP3`/`TMD_PGXP4` macros call
  `Shadow_Copy` after each prim-field write — the item-path twin of `SH_PGXP_PROP3/4`
  in `bodyprog_80055028.c`. Applied at all 17 emit sites (9 triangle, 8 quad).

**Preserved**: the see-through fix is independent and untouched — `ITEM_PRECISE_SZ`
still feeds true per-vertex SZ via `PsyX_SetNextPrimSzExact`, and the
`PsyX_ForceItemDepthBegin/End` bracket in `game_main.c` still forces per-pixel depth
around the item-only OT0 draw.

**Side effect, deliberate**: `water.c` passes its prim fields *directly* as the
`RotTransPers4` output addresses, so water quads now also become PGXP-tracked when
PGXP is on (previously affine). This is the intended behaviour of the setting, but it
is a visible change to water with `use_pgxp = 1` and is worth an A/B.

## World-pickup freeze engages at the interaction start, not model-load end (2026-07-21)

**Symptom**: examining a common ground pickup (First Aid, Health Drink, ammo) made
the camera briefly slide to a new angle and freeze there under the rotating item
until Yes/No.

**Root cause**: the see-through freeze (`Gfx_PickupItemAnimate` sets
`BgmStatusFlag_Pause` + `g_PsxPresentLastFrame`, isolating the item in OT0 — see the
PGXP/see-through section above) engages only once the model finishes its **async**
load, several frames into `Event_ItemTake`. Through that variable-length load window
the world still renders live, so `vcMoveAndSetCamera` (gated on `!BgmStatusFlag_Pause`,
`game_sys_states.c`) keeps easing the fixed camera toward its zone target. The freeze
then snapshots the camera **mid-ease** — a shifted, "stuck" backdrop whose angle
depended on how long the load took.

**Fix** (`events_util.c`, `Event_ItemTake` `EventState_LoadItemModel`, `#ifdef
SH_PC_PORT`): arm the same pause + present-last-frame + `g_PcPickupItemActive` at the
**start** of the interaction (the `LoadItemModel` case, which the `Initialize` case
falls through into), so the pre-examine frame the player was already looking at is
held for the whole pickup. The model still loads — `Fs_QueueChunksLoad` runs outside
the pause gate. OT0 is empty while paused-and-not-yet-loaded, so arming
`g_PcPickupItemActive` early only extends the existing release edge over the load
window; the depth bracket has no item prims to touch until `Gfx_PickupItemAnimate`
draws the model, so the see-through fix still never reaches the world. **Common
pickups only** — the special key pickups (`func_800DC778` etc.) play a scripted reach
animation (`Event_CharaAnimPlayToEnd`) before their `Gfx_PickupItemAnimate`, so their
camera movement is intended and their timing is left alone.

## Alt cameras stand down for scripted scenes; optional TPS/OTS camera collision (2026-07-14, commit `ff77c3d85`)

**Symptom**: small in-engine scenes — Harry's sewer ladder descent above all — were
played from the follow/eye camera instead of the scripted shot, which ruined them.
The FPS camera in particular should only be live during actual gameplay.

**Root cause**: `Pc_TpsCamera_Apply` (the one function that applies TPS/OTS/FPS) only
bailed on the two LETTERBOX markers, `SysFlag_CutsceneActive` and
`cutsceneBorderState`. The small scenes raise neither — `map5_s00.c:685` is a plain
`Player_ControlFreeze()` + `Event_CameraPositionSet()` under `SysState_EventCallback`
and touches neither symbol. So the alt camera happily overrode the scripted camera.

**The guard, and why it looks the way it does.** `Pc_ScriptOwnsScene()` = a letterboxed
cinematic, OR the script driving **both** the camera and Harry. Two adversarial review
rounds killed the obvious single-term versions, each with a concrete break:

| Candidate term | What it breaks |
|---|---|
| `VC_USER_CAM_F` / `VC_USER_WATCH_F` alone | **No alt camera for the entire final boss fight.** `map7_s03`'s `Map_WorldObjectsUpdate → func_800E14DC` re-raises these *every frame* of the Incubus fight while the player has full control. Same class: `map6_s04` (Cybil), `map6_s02` (ladder), `map1_s03` / `map2_s01` region cams. The flags mean "somebody called `vcUserCamTarget` this frame" — not "a scene is playing". |
| `g_Player_DisableControl` alone | **Camera pops to classic on every memo and every item pickup.** `Event_ItemTake` freezes at `EventState_Initialize`, several frames *before* `Gfx_PickupItemAnimate` sets `BgmStatusFlag_Pause`, so the world block still runs in that window. `SysState_ReadMessage_Update` freezes every frame and never sets Pause. The flag means "the engine is driving Harry", which ordinary interactions do too. |

Together they are true exactly for the scripted scenes. `SysState_ReadMessage` is
excluded outright — examining a memo is never a scene (`control_style.c` already makes
the same carve-out). Note that standing down does **not** freeze the view:
`vcMoveAndSetCamera` has already placed the game's own camera that frame, so skipping
our override simply lets it through.

**Bonus fix**: the `fps_fov` block used to sit *after* the early-return inside
`Pc_TpsCamera_Apply`, so a stand-down skipped the restore and left the FPS FOV clamped
onto the scripted shot for the whole scene. It is now `Pc_FpsFov_Update()`, called on
both exits.

### `tps_camera_collision` (default 1)

Launcher Controls checkbox ("Allow thirdperson camera collision") + in-game Controls
options row + config key. Off = the TPS/OTS **render** eye keeps its full orbit
distance and is allowed to pass through geometry.

**The trap**: `g_TpsCamPos` is not just the render eye — it is the **free-aim ray
origin** (`player_control.c` `Ray_CharaTraceQuery`, and `Pc_AimAssistFind`'s cone
apex). The level trace is **double-sided** (`ray.c`: the surface test is a pure
straddle test, and the `gte_nclip` backface reject only runs for `useCylinder`), so an
origin sitting behind a wall hits that wall *first* and flips the shot ~180° back into
it — Harry would fire backwards whenever he backed into a corner.

So the pull-in still runs with collision off, and the aim origin is slid **along the
view line** by the pull distance. It is *not* set to the pulled-in point: that point
lies on `pivot → eye`, which is ~11° off the view axis (`tpLookAt.vy` is anchored to
Harry's chest, not the eye) and is measured from an un-shifted pivot, so it also
cancels part of the OTS shoulder offset. A ray from there is *parallel* to the line
the reticle draws — a constant miss at every range. Projecting the displacement onto
`g_TpsCamFwd` keeps origin and reticle collinear.

With collision on, the slide is skipped and the default path is byte-identical.

## Thirdperson FOV + aim-zoom sliders, OTS aiming in TPS (2026-07-14, commits `29f0633c8` + `5169c9ea6`)

Three launcher options (Controls dialog) with matching console commands. The in-game
rows landed a commit later on a new 4th options page — see the section after this one.

### `tps_fov` — default **71.1**, and why it is not 67.4

Thirdperson/OTS field of view, mirroring `fps_fov`. `Pc_FpsFov_Update` became
`Pc_CameraFov_Update` and now serves both cameras; Classic always keeps the game's
own projection.

**The default is load-bearing.** The GTE projection distance the game uses in
gameplay is `vcWork.geom_screen_dist`, which `vcExecCamera` writes via
`SetGeomScreen` every frame and which equals `g_GameWork.gsScreenHeight`. Gameplay
runs **progressive** (`Screen_Init(SCREEN_WIDTH, false)`), so that height is
`FRAMEBUFFER_HEIGHT_PROGRESSIVE` = **224**, not 240. On the 320-wide frame, H = 224
is a true horizontal FOV of `2*atan(160/224)` = **71.1°**.

`SetGeomScreen(h)` is only a genuine no-op when `h` equals the value the game already
set. 71.1° maps to `round(160/tan(35.55°))` = exactly **224**, so the default changes
nothing. Had this shipped with 67.4 (→ H = 240) every Thirdperson/OTS player's FOV
would have silently narrowed.

> **`fps_fov` had the same off-by-16 — fixed in `870d52a8f`.** Its shipped 67.4 default
> mapped to H = 240 while the game's real gameplay H is 224, so "default" first-person
> was slightly *narrower* than the game's true FOV, and its code comment claiming 67.4
> was "the game's native FOV / byte-identical" was simply wrong. Now 71.1 everywhere:
> the config default, `config.cfg`, the `FOV` console command (value + `default`
> keyword + its message), and the launcher (load, reset, `ClampFov`'s parse fallback,
> tooltip). 67.4/240 is the *interlaced* height — never propagate it to a new camera.

### `tps_aim_zoom_amount` — default 100 (0..200), replaces the on/off checkbox

Scales the aim dolly along a 0..200% range whose full-scale (200%) pulls **twice** as
far in as the old zoom: `TP_DIST_AIM_MAX = 2*TP_DIST_AIM - TP_DIST`, and `aimDist =
TP_DIST - ((TP_DIST - TP_DIST_AIM_MAX) * pct) / 200`. At **100% (the default)** it lands
on exactly `TP_DIST_AIM` (the original zoom — so existing configs saved at 100 are
unchanged), at 200% on `TP_DIST_AIM_MAX` (a new, deeper 2x pull), at 0% on exactly
`TP_DIST` (no zoom). The 0..200 scale was chosen over an earlier 0..100 (50=old) exactly
so a config already carrying `tps_aim_zoom_amount = 100` keeps its original feel instead
of jumping to the deepest zoom. The legacy `tps_aim_zoom` bool key is still parsed (→ 0
or 100) so an existing config migrates instead of reverting to the default; `ConfigManager`
appends new keys at EOF and the game parses last-assignment-wins, so the new key always
beats a lingering legacy line. Launcher slider + `ClampAimZoom` cap at 200.

### `tps_ots_aim` — default 1

Raising the gun in Thirdperson eases the camera into the Over-the-Shoulder framing
and back out when lowered; the shoulder-swap bind works there too. Implemented by
giving TPS a *resting* offset of 0 in the existing OTS block:

```c
restOff   = (style == Ots) ? OTS_OFFSET : 0;
targetOff = (isAiming ? OTS_OFFSET_AIM : restOff) * g_OtsSide;
```

The block is entered **every frame** in that mode — not only while aiming —
specifically so `s_otsOff` can ease *both* ways instead of snapping in from a frozen
value. With the option off, TPS never enters the block, so `s_otsOff` is neither
updated nor applied: byte-identical to before.

### Console

`TPSFOV <deg|default>`, `TPSAIMZOOM <0-100>`, `TPSOTSAIM [0|1]`, `CAMCOLLIDE [0|1]` —
all persist through `PcConfig_SaveKeyValue`.

### Why no in-game options rows

The PC options pages draw at `LINE_BASE_Y` 56 with `LINE_OFFSET_Y` 16 on a 240-line
screen, so a page holds ~11 rows. `PCOPT_C` (Controls) was already at capacity — the
`Camera_Collision` row added in the previous commit pushed `Back` to y = 248, off the
bottom edge, and has been reverted. Any further Controls rows need a 4th page.

## Cutscene timing overhaul — lossless clock + audio catch-up (2026-07-16, commit `983de8432`)

Root-cause batch for the widespread "subtitles at the wrong time / cutscene drifts
out of sync with the dialog" reports. A multi-agent audit of the timing stack
(core clock, DMS timeline, subtitles, XA voice, anim driver) produced 13
adversarially-verified findings; the fixes:

- **Lossless game clock.** The per-frame vCount pipeline lost real time at three
  truncation sites (`GsGetVcount` int cast → `GsClearVcount` epoch reset →
  `Q12_MULT` floor). The loss repeats every frame, so the whole game clock ran
  slow vs the wall-clock XA voices in proportion to fps: ~0.4% @60, ~3.3% @120,
  ~6.25% @240, ~27% uncapped (and stopped entirely above ~5.2kfps) — 6–11+
  seconds of scene/subtitle lag over a 3-minute cutscene. dt now derives from one
  cumulative Q12 clock (`GsGetCumulativeQ12`, fixed epoch); total error is
  bounded < 1/4096 s forever at any fps. Re-applies the *principle* of the
  reverted `edfe66887` — that revert's "map6_s04-only" premise is contradicted by
  the current reports, and it was evaluated under the since-fixed PAL-50Hz vblank
  bug with the genuinely-regressive cap-skip also active. Differences: single
  clock source, no carry statics, catch-up bounded by the PSX step (below).
  [`game_main.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/bodyprog/sys/game_main.c) ·
  [`libgs_stub.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/pc_port/src/stubs/libgs_stub.c)
- **Cutscene audio catch-up.** The 30fps cap + 15fps floor permanently discarded
  wall time on every slow frame (each load hitch pushed the scene further behind
  the still-playing voices; steady sub-30fps ran the whole scene in slow motion
  vs audio). During cutscenes the discarded time accrues to a bounded (2s) debt
  repaid by later fast frames — every frame's dt stays ≤ the PSX 30fps step
  (136 Q12), so DMS/anim stepping never sees a larger step than original
  hardware (the regression mode that killed the old cap-skip `18e35f202`).
  Also unifies `g_DeltaTimeRaw = g_DeltaTime` during cutscenes: subtitles /
  message timers / event waits ran on the raw clock, up to 2× the capped
  DMS/anim clock below 30fps; on PSX these were one variable.
- **ANIM-STUCK detectors on the anim clock.** The (A)/(B)/(C) bypass timers in
  `player.c` accumulated `g_DeltaTimeRaw` while the anims they watch advance on
  `g_DeltaTime` — below 30fps the detector ran up to 2× fast relative to the
  anim and could force-skip scene content. Now accumulate `g_DeltaTime`.
- **Dropped-voice subtitle release.** A page whose voice cmd is range-guarded
  away (table-overrun protection in `Event_DisplayMapMsgWithAudio`) waited out a
  1s fail-open before its text rendered. The drop now sets
  `g_PcMapMsgVoiceDropped` and the subtitle releases immediately; a `[MSGVOICE]`
  log marks each drop (diagnosis lead for the audioCmds-overrun class — six maps
  carry PC pad rows; a per-table length fix remains open).
- **map7_s03 boss motion dwell.** Projectile script nodes counted down once per
  *rendered* frame (8× fast at 240fps, 2× slow at the 15fps floor); dwell is now
  Q12 seconds consuming `g_DeltaTime` (`func_800D88E8`).
- **Good+ ending Aglaophotis bottle break on impact** (`func_800E514C`, commit
  `658b39399`). The thrown bottle hit the Incubator's head then hung intact ~0.7s
  before shattering. Its flight arc is case 27 (`g_Cutscene_Timer` 281→313); the
  scream (`SD_Call(Sfx_XaAudio606)`, case 30) fires at 313 = impact. The earlier
  fix keyed the shatter on `timer >= 320` (case 31's endTime = post-impact hold),
  guessing impact wrong. Shatter (`func_800D7144`) + hide the intact bottle now
  fire at case 30, so break/scream/reaction coincide. Lesson: a DMS projectile's
  impact anchor is the flight-arc end / impact-SFX beat, not the next
  `Event_CutsceneTimerAdvance` endTime.
- **Loose CHARA texture bleeding onto a building** (`src/main/fsqueue_3.c`, commit
  `cd8b1edab`). `gamedata/load/CHARA/BFLU.png` was painted on a random building wall.
  The hi-res loose-override "pending path" table (`s_hiresPending`) is keyed on the
  `s_FsQueueEntry*` pointer, which the FS queue RECYCLES; a stash not popped for its
  own load was popped by a building chunk (`THR2501F.TIM`) that reused the pointer.
  Pre-existing latent bug (in the 2026-07-16 baseline; introduced `17eab4827`),
  exposed by a CHARA loose mod — NOT the recent content-keyed-upload / mod-manager
  work (audited + exonerated). Fix: the stashed path's basename is the disc name it
  was stashed for, so at pop require it to match the loading TIM's disc name
  (`Fs_GetFileInfoName`); mismatch → drop, base disc TIM renders.
- **HD-font "ghost text" — restore baseline** (`hires_override.c` / `text_draw.c`,
  commit `a4cabe12c`, reverts `029b69a40` + `c60389147`). **Correction (2nd audit):**
  the menu A/O "ghost text" is NOT a regression from either commit — it is a
  *pre-existing* bilinear-magnification bleed on the HD pack's gutterless atlas. The
  font override uploads `GL_TEXTURE_MAG_FILTER=GL_LINEAR` unconditionally for upscaled
  packs (`hires_override.c:524`), and the 32-bit RGBA override shader ignores the
  `menu_filter`/`bilinear` toggle (`PsyX_render.cpp:1974`, `u_bilinearFilterLoc=-1`),
  so the bilinear tap at each fixed-12 UV cell edge reaches ~½ texel into the
  neighbouring inked atlas cell. Present at baseline `9fcc18b61`. The two reverts
  return HEAD to the clean baseline (correct) but neither *causes* nor *cures* A/O:
  mipmaps are min-filter only, and `c60389147`'s advance-clip fixes a *separate*
  narrow-glyph (advance<12: I/S/T/E/P) overlap — its `glyphWidth < 12` guard excludes
  A(12)/O(13), so the earlier claim that it "clips wide HD glyphs A/O" is wrong. Keep
  the mipmap revert (baseline; menu font is minified, no-mipmap build was worse).
  Real fix for A/O (not yet applied): force `nearest` on the font/2D-UI atlas upload
  (GL_NEAREST mag) or have the mod add a 1px gutter — `u_hiresHalf`'s existing
  half-texel inset is insufficient for a ~10× minified atlas. `f3c9cbbcd` kept.
- **fps_cap 31–59 honored.** Integer division (`60/fps`) silently turned those
  caps into 60fps; non-divisors of 60 now route through the SDL high-precision
  limiter.

Audit notes (verified non-bugs, do not re-investigate): DMS keyframe
interpolation is stateless (`dms.c` — a large step cannot smear across a camera
cut); `Anim_PlaybackOnce` clamps to `endKeyframeIdx` (equality waits are safe at
any fps); the typewriter's static glyph accumulator leak is ≤ 1 glyph; the MIN
double-eval of `GsGetVcount` loses no time (later sample is consumed).

## Trigger sweep + voice pacing follow-up (2026-07-16, commits `714f8caae`, `eeb1dadda`)

Completion of the timing overhaul's open items (map-wide multi-agent sweep, 61
findings adversarially verified; full voice-cmd table inventory):

- **PSX end-of-voice rhythm restored.** PSX ended a voice via the vblank
  watchdog at `audioLength+32` (~0.53s past the audio) — the authored
  inter-line pacing. OpenAL drain signaled instantly, so each line advanced
  ~0.5s early and long dialogs compressed. Drain now holds the finished signal
  until the watchdog moment; explicit stops still signal immediately.
  [`xa_player.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/pc_port/src/xa_player.c)
- **XA freezes with the console** (`XaPlayer_SetPauseHold`), and the sector
  math derives from the parsed subheader (all 726 USA voice clips verified
  stereo 37800 by reading the disc — the old hardcode was benign on USA but
  wrong for other discs).
- **KeyOfWoodman pickup freeze root** (`player_control.c`): the PC `field_38`
  overlay re-patch sat inside the `mapAnimIdx`-changed guard; a (re)load with
  an unchanged anim idx left `field_38` on map0_s00's linked table whose
  missing rows made scripted poses no-ops (state 59's `0x12C` row exists only
  in the map's own overlay table — verified by parsing `MAP2_S00.BIN`). The
  patch now re-derives on every load.
- **Frame-counted cutscene pacing dt-scaled** (8x fast at 240fps): map2_s00
  cafe-exit/sketchbook map zooms + marking fades; map1_s06 boiler slide;
  map3_s03 plates-door cursor/slides; hospital elevator cursor (shared hdr);
  map5_s01 safe dial; GAME OVER + death-tip holds; map1_s03 locker swings.
- **Voice-table hardening**: the severed alias `Map_MessageWithAudio` got the
  range guard + dropped-voice release + `[MSGVOICE]` log; map6_s02's exe-stub
  voice index now resets at scene start (PSX zeroed it as overlay BSS). The
  table inventory confirmed every other table ends in authored zero tails or
  lives in per-load DLL data — no per-table lengths needed; the six pads stay.
- **Verified non-bugs** (do not re-fix): generic `AwaitAnimEnd`-style `==1`
  polls are safe on PC (range-form `Player_AnimPlaybackStateGet` +
  self-linking one-shot map anims); NPC one-shot waits have no relink race —
  the c4c8d5369 class was player-lower-body-driver specific.
- **Known-cosmetic remainders** (verified minor, not fixed): map6_s04 carousel
  ramp/FX grid decays, map6_s03 corpse bob, map4_s03 theater blink, map5_s00
  sewer-scare pacing, map7_s03 FX grids; plus unverified-minor map7 close-up
  fades. All are per-frame FX pacing with clamps (no freezes possible).

## Characters darker than PSX — double fog on character prims (2026-07-16, commit `c3ed32011`)

User report (cafe-intro side-by-side vs DuckStation): characters render darker
relative to the environment than PSX. Measured off the captures: character-only
deficit ≈ ×0.90 = worldTint/boostedColor, world geometry + subtitles at exact
parity (the "vertical squish" half of the report measured as no difference).

Root: character/lit-model prims were fogged **twice** on PC. The CPU flat-light
dispatch (`func_8005A21C` → `func_8005A42C/A478/A838`) fed the fog-attenuated
alpha into the GTE depth-cue (the PSX mechanism — fade toward the black far
color, with the map-gain boost via negative-IR0 extrapolation: tint 121 →
boosted 133 in the cafe), and the prim builder *also* attached per-vertex fog
bytes (`PC_SCREEN_Z_TO_FOG`) that the PsyCross shader blends toward the fog
color — the PC mechanism that replaced CPU vertex fog for the *world*
(`VTXCOL_LDDP(dp)` → `gte_lddp(0)`). The shader bytes were added while
characters still rendered unfogged (the old `isFogEnabled=0` wrap around
Harry's draw); when that wrap was root-fixed, the compensation became a
double-application — characters darkened by both fades in every foggy scene
while the world stayed correct.

Fix: the PC character light dispatch uses the no-fog alpha, keeping only the
`field_20` light boost in the dpcs; the shader owns all character fog with the
same curve as world geometry. Verified faithful along the way: PsyX GTE `lddp`
(signed IR0) and DPCS negative-IR0 extrapolation, `lm_reformat` byte-0xB
bitfields, character model routing (`fB0=1` → lit drawer). `[LIGHTCMP2]`
probe (world_draw.c) remains until user confirm, then strip.

## Puppet Nurse crash after the Stone of Time puzzle — stale field_124 (2026-07-16, commit `9ad4f5455`)

User report: crash after doing a puzzle in Nowhere (map7_s01). Access
violation reading `0x2c0` in `PuppetNurse_AnimUpdate` (inside map7_s01.dll,
which `#include`s puppet_nurse.c via characters.c), right after a native
Puppet Nurse spawns.

Trigger: the Stone of Time pickup sets `EventFlag_M7S01_PickupStoneOfTime`,
so map7_s01's npcSpawnEvent `func_800DEDA4` calls
`Chara_SpawnFlagsSet(16, 3, SpawnFlag_0|1|3|4)` = 27 → spawn slot 3 fires a
nurse with `stateStep = 27` → `charStatIdx = 3`, `modelVariantIdx = 3`.

Fault (from disassembling map7_s01.dll + the exe): line
`(&animInfoBase[status])->playbackFunc(...)` with
`animInfoBase = field_124->animInfo_24 == NULL`, `status = 22` →
`22 * sizeof(s_AnimInfo)=0x20 = 0x2c0`. `field_124` is a valid **non-NULL**
pointer whose `+0x28` reads 0 — a **stale** value in the recycled npc slot
that does not point at `sharedData_800D5710_3_s03` (whose `animInfo_24` is
always populated by `PuppetNurseData_Init`). The data plumbing is fine: the
map DLL correctly PE-imports the exe's live, initialized instance array
(hospital nurses use the identical import; verified `objdump -p`), and
`__fu12` auto-import references — not copies — the exe's `.bss`.

The existing PC guard forced `PuppetNurse_Init` only on
`controlState==0 || field_124==NULL`, so a stale non-NULL `field_124`
slipped through → Init skipped → the stale pointer's null `animInfo_24`
dereferenced. Fix: harden the guard (nurse + doctor) to also re-Init when
`field_124->animInfo_24 == NULL` (short-circuits after the NULL check).
Re-Init restores `field_124 = &sharedData[charStatIdx]` from the intact
`model.stateStep`, so the nurse animates correctly.

LESSON: a re-Init guard keyed only on `ptr == NULL` misses **stale non-NULL**
pointers in reused npc/object slots. When a "was this initialized?" guard
must survive slot recycling, test a field the initializer always populates
(here `animInfo_24`), not just the top-level pointer.
[`puppet_nurse.c`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/src/maps/characters/puppet_nurse.c#L342)

## map6_s04 Flauros cutscene desync — unpad the subtitle page-advance gate (2026-07-16, commit `d9a34e548`)

The Alessa/Harry/Flauros amusement-park cutscene (`func_800E3EF4`, AMUSE2.DMS;
Harry shoved back, "Damn!" = msg 48) drifts progressively out of sync —
dialogue and subtitles fall seconds behind the on-screen action, worst by
"Damn!". Root-caused by a multi-agent trace of build `60e049203` (includes all
prior timing fixes) + the user's `alesssa.log`; two adversarial verifiers
confirmed.

Root: the `714f8caae` end-of-voice pad (holds the XA "finished" signal ~490 ms
past real audio drain) leaks into the PC-only subtitle page-advance gate. A
voiced page auto-advances only when `mapMsgTimer==0 && !pcVoiceHold`, and
`pcVoiceHold = VoiceDialog && Sd_AudioStreamingCheck()==1`, which stays 1
through the pad. So each voiced page held `max(authored ~J timer, voiceLen +
490 ms)` instead of `max(authored, voiceLen)`. The scene's authored `~J` page
timers were tuned to slightly exceed the voice content (msg42 monologue:
authored 12.0 s vs voice 10.6 s), so the pad flips the winning term on
essentially every page → +~490 ms per voiced line accumulating across the
serialized dialogue track, while the DMS animation runs at true wall-clock
(`983de8432`, correct). On PSX this path had **no** voice gate at all
(`pcVoiceHold`/`D_800BCD74` are entirely `#ifdef SH_PC_PORT`); pages advanced
on the `~J` timer alone. Log proof: every voice `playedMs = expMs + ~490 ms`,
voices strictly serialized (no overlap — refutes the pile-up theory).

`983de8432` (lossless clock) is the **enabler not the regressor** — it made
the animation wall-clock-correct, unmasking the pad's lag; kept. Typewriter
(`377fff821`) benign. No reverted regression touched.

Fix (surgical): `Xa_IsVoiceAudioDraining()` (xa_player.c) — true only while the
voice is actually producing audio (mirrors `XaPlayer_Update`'s true-drain test
but excludes the `s_xaPadEndMs` tail). `pcVoiceHold` (map_msg_display.c) now
gates on that instead of the padded flag, so voiced pages advance at real
audio drain — authored `~J` pacing restored, dialogue re-aligned to the
animation. The pad + guard stay intact for their other consumers (the step-43
inter-DMS load barrier, BGM transitions, console-freeze pause, PAL/JP sector
math). `pcVoiceHold` still prevents PC's instant next-line SD_Call from cutting
a live voice (the PR#17 anti-overlap fix). Under `SH_PC_PORT`; 30fps PSX build
byte-identical. Helps every voiced cutscene.

LESSON: a global "voice finished" pad meant to replicate a PSX watchdog must
not feed a gate that another authored timer already covers — it double-counts.
Scope such pads to the ONE consumer that needs them.

## map6_s04 Flauros cutscene voice desync — resume-not-restart voice index (2026-07-17, commit `554559f69`)

Long-standing (since the port began): the amusement-park Flauros cutscene
(`func_800E3EF4`) is badly out of sync — "as soon as Flauros appears you hear
Alessa scream 5-10s before she should on screen, then the whole scene is out of
sync." Root-caused by a multi-agent trace + 2 adversarial verifiers against the
voice table and the log.

Root: a shared monotonic voice index over-runs by +1 — the `fea838462` map7_s00
class. `func_800E3EF4` displays msg47 at BOTH case7 and case9, sharing one index
(`D_800ED5AC`) into the 34-entry table `D_800EBA64`. Case7 runs the message
alongside a cutscene timer with `autoAdvance` (frames 24→39 = 1.5s); the step
advances to whichever finishes first. **PSX**: CD-load latency keeps the display
from completing before the timer hits 39, so case9 RESUMES it (`isMgsStringSet`
stays true) and fires no new voice. **PC**: the voice loads instantly, msg47
completes in ~1.1-1.4s (< 1.5s), so `isMgsStringSet` goes false and case9
RESTARTS fresh — the setup path returns `MapMsgState_Finish` and fires an EXTRA
`SD_Call` + index bump. The +1 shifts every later line to the next clip, so the
373ms scream/gasp (`D_800EBA64[6]`) fires at case9 (~7.8s before its case17
Flauros beat), then the whole scene is shifted. Log confirms: strict in-order
walk `537..554` then a lone `[MSGVOICE]` drop of `0x0000` at `audioIdx=34` = one
extra Finish (35 fired vs 34 authored).

Fix (shared, PSX-faithful): suppress the voice fire + index bump for a
setup-Finish that is a same-index RE-DISPLAY of the just-completed line
(`map_msg_display.c` flags it via `msgIdx == mapMsgIdx` in the setup branch;
`Event_DisplayMapMsgWithAudio` skips fire+increment when flagged). Reproduces
PSX's resume-not-restart single-fire, keeping the index in lockstep. The flag is
0 for page-advance Finishes (multi-page lines always fire) and genuine first
displays. `SH_PC_PORT`-only; 30fps PSX byte-identical. Generalizes to map7_s00
(its `fea838462` pad becomes redundant but harmless). The `d9a34e548` pcVoiceHold
unpad is KEPT — it targets the separate ~490ms inter-line pad accumulation and is
orthogonal.

LESSON: msg-index selects the SUBTITLE TEXT but a separate shared monotonic index
selects the VOICE CLIP; anything that changes the page-Finish COUNT (instant PC
voice load winning a message-vs-timer race, an extra typewriter break, a re-
display) walks the voice index off its lines. When PC timing flips a msg-vs-timer
race the game authored around CD latency, restore the PSX resolution, don't just
pad the table (padding only absorbs a tail drop, never a mid-scene shift).

## map7_s00 Lisa cutscene — no-op the spurious restart of a multi-page chain (2026-07-17, commit `8a5cfd53c`)

The Lisa death scene (`func_800D0B64`) reported: "halfway through, Harry says
'It's a temporary thing' twice, then the audio runs out before the subtitles."
Same case7/case9 re-display class as Flauros above, but the MULTI-PAGE variant.

The scene shows the `msg30->34` CHAIN (only msg34 carries `~E`; 30 "Harry help
me", 31 "so scared", 32 "It's only a temporary thing", 33 "shock", 34 "don't
fret") at BOTH case7 `DisplayMapMsg(30)` and case9 `DisplayMapMsg(30)`, sharing
`g_Cutscene_MapMsgAudioIdx` into the 23-entry `g_Cutscene_MapMsgAudioCmds` (idx
0-22, then a `0x0000` terminator + the `fea838462` pad). PSX: CD latency keeps
case7's chain mid-flight when the step advances, so case9 RESUMES it (no re-show,
no new voice). PC's instant load COMPLETES the whole chain at case7, so case9
RESTARTS it — every page's subtitle re-shows ("temporary thing" a second time)
and pages 2..N re-fire their voices, walking the audio index past idx 22 into the
`0x0000` pad, so the tail lines (msg35..37) fall silent.

`554559f69`'s voice-only suppression only covered the chain's FIRST re-display
page, so the continuation pages still leaked. FIX (`map_msg_display.c`, top of
`Gfx_MapMsg_Draw`): a re-display (`msgIdx == mapMsgIdx && !isMgsStringSet`) whose
just-completed display was MULTI-PAGE (`g_MapMsg_CurrentIdx != msgIdx`) returns
`MapMsgState_SelectEntry0` immediately — a full no-op that just advances the scene
step, matching the PSX resume (no subtitle re-show, no voice, no index bump).
Cutscene-gated (`SysFlag_CutsceneActive || cutsceneBorderState`) so ordinary
memo/item re-examination is untouched. `SH_PC_PORT`-only; 30fps PSX byte-identical.

DISCRIMINATOR: `g_MapMsg_CurrentIdx` (last displayed page) vs `msgIdx` (chain
start) differ only for a multi-page chain, so SINGLE-page re-displays (map6_s04
Flauros msg47, `CurrentIdx == msgIdx`) fall through to `554559f69`'s lighter
voice-only path and keep their proven step timing. That matters: Flauros's case9
has `autoAdvance=false`, so its message-completion duration gates when
`Chara_Load(Flauros)` fires — an instant no-op there would shift the Flauros model
~1.7s early. Lisa's case9 timing is decoupled (the DMS animation runs off
`g_Cutscene_Timer` every frame at the bottom of the scene function), so the no-op
is safe. LESSON: the case7/case9 resume idiom has two shapes — single-page
(voice-only suppress) and multi-page chain (no-op the whole restart); tell them
apart by `g_MapMsg_CurrentIdx` vs `msgIdx`.

---

## Manual reload: frozen input edge cache + unlatched request (`5d94ae12f`)

`pc_port/src/pc_combat.c`, `pc_port/include/pc_combat.h`,
`src/bodyprog/player_control.c`

ROOT: the rising-edge caches keyed their "resample once per frame" test on
`g_VBlanks`, which is a per-frame vblank DELTA (`VSync(SyncMode_Count) -
g_PrevVBlanks`, pumped to `effectiveMin`, clamped `MIN(...,V_BLANKS_MAX=4)`),
NOT a monotonic counter. At a steady frame rate it is constant (1 @60fps, 2
@fps_cap=30), so `if (s_frame[slot] != g_VBlanks)` never fires again after slot
creation and the cached edge FREEZES — stuck false (bind dead) or stuck true
(action re-fires every frame with no press = the reported accidental/double
reload). Input was only really sampled on frame-pacing hitches. Cycle Weapons,
Quick Heal and Quick Turn shared the defect. FIX: monotonic `g_PcInputFrame`,
bumped once per frame in `Pc_ExtraActionsUpdate` (called unconditionally before
the state dispatch). Keep the INEQUALITY test — a `>` ordering test stalls on wrap.

ROOT 2: the reload bind was sampled ONLY from two aim-gated call sites, and
prev-state only advances when that scancode is passed in — so it went stale
whenever the gun was lowered. Hold the bind while walking, then aim → rising
edge that never happened (phantom reload on aim-entry); release it mid-reload →
next press swallowed. FIX: sample every frame (as cycle/heal/QT already did for
this exact reason) and publish `g_PcReloadRequest`, latched ~0.30s, consumed
ONLY where a reload actually starts. Clearing it inside the query does not work
— free-aim queries it every frame including during recoil, so it would clear and
drop. Conditions are evaluated at PRESS time, so a press mid-reload is rejected
outright rather than queued; that is what stops the double reload.

Plus three state-ownership fixes in `Pc_FreeAimGunUpperBody`:
- Stale `s_state` reconciliation. Damage/grabs/death/inventory cancel a reload
  via `Player_ExtraStateSet` clearing `upperBodyState`, but they do it while
  `Player_UpperBodyUpdate` is skipped (`playerExtra.state >= PlayerState_Idle`),
  so the FSM never saw the cancel: stuck in `PcGun_Reload`, upper body locked,
  gun dead, until `++s_stuckTmr > 600` granted a SILENT FREE CLIP. Gate on
  `!freshAim` — the freshAim block legitimately sets `PcGun_Reload` before
  `upperBodyState` is Reload (`9a594e2fd`'s resume path); unguarded it stomps
  that and resurrects the double reload.
- `extra->model.stateStep = 1` at the top of `case PcGun_Reload`. The native
  `case PlayerUpperBodyState_Reload` re-inits on `stateStep == 0` and rewinds
  `keyframeIdx` to `D_800AF624`, so a camera flip mid-reload silently RESTARTED
  the reload (appeared to take twice as long, no second sound). Must be exactly
  1 — 0 is the sentinel.
- Completion leaves the native terminal aim state (Aim + `Unk34` + hold
  keyframe) instead of a stale Reload state, so a flip on the completion frame
  cannot replay the SFX or a whole second reload.

KNOWN-REMAINING (audited, not yet fixed): free-aim dry-fire is silent when clip
and reserve are both empty; free-aim reload SFX fires ~37 keyframes early vs the
classic camera (should be `D_800AF624 + field_9`); the reload keyframe pre-seed
at ~6272/6517 is dead code.

## Flashlight glow-mask blend-state leak (2026-07-20, "black wedge/banding" reports)

AMD-user reports of solid-black wedges/bands anchored to Harry, worst in
Nowhere (flashlight always on there). 54-agent adversarial audit; two real
defects found, both blend-state hygiene, neither vendor-specific in mechanism:

- **Game side** (`func_800414E0`, `bodyprog_80040B74.c`): with the per-pixel
  flashlight active, the additive center fan is suppressed AND its
  additive-restore `DR_TPAGE` was skipped with it — but the subtractive tpage
  at the head of the mask still ran, so the drawing env leaked ABR=subtract
  into the NEXT frame's untextured semi-trans prims (drawn dark/black until
  the first tpage-carrying prim). Fix: always submit the restore tpage; it
  draws no pixels, keeps the PSX state machine exact in every mode.
- **PsyCross** (`GR_SetBlendMode`): after the shadow pre-pass poisons the
  blend tracker to `-999` (blending actually disabled), the blended branch
  only re-enabled `GL_BLEND` when the tracker was exactly `BM_NONE` — a
  semi-trans split leading the post-shadow batch drew OPAQUE (solid black).
  Sentinel now treated like BM_NONE. Plus `ShadowTriangleCanCast` finite +
  magnitude guards (the PR#8 hardening TODO): `!(vsz>0)` rejected NaN but
  passed +inf, and vsx/vsy were never checked.

Also added a boot `[CONFIG]` fingerprint line (flashlight_mode, use_pgxp,
resident_textures, global_chara_pool) so remote logs self-describe the
render config instead of us asking every reporter for config.cfg.

## IPD chunk lifecycle hardening (2026-07-20, cascade-corruption investigation)

Follow-up to the black-wedge reports: audit of how a chunk buffer could ever
reformat into garbage geometry+collision that persists.

- **Reformat registry eviction + leak** (`IpdHeader_FixOffsets`,
  `bodyprog_80040B74.c`, commit `0b2d9e025`): the 256-slot registry never reset
  and overflowed silently → an unregistered chunk thrash-reloads every frame
  (escalating void/garbage). Latent with current maps (~136 addresses max) but
  armed by anything that grows the address set; also freed the previously
  leaked heap `s_LmHeader` copy on every re-registration.
- **Real IPD validation** (`IpdHeader_FixOffsets_PC`, `pc_port/src/
  ipd_reformat.c`): was one magic byte in the first sector. Now bounds-checks
  every section offset/count and uses the LM header's magic+version at
  `lmHdrOff` (the file's deepest section) as a TAIL-ARRIVAL sentinel — a
  partially-delivered or half-stale buffer ("frankenbuffer") is rejected into
  the designed skip+retry path instead of reformatting garbage and registering
  it as done. `[IPD-VAL]` log canary (capped at 32 lines).

## 0727 user-report batch (2026-07-27, 12 bugs root-caused via multi-agent investigation)

All root causes adversarially verified before implementation. Session log:
one full playthrough on build `46daf37f8` (reports/0727reports).

- **Cutscene rainbow bar, recurring** (`hires_override.c`, `e3598f829`): the
  PSX ghosting overlays (map4_s01/map4_s04/map3_s02) draw 16bpp framebuffer
  SPRTs whose `clut` is legitimately uninitialized (hardware ignores it in
  16bpp mode); the hi-res override honored the garbage as a virtual-pool key
  and hijacked a live resident texture. Virtual branch now rejects tp>=2
  tpages; one-shot `[HIRES] ignored virtual clut` canary proves firing.
  Supersedes the "P_TAG sibling prim" theory for the map3_s02 rainbow.
- **Plate of Queen tilted through desk** (`map3_s04_extracted_data.c`,
  `3e4d9a21f`): PSX VA 0x800CB35C is a DIFFERENT per-overlay variable in
  map3_s04 (zero SVECTOR3 rotation for PLATE_NE + six DR*_HID drawers) vs
  map3_s05 (SFX position); the cross-map exe promotion served the map3_s05
  value to both, read as {20889,1,0} = ~36deg pitch. DLL-local disc-exact
  zeros now shadow the exe stub (same class + same fix shape as the
  b2e4adecf elevator-chime entry). Regen config updated; NEVER bulk-regen.
- **Incubus lightning flood/missing** (`map7_s03_2.c`, `80889e4a9`):
  func_800DBA08 projects arg0[0] AND arg0[1] (PSX stack adjacency); PC stack
  layout put sp18 below sp10 so the second vector was a host pointer ->
  exploded quads (screen flood) or GTE overflow -> segments skipped (missing
  bolts). SVECTOR spVec[2] under SH_PC_PORT restores defined adjacency.
  Same class as the Split Head stack-frame fix. Bolt color fade is still
  per-rendered-frame (24 draws); 30Hz-tick fix designed, not shipped.
- **Good+ bottle-throw scream 0.6s early** (`map7_s03_3.c`, `5c342d2bc`):
  removed the b2565289e 0.7s "shriek lead" - its 0.5-1s PC XA latency
  premise measured false (~2 frames; playedMs==pad on all 33 clips in the
  session log). Authored fire-at-impact restored; ending runs ~0.7s slower
  (PSX-correct). If a user still hears trailing audio, config-gated lead
  only - never a hardcoded one.
- **Dahlia teleport FX invisible** (`map6_s04_2.c`, `b8939e101`): allocator
  used PSX byte offset +0x494 into the FX buffer; on x64 field_494 aligns to
  0x498, every spawn wrote -4 vs readers, [FXBUF-BAD] guard retired all 570
  particles. Struct-member access on PC; pins the old EXECUTING-0x7ffa crash
  source. One-shot [FLAUROS-RAY] probes added for the still-open ray
  question (suspect: per-frame sweep = 0.3s blink at 60fps).
- **VHS tape subtitles only after audio** (`map_msg_display.c`,
  `79239e5a1`): the tape is a J2 map-message cutscene (one 35s XA clip, 13
  timer-advanced pages), not an FMV; pcVoiceHold pinned the blank ~J0 lead
  page for the whole clip. J2 pages (+ their chaining lead page) are now
  exempt; peek gated on an active ~J page to avoid OOB on selection pages.
  Regression-test Flauros/Lisa/Kaufmann per-page voice scenes.
- **Lisa-intro FMV "no music"** (`fmv_player.cpp`, `5ae45f31c`): music
  (SEQ "Loneliness", BGM 802) plays correctly under the movie; the missing
  piece was PSX movie_main's SsSetSerialVol(0,80,80) = 80/128 attenuation of
  the movie audio bed, which the PC FMV paths never modeled - the static
  masked the quiet cue. Factor applied in FmvApplyVolume for all three
  paths; `fmv_psx_volume=0` opts out for remastered packs.
- **Combination-lock see-through** (`item_screens_*`, `game_main.c`,
  `unk_draw_800CD20C.c`, `98612c762`): the map5_s01 lock renders via the
  shared item path but under InGame/arg2=0, outside both gates of the
  radio-antenna fix. New g_PcPuzzleItemDepth (armed at the puzzle draw site,
  world paused so OT0 is reels-only) extends precise-SZ + force-depth to it.
  Dark-room ammo pickups: diagnosed PSX-authentic painter behavior (single
  OT link) made visible by flashlight contrast - enhancement deferred.
- **Chest lens-flare flicker/wander** (`water.c`, `69b58b39b`): Q12 stepLen
  vs Q8 deltaZ left the facing-factor knee unreachable - flare stuck on a
  linear cos ramp at <=50%, re-targeted by every chest-bone wobble. stepLen
  now Q8; knee fires at cos>=1/8 and the ramp target pins constant like the
  PSX seed-pixel test. Expect ~2x brighter than June-July builds (PSX-correct).
- **Cutscene FOV report** (`game_main.c`, `d0c0ca292`): sliders/alt-cam/
  60fps cap were already correctly gated (log-proven; cutscenes run the
  non-Gameplay VSync path and cannot exceed 60fps). Fixed the real residual:
  FOV release now restores vcWork.geom_screen_dist instead of stomping the
  letterbox zoom ramp for one frame. Open decision: widening flashlight
  modes 1-3 stand-down to non-letterboxed scripted scenes.
- **Cutscene overrides, unified predicate** (`game_main.c`,
  `bodyprog_80055028.c`): closes the open decision above. New
  `Pc_ScriptOwnsShot()` = `Pc_ScriptOwnsScene()` minus the room-transition
  fade-in (whose camera+freeze branch trips on every door, and whose camera
  IS the one gameplay resumes under). The per-pixel flashlight's whole-scene
  dim now stands down on that predicate instead of its own copy of the
  letterbox test, so the camera and the lighting hand back on the same frame
  — a scripted camera far from Harry no longer renders a near-black shot
  (the cone lights off-screen while the dim covers the frame). The shadow
  map follows for free (`GR_FlashlightShadowActive` gates on
  `g_PsyX_FlashlightActive`). `Pc_CameraFov_Update` now takes the caller's
  stand-down decision rather than re-testing, fixing first-person room
  transitions that kept the FPS eye but dropped `fps_fov` to the game
  projection for the whole fade-in. The 60fps cap is made explicit in the
  gameplay VSync branch for the frames of a scene that land there (the
  letterbox ramp outlives the event state); the non-Gameplay branch remains
  the primary gate. All of it is recomputed per frame from the predicate, so
  a skipped/aborted scene needs no restore path.

## PGXP depth channel — flat world depth was provably inert; now live + per-vertex (2026-07-27)

PsyCross `71ee9bf`..`6c7d191` + decomp `dfb05966b`/`1d7c6c795` (+ probe demotion).
The FLAT/WORLD depth classification shipped 2026-07-17 (GL_ALWAYS world painter,
`SplitDepthForPrim`) never fired in gameplay: `PsyX_ClearGteDepthTable` memset the
SZ table at every GsDrawOt start — between addPrim capture and the parse. A
runtime probe measured ~70k world entries/s captured and discarded with parse
hit = 0. Fix, in independently-tested steps, all PGXP-on only (`use_pgxp=0` keeps
the wipe + legacy formulas verbatim):

1. **Gen-stamped SZ table** (reuses `s_pgxpGen`, same lifecycle as the shadow/
   affine tables) replaces the wipe when PGXP is on; a reused packet address
   from a prior frame reads as a miss. On->off toggle self-heals (first off
   GsDrawOt wipes as before).
2. **One constant linear depth scale** for every source: `ndc(vz) = 1-2*vz/2^18`
   — flat world prims at `avg_raw + M` (M = `PGXPWALLBIAS`, default 64 SZ,
   writer-side so coplanar testers win LEQUAL), EXACT sans M, untracked (NONE —
   auto-captured TMD FIFO garbage) keeps the OT bucket seed, itself remapped
   onto the same scale via `PsyX_SetOtViewZShift` (world `SZ>>(arg3+2)`, TMD
   `p>>shift` ~ shift+2). Item pass excluded wholesale (legacy path verbatim).
3. **Per-vertex depth on opaque world writes only** (`_p1`/`a_extra.w` marker →
   vertex shader derives depth from the unquantized `a_pgxp.z` on the same
   scale). The GL_ALWAYS class never depth-tests itself, so per-vertex depth
   there cannot flicker — it makes the depth field actors test against
   per-pixel accurate. Semi-trans world prims are NOT marked (LEQUAL testers
   vs their coplanar host = the historical two-interpolants coin-flip that
   killed 4 prior attempts). `Gfx_MeshDraw` also cancels armed-but-culled
   one-shot SZ payloads at every exit (silent kind/depth poisoning).

Verified live by counters (`PGXPDEPTHSTATS`): armF~350k/s, capF = parse-hit F =
splitWORLD (~100k/s), staleR < 400/s, splitHW 794/4096, exhaust 0, no OT-shift
disagreement. Kill-switch: `PGXPWORLDDEPTH`. Remaining distant thin seams are
sub-pixel position cracks (PGXPEDGE/weld class), NOT depth — out of scope here.

## BC7 sub-rect texture-pack entries + faster PNG inflate (2026-07-28)

Two independent changes to the texture-pack decode path.

**1. `.dds` sub-rects were inert.** `TexPack_Compose` fed every match through
`Entry_LoadPng` → `stbi_load_from_memory`, which returns NULL for a `.dds`, so
no canvas was built and the entry was silently dropped. Only the whole-cover
`.dds` fast path (which uploads compressed, 4x cheaper VRAM) ever applied. On
the real SLES-01514 pack that is 121 of 12,227 files: measured end to end
against the actual pack, **40 of 1443 upload groups produced anything before,
1443 of 1443 after** (1403 composited canvases + the same 40 compressed
uploads). ~99% of that pack was dead.
- `Dds_DecodeRgba` (`pc_port/src/dds_load.c`) CPU-decodes mip 0 of a BC7 DDS to
  RGBA8 via vendored `bcdec.h` (v0.98, Unlicense/public domain, iOrange).
  Verified bit-exact against Pillow on 60 real pack files; the
  non-multiple-of-4 edge-block clip verified against cropped aligned references
  on 125 forced-odd sizes.
- `Entry_LoadPng` → `Entry_LoadImage` (`tex_pack.c`): sniffs the file magic
  (not the extension) and dispatches DDS→bcdec, else stb_image. Same
  `(rgba, w, h)` contract, so scale detection, `DecodeNative`, `BlitNearest`
  and the content-hash compose cache are untouched. An extra `*outStbi` out-param
  says which allocator owns the buffer (stb's vs plain `malloc`).
- The whole-cover compressed path is unchanged and still preferred: the
  single-match whole-cover check and the DDS-first collapse both run BEFORE the
  RGBA path. CPU decode is only reached when the entry cannot take that path
  (sub-rect, or multiple matches).
- Only BC7 (DXGI 98/99) decodes, matching the upload path's existing stance.
  Anything else — BC1/2/3/4/5/6H, uncompressed, legacy FourCC, truncated block
  data — now logs its format once via `SH_DBG` instead of vanishing.

**2. PNG IDAT inflate → miniz.** stb_image's own inflate was the single largest
cost in a pack PNG decode and upstream stb offers no hook, so `stb_image.h`
carries a 2-hunk local patch (`STBI_PNG_ZLIB_DECODE`, marked
`SH_PC_PORT LOCAL PATCH`, a no-op unless defined) and
`pc_port/src/hires_override.c` points it at miniz's `tinfl` — already vendored
for `.zip` packs, so no new dependency. Decodes into a right-sized buffer
(stb's `raw_len` guess is exact for non-interlaced PNGs) with the growing heap
decode as the interlaced fallback. Byte-identical output verified by full-content
checksum over every file in three test sets.
- `stbi_load_from_memory`, best-of-6: 2.537 → 1.958 ms/file (**-22.8%**) on the
  audit's 200-file SLES-01514 set; 2.238 → 1.744 ms/file (**-22.1%**) on 200
  files from the real Silent_Hill_HD009654 PNG pack.
- End-to-end `TexPack_Compose` on that HD pack (120 real upload groups,
  best-of-3): **29.72 → 23.53 ms per canvas, -20.8%**.
- The win is encoding-dependent: on Pillow-level-6 re-encodes of the same art it
  collapses to ~3%. libdeflate measured 1.104-1.174 ms/file (-42% to -56%)
  consistently across all three sets, but vendoring it means a multi-file,
  multi-arch C library, against this repo's single-header convention — NOT
  shipped; revisit if PNG packs become a hot path.

## Alt-camera view-matrix ordering + RA-toast blend equation (2026-07-30, commit `a2af0b907`)

Two user reports that looked like one camera bug and are not.

### Mall TV screens track the view under any alternate camera

`map4_s03`'s TV bank is not textured world geometry — `func_800D7718`
(`map4_s03.c:3951`) snapshots the global world→screen matrix through
`Vw_WorldScreenMatrixAtPositionGet` (`vw_calc.c:505`, reads `GsWSMATRIX`), loads it
into the GTE, and `func_800D88C8` (`:4676`) `RotTransPers`es each corner at `:4715`
and writes **finished 2D screen pixels** straight into the `POLY_FT4` at `:4731`
before `AddPrim` at `:4747`. The quads are frozen against whatever matrix was live
at bake time; the cabinets added alongside via `WorldGfx_ObjectAdd` (`:3975`) are
merely queued in world space and reprojected later, at draw.

That bake runs from `g_MapOverlayHdr.func_44`, which sat **above** the PC-port
`DebugCamera_Update()` call in `GameState_InGame_Update`. `DebugCamera_Update` →
`Pc_TpsCamera_Apply` ends with `Vw_SetLookAtMatrix` + `vwSetViewInfo()`
(`game_main.c:991-992`), which re-publishes `GsWSMATRIX`. So per frame:
classic camera published → TV quads baked with it → alt camera overwrites the
matrix → everything else draws with the alt camera. The screens kept the classic
camera's projection and stayed glued to the view. Under Classic nothing
re-publishes, producer and consumer agree, and the bug cannot appear.

Fix: move `DebugCamera_Update()` above the `func_44` dispatch — a matrix producer
belongs before its consumers. Also fixes the same skew for a non-default
`tps_fov`/`fps_fov`: `Pc_CameraFov_Update`'s `SetGeomScreen(h)` (`game_main.c:323`)
is reached only from inside `Pc_TpsCamera_Apply`, so it sat on the same wrong side
and the quads were baked with the wrong projection distance H too.

**Scope (corrected — the commit message overstates this).** `func_44` is non-NULL
in exactly three map headers. Only **map4_s03** and **map7_s03** straddle the
window: map7_s03 via `func_800E9874` → `func_800D917C` → `func_800D90C8` →
`Vw_WorldScreenMatrixAtPositionGet` (`map7_s03_2.c:1882`), reachable only during
the final boss (`D_800F4820 != 0`), where alt cams may stand down — mechanically
affected, symptom possibly unreachable. **map6_s02 is NOT affected**: its `func_44`
is `func_800D1AE4` (`map6_s02_2.c:1149`), pure flashlight/light-vector math. The
`Vw_WorldScreenMatrixAtPositionGet` at `map6_s02_2.c:1506` belongs to
`func_800D2364`, wired to the **`.func_A8`** slot (`map6_s02_header.c:77`) and
dispatched from `func_8005E89C`, already downstream of the camera apply.
Likewise `map7_s03_2.c:4371`/`:4456` are in `func_800DD6CC`, the boss-*character*
draw path (callers `incubus.c` / `unknown23.c`), not a `func_44` site.

Regression history: the ordering was **correct** at `3583772df` (the March chase
cam applied above `func_44`). `7ee17d07c` (2026-03-20) deleted that correctly-ordered
apply, leaving only the debug-only free cam below `func_44` — unreported.
`3d278fc44` (2026-06-22) then promoted TPS to a user-facing mode but routed it
through `Pc_TpsCamera_Apply` **inside** `DebugCamera_Update`, i.e. re-instated the
apply on the wrong side. `3d278fc44` is the regression commit for the symptom.

### RetroAchievements toast renders as a solid black slab

`Pc_RaToast_Draw` saved and set blend *enable*, blend *func*, depth, cull, program,
VAO/VBO, texture, active unit and unpack alignment — but never the blend
**equation**. `PsyX_render.cpp:4436`
(`glBlendEquationSeparate(blendMode == BM_SUBTRACT ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD, GL_FUNC_ADD)`)
is the only equation writer in the whole build, and it never restores `GL_FUNC_ADD`:
`GR_SetBlendMode` early-returns on same-mode (`:4394`) and on `BM_NONE` (`:4417`,
after only `glDisable(GL_BLEND)`), so an opaque split following a subtractive one
leaves `REVERSE_SUBTRACT` latched with blending merely off — and the toast's own
`glEnable(GL_BLEND)` resurrects it. `GR_EndScene` (`:748`) resets nothing, and
nothing between the last split and the post-capture hook touches it.

With the toast's `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` the result is
`dst*(1-a) - src*a`. Panel texels are (10,10,12) at alpha ~0.87
(`toast_build_panel`), so the panel becomes `0.13*dst - 8` ≈ black, and the
white text/badge quads (alpha ≈ 1) subtract to a clamped 0 and vanish entirely —
a black slab with no text, matching the report.

**Why aiming "fixed" it, and why it is not a camera bug.** `Pc_CrosshairDraw`
(`pc_crosshair.c:102`) is the port's only aim-gated draw and runs only in
TPS/OTS/FPS. It emits ABR=0 (`0xE1000200` → `BM_AVERAGE`) semi-transparent
`POLY_G4`s onto `g_OtTags0[buf][4]` (`:152`). Bucket 4 is the highest bucket any
game code uses, `GsClearOt` reverses traversal, and OT2 is the last `GsDrawOt` of
the frame — so the crosshair is genuinely the last blended split and leaves
`GL_FUNC_ADD` bound. Aiming sanitised the state; it never had anything to do with
the camera.

Fix: save `GL_BLEND_EQUATION_RGB/ALPHA`, force `glBlendEquation(GL_FUNC_ADD)`,
restore with `glBlendEquationSeparate` — exactly what `dbg_overlay.c` already does
at `:1888`/`:2079`. Swept for other exposed consumers: `Pc_MinimapDraw` is an empty
stub (`pc_minimap.c:773` — the minimap draws through PSX prims), `pc_mouse_cursor.c`
does no raw GL, and `fmv_player.cpp` is immune (`glDisable(GL_BLEND)` before its
draw). The toast was the only one.

## PAL documents undismissable — the port rendered PAL through the NTSC-U line cap (2026-08-01, commits `a94c5cda5`, `e197b5cfc`, `04cfaf89f`)

**Report** (issue #85): on a PAL Spanish build, reading the Norman's Motel
newspaper or the school music-room piano poem left the document on screen —
"the screen stays like this and can't do nothing". No log. The screenshot shows
exactly nine rendered lines, cut mid-sentence, with the game window still alive.

**Root cause.** `Gfx_MapMsg_StringDraw`'s parse loop is bounded by the rendered
line cap, and only three things write its return value: `~E` (`NO_VALUE`), `~S`
(the select code) and the terminating NUL (`1`). A message taller than the cap
ends the parse *on the bound* instead, so the return value keeps its initial `0`.
`Gfx_MapMsg_SelectionUpdate` passes that straight through, so
`map_msg_display.c`'s `stateMachineIdx1 != 0 && stateMachineIdx1 < MapMsgCode_Select4`
never fires — and that is the **only** assignment of `stateMachineIdx0 = NO_VALUE`,
which all four `stateMachineIdx1 = FINISH_MAP_MSG` sites sit behind. Dismissal is
unreachable forever; the button press only sets `msgDisplayLength = 400`.

**Why PAL only.** One PC binary serves every region and used the NTSC-U cap of 9
everywhere. Retail PAL uses **ten**, at every corresponding site — read out of the
decrypted retail overlays (EUR sha1 `f748528af…` = `configs/EUR/bodyprog.yaml`,
USA `eb118537b…` = `configs/USA/bodyprog.yaml`):

| site | EUR | USA |
|---|---|---|
| `CalculateWidths` loop bound | `0x8004ACFC 28C2000A` (10) | `0x8004AF04 28C20009` (9) |
| widths[] clear | `0x8004AA40` a2=9, base+36 (10 slots) | `0x8004ACF4` a2=8, base+32 (9) |
| `StringDraw` loop bound | `0x8004A42C 241E000A` + `beq $s0,$fp` | `0x8004B5EC 29C20009` |
| `{E}`/`{S}` "lineIdx = MAX" | `0x8004A58C`/`0x8004A5CC 2410000A` | `0x8004B360`/`0x8004B36C 240E0009` |
| positionIdx-4 anchor | `0x8004AE20 2402000A` | `0x8004B058 24020009` |

`MAP_MESSAGE_DISPLAY_ALL_LENGTH` is 400 on both, and the positionIdx 0/1/2/3
anchors are identical — PAL's 256-scanline field bought exactly one extra text
line, nothing else. The PAL localizers used it: the tallest single entry anywhere
on the PAL disc is exactly 10 lines. Fifteen shipped PAL messages exceed nine
(de 2, fr 4, es 4, it 5) — the PTV newspaper in Norman's Motel *and* its two
unreached copies in Nowhere, plus the piano poem. PAL-English and the USA disc
have **zero**, which is why this survived so long.

**Fix, part 1 — match retail.** `g_PcMapMsgLineMax` (`pc_port/include/font_region.h`,
defined in `font_region.c`) is 9 by default and raised to 10 by
`Font_ApplyRegionPatches`, which sits behind its own `g_GameRegion != Region_EUR`
early-return — so USA and NTSC-J are structurally untouched. `text_draw.c` uses it
via `MAP_MSG_LINE_MAX` at the eight **parsing** sites. The positionIdx-4 box anchor
deliberately keeps the literal 9: that constant is retail's centre in a 256-line
field, and the port draws PAL in the NTSC frame with its own `g_PsxMsgVShift`
compensation, so copying it would just drop every PAL document box 8px.

**Fix, part 2 — page instead of stopping.** The reporter is not on the retail disc:
their Spanish matches neither retail PAL nor our copy of the Spanish fan patch, so
it is a newer third-party revision that no data-side fix can anticipate. So the
renderer now *pages*: on ending a page at the bound (or at a `~P`, or at the
400-glyph rollout ceiling) it calls `Pc_MapMsgPageBreak`, which records the resume
offset and returns `1` — the code the engine **already** uses for a message ending
at a bare NUL — and `Gfx_MapMsg_Draw` turns the page in place instead of stepping
`g_MapMsg_CurrentIdx`. `Gfx_MapMsg_CalculateWidths` measures from the same offset.
This needs no message-table surgery, so the randomizer, text overrides and language
packs are all unaffected. Colour (`~C`), alignment mode (`~M`/`~T`) and the `~T`
inset origin are latched at the break and restored on resume — the caller resets the
colour to white every frame, and a continuation page starts *after* the codes that
set them.

**Fix, part 3 — keep retail's own page breaks.** When `Pc_LangPatchMapMessages`
joins retail's split parts (to preserve the compiled maps' message numbering) and
the join exceeds the cap, its seams are promoted from `~N` to the PC-only `~P`, so
each part lands on retail's boundary rather than being cut mid-part.
`MsgCollapseOneBlankLine`, which used to buy headroom by deleting authored blank
lines, is retired.

**A subtlety worth keeping.** `CalculateWidths` counts the `~N` that *opens* the
line it never draws, so a page ending on the bound left `g_MapMsg_WidthIdx` one
above the drawn line count — pushing bottom-anchored boxes up by a line (a visible
jump on every page turn) and letting the longest-line scan read
`g_MapMsg_Widths[MAP_MSG_LINE_MAX]`, the one slot the clear loop does not cover.
Clamped in `04cfaf89f`. Reachable only on the continuation path, i.e. only on
fan-translated discs — retail PAL always breaks on an explicit `~P`.

**Verification.** Simulating the joiner + parse loop + state machine over all 45
maps × 5 PAL languages (9975 messages): 15 soft-locks before, **0** after, every
drawable glyph still reached, only the fr/it piano poem spanning more than one
page. 707 compiled USA map-message literals, none over nine lines, so the
continuation path is never entered there. The USA-region Spanish fan patch's
10-line Alessa-fire newspaper (map7_s01 #33 / map7_s02 #18) pages 9+1 with all
155 glyphs reached. Not a memory-corruption crash: `GsOUT_PACKET_P` is a 16 MiB
arena with a canary, and the worst-case document is ~38 KiB of `POLY_FT4`.

## Play as another character — Lisa / Cybil / Kaufmann / Dahlia (2026-08-03)

**Feature, PC-only.** `player_character` config key, console `PLAYAS <name>`, and
in-game cycling: with `allow_debug_controls = 1`, turn on the K keyframe viewer
and press `-` / `=` to cycle Harry → Lisa → Cybil → Kaufmann → Dahlia (the amber
panel shows the selection; the rifle/shotgun give cheats on those keys stand
down while K view is on). Turning K view off leaves you playing as the selected
character; the choice persists to `config.cfg`. Groundwork for character
selection (and eventually co-op skins).

**Mechanism** (`pc_port/src/pc_playas.c`): retarget
`CHARA_FILE_INFOS[Chara_Harry].modelFileIdx/textureFileIdx` — the same row
mechanism as the PAL Mumbler censorship and the pool's beta retargets — and
rerun `WorldGfx_HarryCharaLoad` + `WorldGfx_PlayerModelProcessLoad`
synchronously (pool-style `Fs_QueueUpdate` pump; the player draw has no
`isLoaded` gate, so the buffer must never be mid-read when a frame renders).
`animFileIdx` is never touched: HB_BASE.ANM, the weapon/map keyframe banks and
`playerBoneCoords` keep animating, so the skin performs every Harry animation.
Disc-data verified: Cybil (SIBYL/SBL), Kaufmann (KAU) and Dahlia (DARIA/DA) are
the same 18-bone skeleton class as Harry — identical parent chain, kfSize 156,
part-name→bone scheme — so they drive directly (proportion stretch at
neck/ankles is expected and cosmetic). Lisa (LISA/LS) is 21-bone: her 3 hair
bones are posed as rigid head-followers from LS.ANM's bind chain into the 5
spare coords after `playerBoneCoords[18]`, with a per-frame `flg` clear
(`Pc_PlayAs_PlayerAnimTick`) because `Vw_CoordHierarchyMatrixCompute` caches by
`flg` and no ANM ever touches those bones.

**The four hazards the naive retarget hits, and their fixes:**
1. **Held-item buffer stomp.** Disc reads are sector-granular: every humanoid
   CHARA ILM read writes 16 KiB, and `HELD_ITEM_LM_BUFFER` sits at
   `HARRY_LM_BUFFER + 14592` (HERO.ILM's exact size). Any swapped read through
   the PSX slab would overwrite the equipped weapon's PLM (and a later weapon
   load would stomp the body's tail). Fix: `Pc_PlayAs_PlayerLmRedirect` (hooked
   after `Pc_BigLm_Redirect` in `WorldGfx_HarryCharaLoad`) hands out a PC-owned
   buffer for any non-HERO player ILM, registered via
   `Pc_BigLm_RegisterExternal(Chara_Harry, …)` so loose byte-replace capacity
   lifts still apply; loose/v7 replacements of the target character
   (already-owned pointers) pass through, so a CJ-style `LISA.ILM` mod follows
   the player. Mid-game swaps also free the held item first and re-equip after
   (`WorldGfx_PlayerPrevHeldItem` + `Gfx_PlayerHeldItemAttach`).
2. **HERO-textured weapons.** Knife/hammer/axe/handgun/rifle/shotgun PLMs carry
   a material literally named `HERO` and sample Harry's VRAM parcel (tpage 27,
   CLUT 736/480) — which now holds the skin's sheet. Fix: HERO.TIM is
   registered once in virtual texture slot `256 + Chara_Harry` (the pool uses
   256+id for 2..43 only, so 257 is free) and `WorldGfx_HeldItemDraw` bakes
   those PLMs against that desc (patched at the lazy bake, so it holds on
   every load order).
3. **Embedded prop meshes.** The targets' ILMs carry in-hand props that the
   show-all after skeleton build would render permanently: Cybil `06LGUN/
   10RGUN`, Kaufmann `06LHAND2/06LBAG/10RHAND2/10RGUN/10RAGLA`, Dahlia
   `10RHAND2/10RKEY/10FLAURO`. Fix: per-character hide tables applied by part
   NAME over the skeleton (slot index = ILM model index, name field on each
   `s_ModelHeader`) after every `WorldGfx_PlayerModelProcessLoad`.
4. **Harry's hand-variant toggles.** `func_8003DE60`'s lists are hard-coded
   HERO part-order indices; on a foreign ILM every equip/unequip hides random
   meshes. Fix: `WorldGfx_HeldItemAttach`'s Harry case re-applies the skin
   visibility table instead when a skin is active (self-healing full pass).

Also: the FPS-camera head hide extends to bones ≥ 18 so Lisa's hair doesn't
float in front of the eye. Swap-back to Harry restores the vanilla row/slab
(byte-identical path when `player_character = harry` and no swap ever ran).
Known cosmetic limits: proportion stretch at neck/shoulders/ankles (worst:
Kaufmann's ankles — his ILM has no shin↔foot weld), no per-weapon grip hand
variants on skins, demo attract mode plays with the active skin.

### Rainbow enemies map-wide after selecting Puppet Nurse — skin TIM overran the CLUT shelf (2026-08-07)

Selecting the Puppet Nurse turned **every enemy in the map** rainbow (and the
player figure with them), persisting until a map load. An out-of-bounds VRAM
write, and the oversized thing was the **pixel block**, not the palette.

`Chara_FsImageCalc` sizes the player's parcel for `HERO.TIM`: tPage 27 → VRAM
word origin `(704,256)`, and Harry's sheet is 64×**192** words, so it ends at row
447. Directly below sits the four-slot chara **CLUT shelf** — the monster branch
of the same function puts every enemy palette at `clutY = 464`,
`clutX = modelIdx*16 + 704`, i.e. `(704..767, 464..479)`. Harry stops 16 rows
short of it. That 16-row gap is the only slack in the page.

`Fs_QueuePostLoadTim` takes the destination x/y from the `s_FsImageDesc` but
**w/h verbatim from the TIM header** (`fsqueue_3.c:1206`/`:1252`) — the descriptor
carries no dimensions at all, and `fsqueue.h` documents "there are no size
checks". `PRS.TIM` is 64×**256** words, so the play-as upload covered
`(704..767, 256..511)` and swallowed the whole shelf, giving all four enemy
groups nurse *texels* as their palette. Nothing re-uploads them
(`WorldGfx_CharaModelLoad` early-returns on an unchanged charaId+lmHdr+desc), so
it stuck. The player's own body corrupts by the same overrun: pixels load
*before* the CLUT (`:1238` then `:1267`), so PRS's palette and the re-queued
held-item TIM land inside her own sheet's rows 224..255.

PRS's 48-row CLUT at `(736,480)` also runs 16 rows past the bottom of VRAM, but
that is **causally inert** — `GR_CopyVRAM` clamps rather than wraps
(`PsyX_render.cpp:4141`), and PRS's prims only ever address CLUT rows 0..13, so
the lost rows are never sampled. Fixing the palette alone would have changed
nothing visible.

**Fix**: `Pc_PlayAs_PlayerImageDesc` (called from the existing `SH_PC_PORT` block
in `WorldGfx_HarryCharaLoad`) hands any non-HERO player texture a **virtual pool
slot** instead of the VRAM parcel — `clutY >= HIRES_POOL_CLUT_ROW_BASE` makes
`Fs_QueuePostLoadTim` skip *both* `LoadImage` calls and decode the TIM straight
into per-CLUT-row GL textures. No VRAM byte is written, so no size can overrun
anything, and the fix is geometry-agnostic: `MOTH`/`BOS`/`BIG` (also 256 rows)
can join the roster with no further work. One interception point suffices because
the same local `image` feeds both the queued read and `harryModel.texture`, whose
material bake (`Lm_MaterialFileIdxApply`) is purely desc-driven. Keyed on
`textureFileIdx == FILE_CHARA_HERO_TIM` so stock Harry is byte-identical.

Slot id is a dedicated `HIRES_POOL_PLAYAS_SLOT 300`, not `256 + skinCharaId`: the
global chara pool resets and re-reads that slot when it spawns the chara you are
wearing, which would blank the player body. 300 sits just past the chara range
(`Chara_Count` 44 → 256..299) and its row spills (364, 428) fall between the
chara slots' own spill sets (320..363, 384..427). Chunk slots below 256 are
hard-capped at 16 rows, so nothing spills up into it.

Free side effects: non-HERO held items (`PIPE`/`CSAW`/`KATANA`) stop being
stomped at `(752,480)`/`(736,498)`, and the enemy shelf now survives a mid-map
swap. **Failure signature if the slot ever fails to register**: the player body
goes *invisible* rather than mis-coloured (prims with a virtual clut word are
dropped by `ShouldDropForClut`) — check the log for `[POOLTEX] slot 300` and
`[CLUTDROP]`.

**Latent stock hazard, deliberately not touched**: a real 48-row PRS monster in
chara group slot 2 or 3 (`clutX` 736/752, rows 464..511) would stomp Harry's own
CLUT home and the held-item CLUT in the *unmodified* game. Shipped map data
always pairs the nurse/doctor in group slot 0 with a textureless
`Chara_DummyNurse`/`DummyDoctor`, so stock never reaches it — but the console
`SPAWN` command and the global chara pool can place charas freely.

## Bigger item TMDs — oversized loose UNQ close-ups AND per-map IT packs (2026-08-03, adapted from PR #88 by keylimesoda)

**Feature, PC-only.** `pc_port/src/pc_big_tmd.c` — the item-model twin of
`pc_big_lm.c`: a fileIdx-keyed registry that validates an oversized loose
`gamedata/load/ITEM/*.TMD`, hands out a calloc'd PC-owned buffer sized for
`GsMapModelingData`'s worst-case read extent (the stub bounds a prim at 64 B
when computing its copy, over-reading past EOF for every real TMD), and
substitutes it at the `Fs_QueueStartRead` seams via a scoped function-like
macro. Consumers resolve the substitution with `Pc_BigTmd_Resolve`; a stock
item after a modded one CLEARS the active record (shared-slab stale-pointer
hazard). Identity in every failure path and with `allow_loose_files` off.

Adaptations beyond the PR: (1) coverage extended from the UNQ close-ups
(`FS_BUFFER_5`) to the per-map inventory packs IT_000..006 (`FS_BUFFER_8`,
`GameFs_MapItemsTextureLoad` + six consumer sites); (2) `BigTmd_Validate` now
also checks every packet's SHAPE — flag must be no-light (bit1 double-sided
allowed) and (mode, ilen) must be one of SH1's shipped shapes (0x30/4,
0x34/6, 0x36/6, 0x38/5, 0x3C/8) — because the port's `GsSortObject4J`
handlers stride by their own struct size, and a lit-flagged or standard-layout
PSX export mis-strides inside a batch and reads wild vertices (the crash class
SHModelViewer's flag preprocessor works around); (3) `GsMapModelingData` now
sizes `data_copy` for the furthest resolved pointer, fixing a latent heap
over-read on gapped TMDs the PR documented but did not fix; (4) the unit test
(first in the repo, `pc_port/tests/`, behind `-DBUILD_TESTING=ON`) is ported
to Windows (`_mkdir` shim) and grown a lit-packet rejection case — 13/13 pass.
map5_s01's FOOK.TMD (`FS_BUFFER_21`, map-DLL code) is deliberately not lifted.

Modded TMDs must reuse the stock VRAM tpage/CLUT words (15/14/13 per the fixed
`s_FsImageDesc` upload rects) — there is no per-mod texture upload on this path.

## Model Viewer 2.0 — props, TMDs, ANM animation playback, editable ANM JSON (2026-08-03)

**Launcher feature.** The viewer now opens `.ILM` (characters), `.PLM` (props/
weapons), `.TMD` (PSX item models) and `.OBJ`, and PLAYS `.ANM` animation on
character models with a play/scrub timeline (1x = the game's native 30 kf/s).

- **Scenes** come from `IlmObjConverter.BuildAnimScene` — the converter's own
  parser + scratch-pool weld replay, so welded joints stay closed in motion
  exactly as in game. Props pose at identity (correct — their parts bind the
  never-animated bone 0), with no spurious "NO REST POSE" warning.
- **Pose math** is `AnmFile.cs`, a complete public ANM implementation verified
  byte-level: engine-exact sampling (component-lerped q12 matrices, shared
  translation slot 0, rootYOffset, arithmetic floor shifts) and a lossless
  editable JSON round-trip — 55/55 retail ANMs re-emit byte-identical over
  their `fileSize` extent (tail pads `00 77 88` preserved verbatim). The
  viewer's Animation menu exports/imports these (`ANM → JSON`, `JSON → ANM`);
  a loose re-import goes in `gamedata/load/ANIM/` under the original name.
- **TMDs** parse via `TmdFile.cs` (verified over all 90 shipped TMDs; packet
  histogram exact) and texture through the stock VRAM mapping (tpage 15 →
  TIM00, 13 → TIM07, 14 → TIM01..06 per-map — ambiguous from the TMD alone,
  defaulting to TIM01; 5 → FOOK) into a per-(tpage, CLUT-row) page atlas.
  The 0x30/0x38 untextured payload layout was confirmed empirically:
  `{r,g,b,code}` then (norm, vert) u16 pairs.
- **Weapon PLM texturing** resolves the material NAME through the clut index
  (KNIFE/SHOTGUN etc. are textured from "HERO" — a sibling probe alone finds
  nothing, the index-based ClutComposer path finds CHARA/HERO.TIM).
- **Single-source converter UI**: the Mod Manager's Model → OBJ / OBJ → Model /
  View Model handlers moved to `ConverterActions.cs`; the Mod Manager buttons
  and the viewer's Convert menu are two entry points over one implementation.
- Verified headless (scratchpad harness renders): HERO/LISA posed by their real
  ANMs, SIBYL at an interpolated keyframe, KNIFE.PLM textured, IT_000.TMD
  textured — all visually correct.

Not in this pass: Harry's weapon/map continuation banks (HB_WEP*/HB_M*) in the
timeline (needs the three-buffer concatenation under HB_BASE's header), named
clip tables (anim-info rows live in code/overlays, not the ANM), and v7
high-poly ILMs in the viewer (no v7 reader in the converter's parse path).

## v7 high-poly welds — the root fix for joint seams on replacement characters (2026-08-03)

**The diagnosis.** Stock v6 models keep joints closed through the shared
256-slot scratch pool: a later-drawn part's prim corner reads the slot an
earlier part already wrote, so the corner follows the earlier part's bone in
every pose. The v7 wide path removed the pool (that is its whole point — full
authored density) and with it the ONLY weld mechanism the format had: v7 parts
were rigid, self-contained meshes, so butt-joined replacements (the user's CJ)
rendered perfectly at rest and opened at every joint the moment a bone bent.
Not a rigging error, not a converter bug — a structural gap, now closed.

**The format.** An OPTIONAL weld section appended inside the wide blob after
the last part record: `'WELD'`, `count:u32`, then 16-byte entries
`{readerOrdinal, readerLocal, ownerOrdinal, ownerLocal}` (mesh-0 vertex
indices; the owner must draw earlier in modelOrder). Older engines stop
reading after the last part and ignore it; older files simply lack it —
compatible both ways with no version bump.

**The engine** (`pc_wide_lm.c` / `pc_wide_lm_draw.c`): the parser validates
the section all-or-nothing (a malformed table fails the whole parse into the
invisible-spine fallback) and distributes entries to reader parts + deduped
owned-slot lists to owners. The drawer retains each owner part's transformed
mesh-0 screen coords for the duration of the character's draw pass (pass
boundaries detected by a modelOrder-rank reset or a different parts array —
two NPCs sharing one chara model still weld correctly), then overwrites each
reader's welded slot with the owner's retained screen vertex before emit —
stock weld semantics exactly. PGXP shadows travel with the value
(`Shadow_Copy`), so welded corners keep the owner's precision. An owner hidden
this frame leaves a stale stamp and its readers fall back to their own
coincident copies (unwelded but sane). `lm_validate.c` gains rule V18 so a
corrupt weld table is refused loudly at accept time.

**The converters** (`ilm_obj.py` `--v7` + launcher `V7Import`, byte-parity):
coincident cross-part vertex pairs (the exact criterion the `--replace` weld
pass uses, eps 0.01) are emitted as weld entries — owner = earliest-drawn
part, ties broken on the lower local index so both emitters stay
byte-identical. Python's `lm_validate_v7` gains the V18 twin. Identity-rest-
pose exports still cannot weld (coincidence is meaningless there) and say so.

**Verified**: HERO exported and rebuilt `--v7` emits 107 welds — exactly the
107 vertices `--replace` welds on the same model; a simulated engine parse
accepts all 107 (owners: chest/head/shoulders/arms/hips/shins — the joint
anatomy); Python and C# outputs byte-identical; engine + launcher builds
clean. In-game verification needs a rebuilt v7 model (re-run the high-poly
import on the CJ source — the new file welds automatically).

## EUR tree billboards + item-prop "rainbow mush" — one VRAM stomp (2026-08-03, commit `e96512123`)

**Symptom (user, build `dacbe0b7c`, pure vanilla, no texture pack):** "past
floatstinger the leaves get bugged and when entering sewers the items turn
into rainbow mush, but not all of them — items you pick up in nowhere are
fine". Trees render as opaque green/black squares (screenshot: map7_s01
exterior); item pickups show colour corruption. "The textures don't get more
messed up with time per se, but at certain breakpoints they break."

**Root cause — ONE stomp, two symptoms.** `map4_s03`'s TV bank
(`func_800D7450` case 1) uploads `TV2.TIM` to VRAM `(800,0 64x256)` using US
coordinates. On EUR that rect lands on the right half of `BG_ETC.TIM`
(`(768,0 64x128)`), because PAL reslices BG_ETC from US 128x256 to 256x128 and
moves both consumers into the stomped columns:

- **Tree/branch billboards** — `font_region.c` shifts `D_800AE4DC[]` UVs by
  +128 texels (u 128..191, v 0..63). BG_ETC is 4bpp, so at tpage 12 (origin
  VRAM 768,0) that is VRAM cells **(800..815, 0..63)** — inside TV2's rect.
  Garbage texels kill the 1-bit transparency key, so the billboard quad draws
  fully opaque = the reported squares.
- **World item props** — `func_8003BED0` binds BG_ITEM.PLM's `"BG_ETC"`
  material at `IMAGE_ETC` u=32,v=64. `s_FsImageDesc.u/v` are VRAM **cells**,
  not texels (log proof: TV2's own `img.u=32` + tpage 12 → `(800,0)`), so that
  band is **(800, 64)** — also inside TV2's rect. This is why only *some*
  pickups corrupt: props on the sibling `"TIM00"` material (drinks, ammo,
  aid kits — CLUT (928,480), page (960,0)) are untouched, and Nowhere's key
  items come from IT_005/IT_006 + TIM05/TIM06, not the BG_ETC prop material.

`BG_ETC.TIM` loaded **exactly once in the whole 85k-line session** (boot,
L92), so the damage was permanent from the mall/hospital TV room onward —
matching "breakpoints", not gradual decay. Log timeline confirms it: TV2
stomp L23510 (map4_s03) → first outdoor area after Floatstinger L33247
(map2_s02, bugged leaves) → sewers L37159 (bugged item props).

**Fix:** re-queue `GameFs_BgEtcGfxLoad()` per map load in `GameBoot_MapLoad`,
EUR-gated, next to the existing FONT16 insurance re-queue that exists for the
same tpage-12 collision. Deliberately *not* also re-queued at the stomp site
in `map4_s03.c` (as FONT16 is): BG_ETC would repaint (800..831, 0..127) over
half the TV texture, and map4_s03 is indoors with no billboards — the next map
load repairs it anyway.

**Ruled out with evidence** (full-session VRAM overlap replay of all 2040 TIM
uploads): the item pixel pages (`896,0` key items / `960,0` TIM00 / `864,0`
TIM07) and the whole item CLUT block `(896..943, 480..495)` are **never**
stomped mid-session — so this is not an item-texture corruption. Chara-pool
row-spill aliasing is also clean (slots used are 0..133, 192..211, 258..299;
the reserved 320..447 spill range is never allocated). TV2 is the *only*
writer into BG_ETC's home all session.

**Secondary findings, not fixed (lower confidence / niche):** `BOTL.TIM`
(L66947, Nowhere bottle scene) overwrites `KATANA.TIM`'s CLUT row 498 with no
reload — same class, but katana is a 2nd-playthrough bonus weapon. ~35
`[CLUTDROP] clutY_oob` prims in map7_s01 carry 0xFF-pattern clut/tpage words
(uninitialised-prim signature); PsyX drops them safely, so the visible cost is
a few missing polys.

## Texture-pack stutter + VRAM: the compose throttle was being bypassed (2026-08-04, commits `1ca0c2eeb`, `1f0c5e4b0`)

**Report** (issue #91): severe stuttering while walking with a pack installed, even on
an NVMe PCIe-4 SSD, and VRAM climbing to 8-10 GB — "probably the reason for a lot of
crashes related to texture packs in GPUs with less than 12 GB".

**Stutter root.** `s_texpackComposeBudget = FSQ_TEXPACK_COMPOSE_BUDGET` (2) is refilled
at the top of **every** `Fs_QueueUpdatePostLoad` (fsqueue_3.c), and `Gfx_InGameDraw`
runs two passes of up to 500 `Fs_QueueUpdate()` calls **synchronously inside one frame**
(world_draw.c:462-476). The per-tick throttle added in July was therefore bypassed ~1000
times per frame, and a cell crossing could run hundreds of ~7.6 ms composes before the
frame ended — worst measured shape ~1.36 s in a single frame. **Any future per-tick
budget inside the FS queue has this same hole.**

**VRAM root.** Not a leak — the configured default. `texpack_budget_mb` is 6144, the cap
is a hard stop with no eviction, and a heavy interior genuinely wants more, so the
process climbs to 6 GB of pack art and pins there; uncharged native pool rows and driver
overhead make up the rest of the reported 8-10 GB. Compounding it, every CLUT row was
composed eagerly — a measured mean of **9.23 rows per slot** when only 2-3 are drawn.

**Fix.** `pc_port/src/texpack_lazy.c`: the post-load retains the TIM's pixel and palette
blocks (they point into `entry->externalData`, which the next queue read recycles, so
both must be copied — ~15.3 KiB/slot, ~4.5 MiB at 300 slots, freed per slot as soon as
every row it ships has resolved). `HiresOverride_LookupByTpageClut` marks (slot,row)
wanted with an O(1) bit test per textured prim; `TexPackLazy_Pump()` composes out of a
wall-clock budget (`texpack_lazy_ms`) **after `PsyX_EndScene`** — after the OT submit,
which is what makes GL object churn safe there, since `ApplyHiresOverride` bakes GL
names into prims mid-frame. Plus `HiresOverride_ClampBudgetToVram()`: 45% of the GPU's
reported VRAM (NVX_gpu_memory_info → ATI_meminfo → no-op), only ever lowering, like the
system-RAM clamp in main_pc.c.

**Landing safety.** The retain sits in the same `if (!registered)` block as
`HiresOverride_PoolSlotRegister(disc TIM)`, which uploads every shipped CLUT row at
native resolution — so an uncomposed row draws correct PSX-resolution art and the
upgrade is in place. CAVEAT, stated honestly: if the base registration or the pump's
upload FAILS, the lookup's `useRow = glTexture[row] ? row : 0` serves **row 0's art**
(wrong palette on a multi-CLUT slot), not native — virtual pool slots (clutY >= 512)
never fall back to raw VRAM because `ClutHasNoPalette` treats clutY > 511 as unpalettable.

**Scope.** Pool-slot path only. The VRAM-rect path (fonts, HUD, 2D backgrounds — and the
*largest* canvases, 3072x960 ≈ 15 MiB) keeps the eager compose, because
`HiresOverride_SetForceNearestUpload` is set and cleared around that synchronous call and
deferring it re-opens the gutterless-glyph ghost text.

**Measured and rejected — do not re-propose without new numbers.** A content-keyed GL
texture cache is NOT a VRAM fix (byte-identical canvases within a source TIM: **0 of
8730**; `Texture_Get` matches by material name before claiming a slot, so two live chunk
slots can never hold the same sheet) — it is a stutter fix only. BC3/DXT5 is disqualified
by mipmaps: `glGenerateMipmap` raises GL_INVALID_OPERATION on compressed textures and
world rows require mips, so it means CPU box-downsampling and encoding *every* level
(+33% work) at ~40-130 ms/row against a 7.6 ms compose. BC7/.dds already only reaches
0.86% of a real pack: a sub-rect .dds cannot take the compressed whole-upload path.

**Also fixed here (pre-existing).** `HiresOverride_PoolSlotRegister` returned out of its
per-row loop on failure, skipping the stale-row drop — on a recycled slot that left rows
r+1..15 holding the PREVIOUS TIM's textures, so the slot rendered a mix of two sheets
depending on palette row, with the old charges still on the books. The four keyed
registrars did not credit the budget when an upload failed after the uploader had already
deleted the texture. New rect entries never initialised `packBytes`.

**Build gotcha.** `file(GLOB_RECURSE PC_PORT_SOURCES)` had no `CONFIGURE_DEPENDS`, so a
new file under `pc_port/src` was silently not compiled by a plain `cmake --build .`.

## Borderless cursor confinement (2026-08-04, commit `b32ef5ba4`, PsyCross `09aa645`)

Issue #87: on multi-monitor borderless the pointer walked off the game screen during any
mouse-driven moment and a click there tabbed the game out. Nothing in the tree ever
grabbed the mouse — `SDL_SetRelativeMouseMode` (control_style.c) confines it during
TPS/OTS mouse-look, which is why only the cursor-visible states were affected.
`PsyX_UpdateMouseConfinement()` calls `SDL_SetWindowMouseGrab` while fullscreen or
borderless. **Mouse grab only** — `SDL_SetWindowKeyboardGrab` is a separate call and is
never made, so Alt+Tab and the Windows key keep working, and SDL's Windows backend clips
only while focused and drops the clip on deactivation. Windowed mode is never confined.
Config `confine_cursor` (default 1).

## Quick save refused during a boss fight (2026-08-04)

A quick save inside a boss arena records the player mid-fight; reloading it can strand a
run that was entered with no health or ammo. The original game has no save point in a
boss chamber for the same reason. `Pc_QuickSave_BossActive` (pc_quicksave.c) keys on a
LIVE boss actor rather than a map/room table, so it covers every boss with no table to
maintain and lifts itself the moment the boss dies. Quick LOAD is deliberately untouched.

## Texture packs: VRAM budget derivation + GPU-side LRU + zip reader cache (2026-08-06, commit `8ff8c8ed7`)

**Report after the 2026-08-04 lazy-compose change:** "(1) stutters way less on
the SSD but still noticeable; on the HDD still very noticeable, maybe a little
improved. (2) Uses less VRAM, but stops loading textures after some point — it
used 6 GB of VRAM then stopped. 16 GB card (4070 Ti Super), more than 8 GB free."

**(2) was a one-line arithmetic consequence of shipping a constant.**
`texpack_budget_mb` defaults to 6144 and `HiresOverride_ClampBudgetToVram()`
only ever *lowered* it. A 16 GB card computes a 45% ceiling of 7372 MB; 6144 is
already below that, so the clamp did nothing and the run pinned at exactly the
shipped constant — the reported 6 GB, with 8 GB free. No constant can serve both
a 4 GB laptop and a 24 GB desktop. Now: with no user value the budget is
**derived** from the GPU and may be raised as well as lowered
(`texpackBudgetUserSet`); an explicit config value is still only lowered; and
main_pc.c's system-RAM clamp publishes `texpackBudgetCeilingMb` that the
derivation may not exceed, because on a shared-memory APU the GPU's reported
"VRAM" is the same RAM that clamp exists to protect.

**The general form of (2) was the absence of GPU-side eviction.** The budget was
a one-way ratchet: `PackBudgetExceeded` latched on and every later row kept
native art for the rest of the session, however far the player walked from
whatever filled it. The CPU-side compose cache has had LRU eviction since it was
written (`tp_cache_evict_lru`); the GPU side had none. Pool rows are now stamped
with a pump tick when **sampled** (`HiresOverride_LookupByTpageClut`), and under
pressure `HiresOverride_EvictColdestPackRow` frees the coldest and credits the
bytes back. The prior note that "LRU is near-worthless here because refCount
never reaches 0 under `resident_textures=1`" was wrong reasoning — eviction never
needed refCount; the lazy layer's per-prim want signal *is* the LRU key.

Two constraints the implementation turns on, both load-bearing:
- An evicted row must be **restored to native art**, never left empty. The
  lookup's `useRow = glTexture[row] ? row : 0` otherwise serves ROW 0's palette
  — a monster in another monster's colours. `texpack_lazy.c` therefore now keeps
  its retained TIM pixel+CLUT blocks for the life of the slot (it used to free
  them once every row resolved) and `HiresOverride_PoolSlotRestoreNativeRow`
  re-expands the row, uncharged against the budget.
- Only rows that *can* be restored are evictable, via
  `HiresRestorablePredicate` — the chara pool registers no lazy source, so its
  rows are never candidates.
Rows sampled within `LAZY_EVICT_MIN_AGE` (8) pumps are exempt, so when the hot
set alone fills the budget nothing is evictable and the pump degrades to native
art rather than thrashing evict/recompose.

**(1), partially.** `mz_zip_reader_init_file` opens the archive and parses its
**entire central directory**, and both entry loaders in `tex_pack.c` did that per
extracted file — N textures cost N opens and N full directory parses on top of
the N reads actually needed, and on an HDD every one of those is a seek. Readers
are now opened once and kept for the process (`Scan_Once` is one-shot; nothing
rescans). **This only helps zip packs** — a loose-folder pack still pays
fopen+fread+stbi-decode on the main thread. The
`[TEXPACK] indexed N replacement entries (M zip packs)` log line says which a
given reporter has.

**Still open — the real stutter fix.** Compose runs synchronously on the main
thread inside the pump's millisecond budget, and that budget cannot bound a
blocking disk read (one HDD seek is 10–50 ms, against a ~4 ms budget). The fix is
to run read + decode + composite on a worker and leave only `glTexImage2D` on the
main thread. Scope it against this blocker first: `TexPack_Compose` is
non-reentrant — `g_tpCache`, `g_tpLastHash/IsDds/Built`, `g_tpDdsBytes` and
`g_tpTransient` are file-scope globals and the `TexPack_LastCompose*` accessors
are inherently single-call. `g_entries` is read-only after `Scan_Once`, so the
match itself is already thread-safe.

**New log lines to check on the next report:**
`[TEXPACK] GL texture budget X -> Y MB (45% of Z MB VRAM, auto)` and
`[TEXPACK/LRU] evicted N cold row(s), M MB pack GL live`.

## Play-as skeleton retarget — the swapped characters get their OWN proportions (2026-08-07)

**Symptom.** Playing as Cybil stretched her neck; Kaufmann's shoulders bunched
into his collar and his shoes floated off his trouser legs; Lisa read slightly
off; Dahlia looked perfect.

**Cause.** A skin's mesh parts are authored in BONE-LOCAL space against THEIR
OWN skeleton's bind translations, but the swap posed them on Harry's. Measured
deltas vs HB_BASE: Cybil's head bind sits 20 units below Harry's (neck pulled
long), Kaufmann's 24 above (head/collar telescopes into the shoulders), and the
leg chains (hips→thigh→shin→foot) differ by 4 / 36 / 16 / 0 units for
Cybil / Kaufmann / Lisa / Dahlia — Dahlia's exact match is why she alone
looked right, and Kaufmann's 36 is his floating feet.

**Fix** (`pc_playas.c`): textbook retargeting — source rotations, target
offsets. Every bone whose `translationDataIdx` is −1 takes its local
translation from the ANM bind table once in `Anim_BoneInit` and is never
rewritten by `Anim_BoneUpdate` (which only writes translations for slot-0
bones 0/1/11), so the skin's own `translationInitial << scaleLog2` values are
written into those coords and stay, while Harry's keyframes keep supplying
every rotation. Re-asserted each frame from `Pc_PlayAs_PlayerAnimTick` — 17
vector writes, idempotent, and immune to any re-init path (map load, save load,
warm reset).

Foot height is corrected through the ANM header's `rootYOffset` byte, which the
engine already subtracts from every slot-0 sharer's Y each frame: patched to
`255 − (harryLegDrop − skinLegDrop)` (Cybil 251, Kaufmann 219, Lisa 239, Dahlia
255), so there is no per-frame accumulation to get wrong. Both the binds and
the byte are restored when swapping back to Harry.

The skin's bind table is read straight off its ANM through the FS queue (pumped,
not VSync-waited) at `Pc_PlayAs_OnPlayerModelLoaded` — after the filesystem
exists, unlike `Pc_PlayAs_Init`. Harry's own binds are read live out of
FS_BUFFER_0, so the compensation stays correct whatever HB_BASE holds.

**Verified** by rendering all four skins on HB_BASE offline before and after
(scratch harness reproducing the same byte patch): Cybil's neck normal,
Kaufmann's collar mass gone and his shoes attached, Lisa's proportions natural,
Dahlia unchanged. Numbers the harness derived independently match the engine's.

## TMD ↔ OBJ converters — item models are editable (2026-08-07, launcher 2026.8.7.1)

The Model Viewer gained .TMD support in 2.0, but "Export This Model → OBJ…" was
gated on `_anim != null && _anim.IlmPath != null` and a TMD scene carries no
`AnimScene` (it has no bones) — so the menu item was greyed out on exactly the
models the new parser could read. `TmdObjConverter.cs` supplies the missing
pair; the menu item now dispatches on the loaded extension.

**Every convention was MEASURED against all 90 shipped TMDs (40 703 prims), not
carried over from `IlmObjConverter` — and two of them differ:**

- **Normal orientation.** A TMD normal is ANTI-PARALLEL to
  `cross(v1-v0, v2-v0)` in 40 665 of 40 679 non-degenerate prims (100.0%; the 14
  exceptions are near-degenerate slivers). Reflecting Y to reach OBJ's Y-up space
  negates cross products, so exporting positions as `(x, -y, z)` and normals as
  `(nx, -ny, nz)` — the SAME transform, corner order untouched — leaves winding
  and vertex normals agreeing and pointing outward. This is NOT the ILM rule
  (`(-x, y, -z)`, because ILM normals are authored inward); using ILM's rule here
  lights every item from the inside. Independently confirmed: stored TMD normals
  point away from the object centroid in 306 of 308 objects.
- **Quad corner order DOES carry over**: a TMD quad is a triangle STRIP (tris
  0,1,2 and 1,3,2) against OBJ's perimeter LOOP, so corners emit `0,1,3,2` — an
  involution, so import reuses the one table.

**Import PATCHES IN PLACE**, like the ILM importer and for the same reason: it
preserves every non-geometry byte for free. That matters more than expected here
— 88 of 90 files carry trailing bytes after the last normal block, and in 80 of
those the tail is a literal copy of the file's own opening bytes (a CD-extraction
artifact). Topology is immutable; object/face/corner counts must match the
template, and mismatches fail with a located message rather than a silent
scramble. Vertex slots are resolved through FACE CORNERS, not by trusting the
OBJ's v-block order, because Blender renumbers freely.

Other verified layout facts: sections are laid out `[all prims][all verts][all
normals]`; offsets are relative to the object table at 0xC; vertex/normal entries
are 8 bytes with the 4th halfword always 0; `flag` and object `scale` are always
0; on a textured prim corner 0's word is the CLUT id, corner 1's the tpage id and
corners 2..3's are padding (always 0).

**Validation (all three, because the first two are individually blind):**
1. Round-trip over all 90 shipped TMDs: **90/90 byte-identical**, 0 errors.
2. Independent check on the exported OBJ — face winding vs its own `vn`:
   **40 665/40 679 = 99.97%** agree, **0** files majority-inverted. (A byte-exact
   round-trip cannot see a shared export/import assumption — that is exactly how
   the ILM converter's first ship was a visual disaster.)
3. **Rendered and looked at**: FOOK, IT_000, IT_003, UNQ20/30/40/50/60, UNQC1,
   UNQE1 — hook, bottles, keys and boxes, smoothly lit, no bowties, no
   inside-out faces.
4. Perturbation: shifting an OBJ's vertices +7X/−3Y patched through and
   re-exported at exactly the shifted values (96 verts, 91 normals, 564 UVs).

**UI.** The tool column was full, so "Model → OBJ" and "OBJ → Model" became
dropdowns (the "DDS ▾" pattern): character vs item model, and the OBJ → Model
menu now also surfaces the **simple** character import that was previously
reachable only from a button inside the high-poly dialog. The Model Viewer's
Convert menu gains "TMD → OBJ…" and "OBJ → TMD…". Export asks which of TIM01..06
to decode previews with (`TmdPageDialog`) — VRAM page 14 holds one per-map bank
at a time, so it is genuinely ambiguous from the TMD alone; the choice affects
only the preview PNGs, never the geometry.

**Known limits:** a bank file's objects all sit on the origin (a TMD stores no
placement) so they overlap in Blender — the dialog says so. Adding or removing
geometry is not supported; the engine's big-TMD path (`pc_big_tmd.c`) would allow
it, but a rewrite has to reproduce the section layout and the trailing bytes, and
modded TMDs must reuse the stock VRAM tpage/CLUT words either way.

## TMD replace (real geometry changes) + page-14 bank auto-detect (2026-08-07, launcher 2026.8.7.2)

Two follow-ups to the TMD converters.

### "OBJ → TMD — replace…": arbitrary geometry
`TmdObjConverter.Import` only moves existing vertices (counts must match), which
is not model *replacement*. `Rebuild` emits a whole new file, so vertex, normal
and face counts are free. The engine already supported this — `pc_big_tmd.c`
accepts **8192 vertices and 8192 primitives per object** (stock's largest object
is ~140 verts), 48 objects, 16 MB — so the limit was entirely converter-side.

The template is still required, because geometry cannot carry it: it supplies
which VRAM page and CLUT each material binds to. That is not a convenience — the
GAME decides what is uploaded to a texture page, so a modded TMD must reuse stock
tpage/CLUT words or it samples whatever happens to be resident. Materials named
the way Export writes them (`tpNN_clutNN` / `rgb_r_g_b`) carry the binding
directly; anything else falls back to the template's own binding with a warning.

**Object COUNT stays fixed**: a bank's object index IS the item identity
(`g_MapOverlayHdr.loadableItems` indexes it), so adding or removing objects would
silently repoint items at each other's models.

Only the five shapes `pc_big_tmd.c` accepts are emitted — it hard-switches on the
exact mode byte (`0x30/0x34/0x36/0x38/0x3C` at their SH1 ilens, flag bit 0
clear). Two consequences fall out of that and are handled explicitly:
- **`0x36` (textured tri) is the only semi-transparent shape that exists.** A
  semi-transparent quad is split into two triangles; semi on an untextured face
  cannot be represented and is dropped with a warning.
- n-gons are fan-triangulated (no n-gon packet exists).

### Page-14 bank is now derived, not asked
Export used to prompt for TIM01..06 on every model, and the viewer silently
defaulted to `TIM01.TIM`. Both were wrong more often than not:

- **30 of 90 shipped TMDs never sample page 14 at all** (they use 15/13/5 =
  TIM00/TIM07/FOOK, which are unambiguous). Those are never asked about now.
- **The `IT_00x` banks are deterministic.** `item_screens_3.c` loads the TMD bank
  (`GameFs_MapItemsTextureLoad`) and the page-14 TIM (`GameFs_MapItemsModelLoad`)
  from the SAME map-group switch, so the two tables are the same partition of the
  map list. That fixes the pairing exactly — **including the 001/002 swap**:
  `IT_001→TIM02`, `IT_002→TIM01`, `IT_003→TIM03`, `IT_004→TIM04`,
  `IT_005→TIM05`, `IT_006→TIM06`. `IT_000` is absent because its group
  (`MAP0_S00`) loads no `TIM01..06` — and the shipped data agrees: IT_000 is the
  only bank with no page-14 primitive. That coincidence is what confirms the
  table.

So the viewer was mis-palettting five of the six banks, and the dialog now
appears only for `UNQ*`/`ITEM_00`, where the bank genuinely depends on which map
the player is in. A CLUT-row-range test was tried first and **does not work** —
all six banks ship 16 rows at VRAM y=0, so rows cannot discriminate them.

### Validation
- Rebuild across all 90 shipped TMDs: **90/90** rebuilt, and re-exporting each
  reproduces the original's geometry **and its material runs** (order-independent
  triangle-corner comparison + usemtl run comparison).
- Patch-mode round-trip still **90/90 byte-identical** (unregressed).
- Growth: UNQ30 subdivided to **660 verts / 752 prims** (from 96 / 188) rebuilt
  clean, and was checked against a transcription of `BigTmd_Validate`'s rules
  taken from the C — **accepted**, as is stock UNQ30 by the same checker.
- Rendered and compared: the rebuilt high-poly bottle matches stock in silhouette
  and shading with visibly denser triangulation; a rebuilt IT_003 bank renders
  correctly.

The material-run check is what caught two real defects the geometry check was
blind to: `ObjFace.Mtl` was never assigned (every face fell back to the
template's default binding — fine for single-material objects, wrong for
anything else), and `_semi` was parsed off the material name but never re-applied,
so every semi-transparent prim rebuilt opaque.

**Still unverified: in game.** All of the above is offline.

## Launcher was clobbering config.cfg settings it does not own (2026-08-07, launcher 2026.8.7.3)

**Report:** "my config keeps setting resident_textures back to 0, and I think it
removed my 512mb texture line." Correctly diagnosed by the reporter as editing
config.cfg with the launcher open, then pressing Play.

`ConfigManager` read `_lines` and `_values` **once, in its constructor** — i.e. at
launcher startup — and `Save()` wrote that snapshot back wholesale. Two distinct
losses followed:

- `_values` holds EVERY key parsed from the file, not only the ones the launcher
  has UI for, and `Save()` rewrote all of them. `resident_textures` was read as
  `0` at startup, so Save put `0` back over a hand-edited `1`.
- A line added *after* startup was never in the stale `_lines`, so
  `File.WriteAllLines` simply dropped it. That deleted `texpack_budget_mb = 512`
  outright.

The launcher knows neither key — `grep` finds no reference to `resident_textures`,
`texpack_budget_mb`, `texpack_cache_mb` or `texpack_lazy_ms` anywhere in it. It
was resetting one and deleting the other purely as collateral. `Form1.cs:903`
already documented the intended contract ("Save() rewrites only keys it holds, so
a user's existing value is preserved untouched") — the code just never honoured
it.

This also silently fought the GAME: `PcConfig_SaveKeyValue` (pc_config.c) writes
`control_style`, `language`, `use_pgxp`, `post_process`, `tonemap` and the
flashlight keys at runtime, and any of those written while the launcher sat open
were reverted on the next Play.

**Fix:** `Set()` records the key in a `_dirty` set, and `Save()` re-reads the file
from disk immediately before writing and applies **only the dirty keys**. Every
other line passes through byte-for-byte, comments and ordering included; keys the
launcher never set are now genuinely untouched. `EnsureLauncherSection()` became a
flag applied after the re-read (it used to mutate the stale list, which the
re-read would discard). Line splitting moved to the first `=` only, so a value
containing `=` is no longer silently unparseable.

**Verified both directions** with a harness that replays the exact scenario
(launcher starts → user changes a launcher setting → file hand-edited to flip
`resident_textures` and add `texpack_budget_mb` → Save):
- pre-fix `ConfigManager` (from git): `resident_textures` reverted 1→0 and the
  `texpack_budget_mb` line was gone — the report reproduced exactly.
- post-fix: both hand edits survive and the launcher's own `fullscreen` change is
  applied.

## Launcher warns when resident_textures is off (2026-08-07, launcher 2026.8.7.4)

`resident_textures = 0` is a compatibility fallback, not a performance option,
and nothing told the user what it costs. Several test sessions ran with it off
without anyone noticing (helped by the config-clobber bug above), and the
resulting reports were hard to read because the setting silently changes three
different subsystems.

At Play, if the config carries `resident_textures = 0`, the launcher now explains
the trade and offers to turn it on:
- distant walls/floors losing textures or going garbled/rainbow (the vanilla
  8-full/2-half VRAM page pool steals pages from farther chunks)
- more stuttering while texture packs load (eager compose at TIM upload instead
  of the demand-driven, frame-budgeted pump)
- HD packs quietly stopping partway through a session (that path has no LRU
  eviction, so the pack VRAM budget latches once spent — see the eviction work
  above, which is scoped to the pool-slot path)

Three outcomes, none of which block launching: **Yes** sets it to 1 and saves,
**No** proceeds unchanged and asks again next launch, **Don't ask again** writes
`launcher_warn_resident_textures = 0` under the `## Launcher` section. It only
ever asks — the fallback is genuinely correct on some hardware, which is why it
exists, and the launcher must not overrule a deliberate choice. Esc / the X
button read as "No".

Runs AFTER `SaveConfig()`, so the value tested is the one the game is about to
load, including a hand edit made while the launcher was open — the launcher never
`Set()`s this key, so post-fix `ConfigManager.Get` reports what is really on disk.

Verified headlessly against the real `ConfigManager`: warns on `0`; "Yes" writes
`1` and leaves `texpack_budget_mb` and the rest intact, then stops warning;
"Don't ask again" writes the suppression key, leaves `resident_textures = 0`
untouched (choice respected), and the suppression sticks on reload.

## PsyCross PR triage — 5 open upstream PRs (2026-08-07, PsyCross `a228215`, `7a33ffe`, `05fc558`)

Three adapted, two closed as superseded.

**#9 `include/psx` shadowing POSIX `<strings.h>`** (`a228215`). `include/psx/strings.h`
is a guard-only stub that declares nothing, and `include/psx` was on
`psycross_static`'s PUBLIC include path — so it resolved ahead of the POSIX header
for every TU built against the archive. That, not the feature-test-macro ordering
the old `#ifdef __GNUC__` comment claimed, is why `strcasecmp` came up undeclared
on glibc. Dropped from PUBLIC and the workaround deleted. Safe because every
consumer that needs bare `<libgpu.h>`-style includes adds the directory itself
(exe `CMakeLists.txt:334`, map DLLs `maps/CMakeLists.txt:21`, big-TMD test, and
PsyCross's own audio tests). Took `#else` rather than the PR's
`#elif defined(__unix__)` — Apple defines `__APPLE__`/`__MACH__`, not `__unix__`.
Remaining: the host re-adds `include/psx`, so a few host TUs still get the stub;
renaming/deleting it would close that for good.

**#15 DualSense SDL hints** (`7a33ffe`). Taken as an explicit pin, NOT a fix:
`SDL_HIDAPI_DEFAULT` is unconditionally `SDL_TRUE` and the PS5 driver resolves its
hint against it in 2.0.14 / 2.0.20 / 2.28 alike, so both hints are already "1"
everywhere. The one non-obvious constraint (recorded in the comment) is ordering —
they must precede the `SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)` in the same
function. The real DualSense bug is elsewhere and still open:
`PsyX_Pad_Event_ControllerRemoved` compares a stored SDL *device index* against
`event.cdevice.which`, which is a joystick *instance ID* for
`SDL_CONTROLLERDEVICEREMOVED`, so disconnects often fail to free the slot.

**#12 VRAM decode via CPU-baked LUT** (`05fc558`). `samplePSX` now returns the two
raw VRAM bytes; `lut()` indexes a 256x256 table (`GR_InitRG8LUT`, texture unit 2)
built once with integer bit math. Two corrections were required against the PR as
written, both mandatory to keep the image where it is:

- the PR sampled the table's `v<<3` bytes straight out, a uniform 256/255 gain.
  Not sub-visible: `GPU_LIT_TAIL` multiplies by `v_color` (`bright = 2` on textured
  prims) and the dither offset is added *before* the 5-bit quantize, so the gain
  crosses a band boundary and moves dithered pixels a full 8/255. Recovering the
  integer byte and scaling by 1/256 reproduces `v/32` exactly.
- the PR applied `1.0 - w` AFTER the bilinear mix. Table alpha is 0 for both an
  opaque texel and a colour-0 texel, so holes read opaque and every 4/8-bit cutout
  edge gained a dark fringe. Alpha is now resolved per texel inside `lut()`,
  keeping `decodeRG`'s colour-0 case, landing on exactly 0.0 / 0.5 / 1.0
  (`BM_AVERAGE` feeds it to `GL_SRC_ALPHA`).

**Root cause is unproven for this tree** — worth remembering before chasing it
again. The byte-snap in `GPU_FETCH_VRAM_FUNC` is present on BOTH `VRAM_FORMAT`
branches, and with a snapped input every `floor()` in the old `decodeRG` had
>= 0.128 of fp32 headroom (worst case C=32767 -> 32767.129/32768), so it was
already provably exact on any IEEE-754 GPU including Mesa. If the Steam Deck tint
was real, the likelier culprit is the CLUT *index* arithmetic bundled into the same
hunks (`c_PackRange` -> `255.0`, `floor(v)` -> `floor(v + 0.001)`), which was kept.

**#5 hi-res override hook** — closed, already implemented as `7ffe8b9` ("Port of
PR #5 onto current master"), and since extended (8-param lookup feeding
`u_hiresHalf`, DR_PSYX_TEX restore on miss, `0bc5fca`'s floor/clamp tc replacing
the PR's `+ texelSize * 0.5`, which bled into the neighbouring atlas cell).

**#6 SPU rung-out sustain / loop stacking** — closed. Fix 1 is superseded by
`251091d` (4-state `SpuGetKeyStatus`): a rung-out decreasing sustain reports
`SPU_ON_ENV_OFF`, which is what `SdAutoKeyOffCheck` needs to key off AND run the
bookkeeping that clears the SFX-to-voice table; forcing `ENV_OFF` reports `SPU_OFF`
and skips both. Fix 2's hunk no longer applies (`90df050` removed the
`voice->looping` gate) and is inert now that ADSR is engaged for any voice that
programmed one. The genuinely open gap is neither: an orphaned looping voice with
a NON-decreasing sustain never reaches level 0, stays `SPU_ON` forever, and is
invisible to both `SdAutoKeyOffCheck` and `voice_check`.

### Verification harness (reusable)

GLSL only compiles at runtime, so a clean C++ build proves nothing about shader
edits. Two throwaway gates in the scratchpad caught/settled this one:

1. **Shader compile check** — `g++ -E` on `PsyX_render.cpp`, strip cpp linemarkers
   (`^# \d+ "..."` — otherwise the filename string gets scooped into the GLSL),
   pull the `gte_shader_*` literals, prepend the same `GLSL_HEADER_*` +
   `AFFINE_VARYING`/`BILINEAR_FILTER` defines `GR_Shader_Compile` uses, and compile
   all variants in a hidden SDL GL window. All 8 linked; `gte_shader_32_rgba`
   reports `s_rgLut = -1`, confirming the sampler assignment is a no-op there.
2. **Decode equivalence** — one fragment per (highByte, lowByte), running the old
   `decodeRG(packRG(rg))` and the new `lut(rg)` side by side and flagging any
   disagreement. 65536 values, 0 rgb and 0 alpha mismatches on the real driver.
