# PGXP Near-Plane Clipping — Design (2026-07-06)

> **Status — Supporting reference.** PGXP near-plane clipping is implemented, runtime-gated with PGXP, and defaults on within that experimental path. Use `pgxpnearclip` and `pgxpnearz`; see the [feature catalog](../../features.md), [operational reference](Console_And_Debug_Reference.md), and [documentation index](README.md). The design below records its rationale.

## Symptom

With PGXP on, geometry very close to the camera warps/smears like affine mapping
(user screenshots: hallway corner and locker close-ups in first person). Normal
viewing distance is perspective-correct; the warp appears only as the camera
approaches within ~a character radius of the surface.

## Root cause (traced 2026-07-06)

`GTE_RotTransPers` (pc_port/PsyCross/src/gte/PsyX_GTE.cpp ~316):

- In-front vertices (`SZ3 > 0`) get a full-precision float projection and a
  positive W → perspective path. This already covers the "close but in front"
  case (the old Lm_E saturation bug was fixed earlier).
- Vertices **at/behind the near plane** (`SZ3 == 0`) have *no valid projection*
  — the code stores `W = 0`, and `GetPreciseVertex` (PsyX_GPU.cpp ~190) sends
  them down the affine path.

A polygon that *straddles* the camera plane (wall/floor plane continuing past
the eye — guaranteed when the FPS camera leans into a corner) therefore renders
with MIXED per-vertex modes: some perspective (ppw>0), some affine (ppw=0). The
interpolation across the poly is inconsistent → the affine-looking smear.

This is not a regression: PSX hardware has the same failure (no near clipping;
SH1 works around it by dynamically subdividing map geometry near the camera,
tuned for cameras that never got this close). PGXP just makes the rest of the
frame clean enough that the near-warp stands out, and FPS mode creates camera
positions the original game never produced.

## Why per-vertex tricks cannot fix it

A vertex behind the eye has no meaningful screen position or 1/W; any value
assigned to it produces some wrong interpolation. The only correct treatment is
to CLIP the primitive against a near plane and generate new vertices at the
intersection — i.e. what every real 3D pipeline does before the divide.

## Design

Clip at prim-assembly time in the GL backend (PsyX_GPU.cpp), in view space,
using data we already capture per vertex:

1. **View-space source**: the flashlight FIFO (`VsEntry`: GTE RTPS MAC1/2/3 =
   view-space x,y,z, address-keyed + value-validated as of this batch) has
   exactly the needed positions. Change its gate from
   `g_PsyX_UsePerPixelFlashlight` to `(g_PsyX_UsePerPixelFlashlight ||
   g_PsxUsePgxp)` in PsyX_GTE.cpp `PGXP_StoreAddr` + `GTE_RotTransPers` (and
   `Shadow_Copy`). Memory cost: none (table exists); CPU cost: negligible.

2. **Eligibility**: only 3D prims where ALL vertices resolve BOTH a PGXP shadow
   entry (value-validated) AND a view-space entry, and at least one vertex has
   `ppw == 0` (i.e. SZ3 clamped to 0) while at least one has `vsz > NEAR`.
   Everything else keeps the current behavior (fully-in-front polys are already
   correct; fully-behind polys are culled by the GTE/game anyway).

3. **Clip**: Sutherland–Hodgman against `z = NEAR` in view space (NEAR = a few
   GTE units, e.g. `C2_H / 16`, tunable). Interpolate per-vertex UV, RGB, and
   view pos along each crossing edge. A clipped quad yields up to 5 vertices →
   fan-triangulate.

4. **Re-project** new vertices with the same formula the PGXP path uses:
   `sx = OFX + vsx * (H / vsz)`, `sy = OFY + vsy * (H / vsz)`, `W = vsz`
   (matching `pgxpW`'s unquantized scale). OFX/OFY/H must be the values active
   at GTE time — capture them alongside the vs FIFO (they are GTE registers;
   store per-frame, they don't change mid-frame in SH1).

5. **Feed** the resulting triangles through the normal vertex-emission path of
   the same prim (same OT bucket, same texture/blend state) so painter's-order
   and semi-transparency are unaffected.

6. **Toggle**: console `pgxpnearclip 0/1` (default 1 when PGXP on) for A/B and
   regression triage; OFF path byte-identical to today.

## Risks / notes

- The guard-band position clamp (`g_PgxpEdgeMax`) still applies to the
  re-projected verts; clipped verts project inside the frustum by construction
  so they never hit it.
- Gouraud interpolation of RGB along the clipped edge differs slightly from
  PSX (which never drew these polys correctly at all) — acceptable.
- The prim's integer vertices stay untouched; only the GL vertex stream is
  clipped, so PGXP-off rendering is unaffected.
- Test plan: FPS mode, school hallway corner + locker from the report; also
  classic mode close-camera cutscenes (Kaufmann office) for no-regression, and
  `pgxpnearclip 0` A/B.

## Status

IMPLEMENTED 2026-07-06 (PsyCross: PsyX_GTE.cpp + PsyX_GPU.cpp; console cmds in
pc_console_cmd.c). Deltas from the design above, decided at implementation:

- View-space validity marker rides `GrVertex.ny` (unread by every shader): a
  behind-the-eye vertex legitimately has `vsz <= 0`, so "has a vs entry" can't
  be inferred from the position itself.
- Eligibility requires only the view-space entries (not the PGXP shadow): kept
  in-front vertices reuse their GTE-precise projection when present (`ppw > 0`,
  bit-identical to the unclipped case so shared edges with neighbouring
  unclipped polys can't crack) and are re-projected from view space otherwise.
- The whole-poly affine drop in MakeVertexTriangle/Quad now spares clip-eligible
  polys (it would destroy the in-front vertices' precise data before the
  clipper runs); vs fill was moved above the PGXP block to enable the test.
- Clipping runs per emitted triangle (post-TriangulateQuad) via
  `PgxpNearClipEmit` at all 8 3D-poly sites; a quad grows to at most 12 verts,
  guarded against vertex-buffer overflow. Quad diagonals can't crack: both
  triangles hold bit-identical copies of the shared verts, so the lerp yields
  identical clip vertices.
- Re-projected verts skip the `g_PgxpEdgeMax` clamp: with a true W the GPU
  clips far-off-screen positions exactly in homogeneous space; clamping would
  drag the vertex and distort the visible part.
- NEAR default 16.0 GTE units (flat, not `C2_H/16`): an invisible cut right at
  the eye. Tunable via console `pgxpnearz`; toggle `pgxpnearclip` (default ON
  with PGXP). OFF path byte-identical.
- Fully-behind polys (all `vsz < NEAR`) are left untouched, not dropped — the
  GTE/game culls them anyway, and matching today's behavior keeps the change
  minimal.
- `[PGXP] cov` log line gained `clip=N` (polys clipped in the 60-frame window)
  for in-game verification.

Test plan (unchanged): FPS mode, school hallway corner + locker close-ups;
classic-mode close-camera cutscenes (Kaufmann office) for no-regression;
`pgxpnearclip 0` A/B.
