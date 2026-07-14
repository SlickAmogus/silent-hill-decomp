# PGXP PRs Vetting — decomp #51 + PsyCross #11 (2026-07-14)

Adversarial multi-agent review (57 agents, findings verified end-to-end against
both PR branches and our HEADs). PR branches fetched locally as `pr51-pgxp`
(decomp, merge-base `4d94eae90`, 26 commits behind pc-port) and `pr11-pgxp`
(PsyCross, merge-base `58866c94`, 3 commits behind — notably missing the
mid-walk vertex-buffer flush `55e1dfe` and the override sampler `0bc5fca`).
Diffs + full findings in the session scratchpad `pgxp_pr/`.

**Verdict: ADAPT — genuinely valuable core, unmergeable as-is.**
All five review dimensions (off-invariance, gameplay-invariance,
depth-architecture, look-drift, code-quality) independently reached the same
conclusion. 40 confirmed findings: 5 critical, 13 major, 22 minor.

## What it genuinely fixes (all on our open-bugs list)

- **PGXP vertex jitter, at the root**: camera/model/dynamic-vertex transforms
  lose 4 bits at Q12→Q8 truncation; the PR captures exact "twins" at the GTE
  macro layer, generation-stamped and value-validated against the quantized
  memory before use, falling back conservatively (quantized, never wrong
  geometry). The validation discipline is genuinely well engineered
  (isfinite/w guards everywhere, no store/copy race, content-keyed lookups).
- **Coplanar z-fighting (rugs, save pads, papers)**: the central insight is
  elegant and correct for us — draw static opaque world in exact PS1 OT order
  with GL_ALWAYS + depth writes (authentic PSX layering preserved, painter
  winner leaves true per-pixel depth), then LEQUAL-test other opaque 3D
  against it; transparent tests without writing. This sidesteps the
  "true per-vertex depth" approach we failed at three times.
- **Snow/particles through walls** (explicit camera-space SZ capture),
- **inventory pickup see-through** (per-pixel depth + EXACT SZ — our OPEN bug),
- **precise TMD quad backface** (silhouette flicker; integer decision verified
  bit-identical when off), **marker-based v_is3d** (stops per-frame
  dither/bilinear flicker on affine-fallback polys), **scissor-safe
  clears + depth-write state-cache repair** (fixes a real latent bug),
  **constant szMax=2^18** replacing the content-dependent prev-frame max.

## Disqualifiers (must fix before any of it ships)

1. **`use_pgxp=0` is grossly broken** (root of all 5 criticals). The depth
   pipeline is only half-gated: world prims get classified/marked ungated
   (`PsyX_SetNextPrimSz` producers are `#ifdef SH_PC_PORT` only,
   `ApplyGtePerVertexDepth` ungated), while character/prop markers ARE
   PGXP-gated. The new unconditional 3-pass DrawAllSplits reorder then draws
   world first (GL_ALWAYS, depth-writing) and characters LAST with depth
   testing disabled — **Harry renders through walls with PGXP off**. The PR
   also deletes the current off-path z machinery (GR_SetBlendMode/
   GR_EnableDepth coupling, quantization-64 flatten, per-OT depth clears)
   that today's default `pgxpZBuffer=1` path relies on. The author validated
   ON-mode only (10-second smoke test).
2. **A gameplay-state write**: `vw_main.c Math_MatrixToPosition` writes the
   recovered unquantized camera position back into `vwViewPointInfo.worldpos`
   — which feeds player control, NPC AI, SFX attenuation and map triggers.
   A graphics toggle must never be gameplay-visible. Delete/side-channel it.
3. **~19-24 hook blocks missing `#ifdef SH_PC_PORT`** (bodyprog_8005E0DC.c,
   map6_s00.c and 6 more files) — breaks the PSX matching build.
4. **Ungated hot-path cost when off**: matrix twins run
   `std::unordered_map` insert/find + double 3x3 math on every
   `gte_SetRotMatrix`/`gte_ldclmv`/`MulMatrix0` (per bone per frame);
   `gte_ldv*` adds per-vertex call overhead; GrVertex grows 60→64 bytes for
   all paths; SZ table 64KB→6MB. Estimated 0.1–1 ms/frame of pure waste at
   use_pgxp=0. (Their own `gte_stsxy*` hooks show the correct gated pattern.)
5. **Coverage-dependent occlusion** (the design's structural weakness): any
   prim without provenance (only ~20 of 60+ map dirs are instrumented; future
   code, mods, region variants) is classified 2D and drawn in the final
   depth-off pass — **always on top**, the inverse of the bug being fixed.
   Fix: keep ORIGINAL painter order with per-split depth states instead of
   the 3-pass reorder; untracked content then degrades to PS1-correct painter.
6. **Invalidates our decal tuning** (`DECAL_SZ_BIAS=96` becomes a 96-unit
   see-through/overdraw window against true per-pixel depth) and changes the
   PSX composite look of intentional screen-space overlays (lens ghosts,
   water/flashlight flares, near-bucket-forced FX now depth-occlude at source
   depth) — needs per-spot A/B sign-off.
7. **Stale base**: PsyCross side predates our mid-walk vertex-buffer flush,
   which lives in the same ParsePrimitivesLinkedList/DrawAllSplits region and
   invalidates the "world depth resolves before actors test" premise (each
   mid-OT flush would reorder only its own chunk).
8. **Default flips (rejected by policy)**: `use_pgxp` 0→1, and a second,
   silent one — `pgxpedge` 8192→0, removing the shipped guard-band clamp.
9. Minor pile (fix during extraction): hooks added to dead `twinfeeler.c`
   (live code is map4_s03.c, which got its own hooks); one-shot
   `PsyX_SetNextPrimSz` leaks WORLD classification to an unrelated prim when
   the last world poly is culled; blanket -1/-1 polygon offset on transparent
   3D causes grazing-angle halos; painter tie-rank caps at 127 prims/bucket;
   `PGXP_NEAR_BEHIND` culls polys PSX rendered; `PGXP_GetSzMax` couples world
   depth to the live `pgxpnearclip` A/B knob; split merge-key fragmentation
   raises draw calls against MAX_DRAW_SPLITS=4096.

## Extraction plan (pending maintainer go-ahead)

- **Phase A — PsyCross core (adapt)**: take the exact-twin GTE layer but
  rebuild its storage on flat generation-stamped arrays (house style — the
  antithesis of per-frame unordered_map churn) and gate every entry point on
  `g_PsxUsePgxp`; take the manual-projection side channel, depth-kind
  markers, per-split depth states, GL_ALWAYS world painter, reciprocal depth
  + tie-rank, and the scissor/clear fixes (standalone commits) — but draw in
  ORIGINAL split order (no 3-pass reorder) and gate the whole depth path so
  use_pgxp=0 keeps today's byte-for-byte behavior. Rebase onto our HEAD
  (mid-walk flush interaction resolved by drawing in original order).
- **Phase B — decomp hooks**: take the ~44-file capture-hook set minus the
  `worldpos` writeback and dead twinfeeler.c, adding the missing SH_PC_PORT
  guards and runtime gates; re-tune decal biases for true depth; defaults
  unchanged.
- **Phase C — look A/B**: per-spot sign-off on composite overlays
  (flares/ghosts/forced-near FX) — keep forced-composite where the PSX look
  demands it.
- No overlap with the parallel texture session's files (hires_override.c,
  tex_pack.c, fsqueue_3.c, terrain.h untouched), but PsyX_GPU.cpp /
  PsyX_render.cpp are shared surfaces — coordinate merges.
