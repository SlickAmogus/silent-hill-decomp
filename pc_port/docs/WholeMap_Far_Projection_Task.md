# Whole-Map Far Projection — Task Spec (dedicated session)

Status: **v2 SCENIC REDESIGN 2026-07-13 (awaiting in-game test).**
The v1 build system-crashed on 2026-07-12: the user loaded a save INSIDE the
Levin Street house with a hi-res texture pack enabled; the machine hard-hung
during the map2_s00 load and had to be power-cycled.

## What the 2026-07-12 crash taught (v1 post-mortem)

1. **Hosted interiors live in the SAME map grid.** The Levin house is THR
   cells of map2_s00 (the street map) — NOT a `Map_PlaceIpdAtCell` placement.
   The v1 parked-cell gate only knew the 2 placed cells, so it read "outdoors"
   inside the house.
2. **With the gate wrongly on, texture-all + a texture pack is unbounded.**
   Every claimed page composes + uploads a pack-resolution RGBA GL texture
   (with mipmaps) PER CLUT ROW (`Fs_QueuePostLoadTim` → `TexPack_Compose`).
   The crash log shows ~1500 GL textures created mid-load ([POOLTEX] slot 115,
   tex=1485) before the system died of memory exhaustion. v1's crash fixes
   removed the fast process-crash and let it grind into system death.
3. **Clarified intent (user, 2026-07-12): this is a SCENIC mode.** The only
   goal is seeing/flying around the whole town on the few exterior maps.
   Distant geometry/textures may be low quality; no distant collision,
   monsters, items, or triggers are wanted.

## v2 design (implemented 2026-07-13)

- **Outdoor-room classification** (`bodyprog_80040B74.c`): every grid cell
  with an IPD is classified by the map's own authored position→room function
  (`g_MapOverlayHdr.mapRoomIdxGet`, the same one `Game_MapRoomIdxUpdate` uses),
  sampled at **5 points per cell** (center + 4 corners inset 14u — the
  authored street bands are ~24–32u wide and miss cell centers on map2_s00's
  east-west streets; corners catch them; hosted-interior rooms come from the
  per-cell fallback grid so all 5 samples agree inside a house). A room
  spanning ≥ 3 DISTINCT CELLS = outdoor; a cell is outdoor if ANY of its
  samples hits an outdoor-sized room (an intersection cell's corners reach
  into the adjoining street bands). Table computed lazily once per map load
  (invalidated in `Ipd_PlayerChunkInit` / `Map_PlaceIpdAtCell`), logged
  one-shot as `[WHOLEMAP] room table`.
  - **Gate** = config prereqs && exterior && the PLAYER'S CELL (floor of the
    actual player position) is outdoor. Cell-based, not room-based, so the
    mode cannot flicker off on small street/intersection rooms, and an active
    gate implies the ground under the player is textured and drawn. Off
    inside the Levin house from the first load frame.
  - **Far-draw filter** = outdoor cells only (+ parked cells excluded), so
    interior islands never float in the flyover and the street never renders
    through a house. Chunks inside the vanilla claim window (padded distance
    ≤ 0) are exempt from both filters — the local scene always matches
    vanilla regardless of classification.
  - Known cosmetic limitation: genuinely-outdoor rooms spanning only 1–2
    cells (a few nooks, e.g. map2_s00 rooms 0x1B/0x1C/0x1D) classify indoor —
    the mode pops off standing in them and they hole the far view. Safe
    direction (conservative); whitelist from test feedback if it bothers.
- **Texture-all v2** (`Ipd_ChunkMaterialsApply`): claims outdoor cells only,
  staggered — at most 3 FIRST-TIME chunk claims per frame, nearest first
  (padded edge distance). Already-textured chunks keep their per-frame
  refresh. Interior-class maps unchanged.
- **Texture-pack GL byte budget** (`hires_override.c` + `fsqueue_3.c`):
  every pack-composed GL texture is charged (mips ≈ 4/3×) and credited when
  replaced/deleted; once live pack bytes exceed 768 MB, the compose loops stop
  making MORE pack textures (one-shot `[TEXPACK]` log) — those rows keep
  native disc art. Nearest-first claims mean the budget favors what is close.
  Normal streamed play churns slots in place and never approaches the cap.
- **Mid-walk flush** (`PsyX_GPU.cpp ParsePrimitivesLinkedList`): when the
  vertex buffer nears full, `DrawAllSplits()` draws the accumulated batch and
  the walk continues — unbounded geometry, bounded memory, painter's order
  preserved across flushes. Replaces v1's truncation (which dropped the
  NEAREST geometry — the walk is far→near). OT-walk safety cap raised
  16384 → 1M nodes (it counts per-prim nodes; the town exceeds 16k prims).
