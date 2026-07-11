# Whole-Map Far Projection — Task Spec (dedicated session)

Status: **READY TO START — begin with the STEP-0 crash.** Prepared 2026-07-11,
updated same day after the first live engagement.
Read alongside memories `[[project_interior_room_islands]]` (whole-map draw-path
history + the four lifted gates), `[[project_pgxp_implementation]]` (float GTE
infra), and `pc_port/docs/PGXP_NearClip_Design.md` (prior art for a gated
projection change).

## Problem

`whole_map_exteriors = 1` now loads, textures, and SUBMITS every exterior chunk
(all cull gates lifted 2026-07-10/11, commits `286157766`, `94b60f11c`), but the
visible world still ends ~6 cells out. The wall is the PSX GTE pipeline itself,
not a cull:

- GTE `RTPT` computes screen X/Y as `pos * h / SZ3`, and **SZ3 saturates at
  0xFFFF Q8 = 256 world units** of view depth (PsyX_GTE.cpp `Lm_D`). Beyond
  that, projection output freezes (geometry renders as if pinned at 256u —
  wrong scale/parallax) or degenerates.
- The game stores per-vertex depths in **s16 scratch** (`field_18C`); beyond
  128u they wrap negative. `SH_WHOLEMAP_DEPTH_RESCUE` (bodyprog_80055028.c)
  keeps those polys alive by bucketing them into the last OT slot, and
  `PsyX_SetNextPrimSz` reads the same slots as u16 for GL depth — but that only
  fixes ORDERING, not the frozen projection.
- Practical result: correct render to ~128u, increasingly wrong 128–256u,
  frozen/garbage beyond 256u. A town map spans 600u+.

The user confirmed on-foot visibility matches this analysis. A `[WHOLEMAP]`
probe (Ipd_ChunkCheckDraw, logs `total/loaded/drawn` every 2s while the mode is
active) is in the build — **check the user's latest log first**: `drawn` ≈
`total` (~256) confirms the GTE limit is the only remaining wall; `drawn <<
total` means a residual chunk gate escaped the audit (find it before starting
the big work).

## Goal

While `Pc_WholeMapDrawActive()` (declared in pc_config.h): exterior world
geometry projects correctly to arbitrary distance. Mode off = byte-identical
PSX path (the hard invariant of every PC feature).

## Submit chain (audited clean, for orientation)

`Ipd_ChunkCheckDraw` → `Ipd_ChunkDraw` draw-all branch (bodyprog_80040B74.c)
→ `func_80057090` → `func_80057344` (no culls) → per-mesh vertex transform
(`func_800574D4`/`func_8005759C` copy verts; `func_80057658`/`func_80057A3C`
GTE-transform 3 at a time into `screenXy_0` + s16 `field_18C`; `func_80057B7C`)
→ `Gfx_MeshDraw` (+ 4 sibling prim loops) emits POLY_* with those coords.

## Candidate approaches (pick after the probe verdict; A is the leading one)

**A. Float far-transform behind the PGXP delivery channel (leading).** For
chunks whose view depth can exceed ~100u in whole-map mode, transform the mesh
vertices on the C side in float/double (view matrix × vert, then the same
`h/z` projection unclamped) and deliver the precise screen coords + depth to GL
the way PGXP already does (shadow-mem vertex substitution at draw; see
`project_pgxp_implementation.md` — runtime-gated, per-vertex float positions
keyed by scratch address). The PSX prim still carries its saturated coords
(harmless — GL replaces them). Reuses proven infra; near geometry keeps the
authentic PSX wobble. Risks: shadow-mem is keyed for the PGXP flow — check the
key scheme tolerates the world-geometry scratch reuse pattern; per-vertex fog
bytes (0..127) must be recomputed from the float depth or they'll saturate the
same way.

**B. Dedicated far-chunk GL renderer.** Draw chunks beyond ~100u through a
small modern path (float MVP, the chunk's own verts/UVs, hires-override
textures) and skip them in the PSX path; near chunks unchanged. Precedent: the
decal renderer (pc_port/src/pc_decals.c) already draws arbitrary textured
world quads through the override texture path. Clean separation, but needs
material/UV → texture-page resolution duplicated (Material_FsImageApply
knowledge) and a seam strategy at the near/far boundary.

**C. Widen the GTE numeric range in whole-map mode.** PsyX_GTE.cpp is ours —
skip the SZ3 u16 clamp behind the mode flag. Rejected as primary: the game
stores depths in s16 either way, and dozens of game-side sites assume PSX
ranges (the div-zero sweep class). Touching GTE semantics leaks far beyond
world geometry.

## Constraints / interactions

- Fog: per-vertex fog factors are game-computed bytes (`PC_FACE_FOG_VERTS`,
  fogRamp LUT) — far geometry needs sane fog (or fogstr-0 testing first).
- OT vs GL depth: far polys currently pile into the last OT bucket; with float
  depth via `PsyX_SetNextPrimSzExact`-style delivery, GL depth ordering is
  exact regardless of bucket.
- Perf: whole-map already submits everything with no frustum cull; a far path
  should add per-chunk frustum rejection (cheap AABB vs view) or it will be
  slow. `disable_culling=1` is the shipped default — do not key anything off it.
- The hires-override per-prim texture path must keep working for far geometry
  if approach B is chosen (texture packs on distant buildings).

## Testing

map0_s00 / map2_s00 street, `whole_map_exteriors=1`, `preload_chunks=1`,
`resident_textures=1`, `fogstr 0`: whole town visible, correct scale/parallax
while walking, no seam artifacts at the near/far boundary, playable FPS.
Flycam (with its chunk cap already lifted) for aerial verification. Regression:
mode OFF must stay byte-identical (spot-check hash/screenshots of normal play);
interiors unaffected (mode is exterior+street-room gated).

## STEP 0 (added 2026-07-11): Levin-house load crash + gate correctness

History of the activation gate — both attempts so far are WRONG:
1. `mapRoomIdx == 0` — never engaged anywhere: street room numbering is
   per-map (map2_s00 street = 0, map0_s00 street = 37). The mode was dead in
   all early "still can't see the town" reports; those tests tested nothing.
2. `g_WorldEnvWork.isFogEnabled` (`fd359cb05`) — ENGAGED on the street (first
   real activation), but **loading into the Levin Street house crashed the
   game immediately**. Either the house keeps a bit of outdoor fog enabled
   (gate stays true inside a hosted interior = the original street-through-
   house disaster, now with far caps lifted), and/or the gate flips mid room
   transition while chunks/materials are in flight.

Triage first: [CRASH] backtrace in the user's newest SilentHill_*.log, or ask
for a WinDbg `!analyze -v`. Suspects: texture-all mass-claim during the
transition window (Ipd_ChunkMaterialsApply on half-loaded chunks), OT overrun
via a lifted cap on a path missing SH_CLAMP_OT_DEPTH, or the draw-all branch
touching a chunk whose ipdHdr is mid-fixup (the classic lmHdr NULL window).

Then build a REAL hosted-interior predicate. Strongest candidate: hosted
interiors are chunks PLACED AT PARKED GRID CELLS via `Map_PlaceIpdAtCell`
(e.g. THR05FD at (-1,8)) and the interior rooms teleport the player there —
so "inside" = the player's current cell is a placed/parked cell, which is
also exactly why the street is invisible from inside on vanilla. A parked-
cell registry (collect the Map_PlaceIpdAtCell targets at map init) gives an
exact, per-map-correct test with no fog or room-index assumptions.
