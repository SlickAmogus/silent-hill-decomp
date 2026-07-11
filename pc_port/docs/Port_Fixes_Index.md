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
- **PGXP near-camera warp**: root-caused; fix = GL near-plane clipping, design
  in `PGXP_NearClip_Design.md` (not yet implemented).

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
- `.rar` packs supported too (official unrar library vendored in
  `pc_port/third_party/unrar`, RAR4+RAR5+solid): extracted ONCE to
  `<name>.rar.extracted/` beside the archive (only *.png + config.yaml;
  `.done` marker skips re-extraction), then indexed like a loose folder.
  The standard DuckStation layout — `<GAMEID>/replacements/...` with a
  `config.yaml` — works as-is, extracted or archived at any nesting depth
  (scan is recursive; names are matched by basename). `texpage-*` entries
  (page-mode dumps) are unsupported and counted in the log.

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
  that opens `ModManagerForm`. Manages a self-owned `mods/` library beside the
  game exe; deploys ENABLED mods into the game's additive override dirs and
  undeploys via a `mods/deployed.txt` manifest (nothing touches original game
  data — deploy = copy, undeploy = delete tracked files).
- Three auto-detected types: DuckStation texture packs (`texupload-*`/`config.yaml`)
  → `gamedata/texturemods/<mod>/` + `loadorder.txt`; load-folder mods (a `load/`
  or disc-structured tree) → `gamedata/load/` (merge; forces `allow_loose_files=1`);
  FMV mods (`.avi`) → flattened into `gamedata/FMV/`. List order = load order (top
  = highest priority): texture packs ranked via `loadorder.txt`, load/FMV copied
  highest-last so it overwrites. Logic in `ModManager.cs`, UI in `ModManagerForm.cs`.
- Archives: `.zip` dropped in `mods/` auto-extracts once; `.rar` is kept as-is
  and (as a texture pack) deployed into its `texturemods/<mod>/` subfolder for the
  GAME's own `.rar` extractor to unpack on launch (no unrar in the launcher —
  reuses the vendored one linked into the game). Drag-and-drop onto the window
  imports folders/`.zip`/`.rar` into the library; right-click a mod for a friendly
  display name + notes (stored in `mods/modmanager.json`, DataContract JSON; the
  folder name stays the deploy identity). The Form1 button swaps to
  `manager_clicked.png` while pressed.

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