- **Unchanged from v1** (all still active, all verified): GTE far projection
  (unclamped re-projection past SZ3/IR saturation, PsyX_GTE.cpp), world-space
  per-chunk frustum reject (FOV/aspect-aware cone), `SH_WHOLEMAP_FARCAP` /
  `SH_WHOLEMAP_DEPTH_RESCUE`, widened split indices.

Read alongside memories `[[project_interior_room_islands]]` (whole-map draw-path
history + the four lifted gates), `[[project_pgxp_implementation]]` (float GTE
infra), and `pc_port/docs/PGXP_NearClip_Design.md` (prior art for a gated
projection change).

## IMPLEMENTATION (2026-07-12)

Four independent walls were confirmed and fixed; all gated on
`Pc_WholeMapDrawActive()` / the PsyCross flag `g_PsxWholeMapFar` so mode-off is
byte-identical.

1. **GTE far projection (the visible-distance wall).** `GTE_RotTransPers`
   (PsyCross `src/gte/PsyX_GTE.cpp`) clamps view depth (`C2_SZ3 = Lm_D(m_mac3,1)`
   -> `[0,0xffff]`) and view X/Y (`IR1/IR2` -> s16); beyond that, `screen =
   OFX + IR*H/SZ3` freezes. The PGXP float path divided by the *clamped* SZ3 too,
   so it saturated identically. FIX: when `g_PsxWholeMapFar` and a clamp actually
   fired (`fz=gte_shift(m_mac3,1) > 0xffff || C2_MAC1!=C2_IR1 || C2_MAC2!=C2_IR2`),
   recompute the screen coord in double precision from the UNCLAMPED analogs
   `C2_MAC1/C2_MAC2 * (C2_H / fz)` and overwrite `C2_SX2/C2_SY2` (still Lm_G-limited
   to the screen box). Feeds the normal prim path AND (when PGXP on) the PGXP FIFO
   with `fx/fy` + `pgxpW=(float)fz` so PGXP-on additionally gets correct far DEPTH.
   In-range this is identical to the standard path (only genuinely-far verts change).

2. **s16 depth-scratch wrap** — unchanged; `SH_WHOLEMAP_DEPTH_RESCUE` (already
   present) buckets wrapped-negative far polys into the last OT slot.

3. **Vertex-buffer capacity / the crash.** PsyCross accumulates every vert into
   `g_vertexBuffer[MAX_VERTEX_BUFFER_SIZE]` via unchecked `g_vertexIndex += 6`;
   129 chunks submitted at once overran it (the 2026-07-11 write-AV on the street).
   FIX: (a) `MAX_VERTEX_BUFFER_SIZE` 1<<16 -> 1<<18 and `GPUDrawSplit.startVertex/
   numVerts` u_short -> unsigned int (output-neutral for normal play); (b) a
   one-shot `[VBUF]` break in `ParsePrimitivesLinkedList` stops the OT walk before
   any emit could overrun (reserve 24 = largest single-prim LINE_F4); (c) a
   **per-chunk world-space frustum reject** (`Pc_WholeMapChunkCulled`, game side)
   transforms each 40u cell center through `GsWSMATRIX` (world->view, immune to the
   GTE saturation) and drops cells behind the camera or outside a cone whose slope
   = `(160/H)*(winAspect/(4:3))*1.3` (`H=ReadGeomScreen` folds in fps_fov; tracks
   ultrawide; +30% margin -> never over-culls). This bounds the software-GTE
   transform + vertex count. Reported via `[WHOLEMAP] ... culled=N`.

4. **Activation gate.** Replaced the crash-prone `isFogEnabled` gate with a
   parked-cell registry: `Map_PlaceIpdAtCell` targets (hosted-interior host cells)
   are recorded (reset each map load in `Ipd_PlayerChunkInit`); "inside" = the
   player's ACTUAL-position cell (`FLOOR(g_SysWork.playerWork.player.position)`,
   NOT `g_Map.cellX/cellZ` which is a ~14u-forward-projected sample that leaked the
   old gate) equals a parked cell. Exact, per-map-correct, conservative-safe.

**Depth note:** far OT order is painter's (rescue bucket); for correct depth
ordering of distant buildings enable PGXP (`use_pgxp=1` / F1) — its unclamped
per-vertex W (now fed by the far block) sorts them exactly.

**Test:** add `whole_map_exteriors = 1` (with `preload_chunks=1`,
`resident_textures=1`) to config, optionally `use_pgxp=1`; map0/map2 street; check
`[WHOLEMAP]` shows `drawn` bounded by `culled`, whole town visible receding
correctly, no crash on the street or entering a house. `fogstr 0` (console) to see
distant geometry through the haze.

---
### Original spec (below, for provenance)

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
