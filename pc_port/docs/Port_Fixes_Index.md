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
