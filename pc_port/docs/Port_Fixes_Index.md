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
- [1. NULL-pointer guards (PSX had no memory protection)](#1-null-pointer-guards-psx-had-no-memory-protection)
- [2. Zero-stubbed data tables → real tables](#2-zero-stubbed-data-tables--real-tables)
- [3. IPD chunk streaming & buffer sizing](#3-ipd-chunk-streaming--buffer-sizing)
- [4. Fixed-point overflow](#4-fixed-point-overflow)
- [5. 64-bit pointer width & struct layout](#5-64-bit-pointer-width--struct-layout)

---

## 1. NULL-pointer guards (PSX had no memory protection)

On PSX a NULL (or small-integer) pointer dereference reads valid low RAM and
usually does harmless garbage work; on PC the same read access-violates. These
guards skip the work the pointer can't do, matching the PSX outcome.

- **`Anim_BoneInit` — cat locker scene-end crash.** The cat scene's end cleanup
  (`func_800D87C0` → `Chara_BonesInit(0)`) passes `g_CharaModelAnimsData[1].activeAnmHdr`,
  which is NULL for that unloaded slot; reading `anmHdr->boneCount` (offset 6) AV'd.
  WinDbg-confirmed. Guard skips the bone loop after the root coord is initialised.
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
- General guidance on finding these lives in
  [`struct_offset_portability.md`](https://github.com/SlickAmogus/silent-hill-decomp/blob/pc-port/pc_port/docs/struct_offset_portability.md).
