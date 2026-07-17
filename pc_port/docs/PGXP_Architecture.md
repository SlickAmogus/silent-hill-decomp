# PGXP Architecture — Shadow-Memory Model (DuckStation-faithful) + Near-Plane Clipping

**Status:** IMPLEMENTED. This documents the PGXP architecture the code now
implements — the shadow-memory model (§1–§8) plus near-plane clipping (§9). The
step list in §5 records the build order and doubles as a map of where each piece
lives; read §1 and §6 before touching any of it.
**Goal:** Replace the entire home-grown PGXP matching (address map + per-prim parked
set + (x,y) ring + closest-(x,y) + slot-index + weld) with one deterministic
shadow-memory model, faithful to DuckStation. Result: characters and environment
both correct — no collapse, no wobble, no seams, no gaps, no flicker. Where a vertex
isn't tracked it falls to **clean affine** (PSX look), never garbage.

---

## 1. Why the current approach fails (do not repeat these)

The current system tries to *re-associate* a drawn vertex with its precise GTE
projection using heuristics, because the precise value is stored under the GTE's
scratch address but the GPU draws from a *different* address (the prim packet the
game copied it into). Every heuristic has a failure mode we cycled through tonight:

- **(x,y) ring** (match by integer screen coord + hint): collisions → wobble, and
  cross-prim joint verts get different precise → **seams**.
- **closest-(x,y)** (nearest parked vert): two corners on the same pixel grab the
  same vertex → **flat collapse** (pews, crucifix, Harry's legs).
- **value-as-primary-key** (match parked vert whose integer value equals drawn):
  same-pixel corners share a value → also **collapse**.
- **slot-index** (parked slot i = drawn vertex i): correct *only* where the drawer's
  copy order matches the park order; silently wrong elsewhere.
- **weld** (snap nearby cross-bone verts): papers over coverage gaps, overshoots
  into **collapse**; DuckStation has no weld.
- **small vs big hash tables**: small → everything overflows to the ring (`det=0`);
  big → parked tier used but the heuristics above bite.

Root cause: **incomplete + non-deterministic coverage.** The fix is not a better
heuristic — it's making coverage complete and address-exact so no heuristic is needed.

## 2. How DuckStation actually works (the reference)

Source: `github.com/stenzek/duckstation` → `src/core/cpu_pgxp.cpp` / `.h`
(was `pgxp.cpp`; pull with `gh api -H "Accept: application/vnd.github.raw"
"repos/stenzek/duckstation/contents/src/core/cpu_pgxp.cpp"`).

DuckStation is a CPU emulator. It keeps a **shadow array parallel to PSX memory**
(`s_mem`, one `PGXPValue` per memory word) plus shadow CPU/GTE registers. Every
relevant CPU instruction is hooked (`CPU_LW/SW/LWC2/SWC2/MOVE/ADD/...`) to *copy the
shadow value along the same data path* as the real value. So when a vertex is
projected by the GTE (`GTE_RTPS` stores precise x,y,z into a shadow register) and
the game shuffles it through memory into a GP0 packet, the precise value rides
along to wherever the GPU finally reads it.

`PGXPValue` (cpu_pgxp.cpp): `{ float x, y, z; u32 flags; u32 value; }` where
`flags` has `VALID_X/Y/Z` bits and **`value` is the integer word it shadows**.

The GPU-side lookup is the whole game (cpu_pgxp.cpp):

```cpp
bool GetPreciseVertex(u32 addr, u32 value, int x, int y, int xOffs, int yOffs,
                      float* out_x, float* out_y, float* out_w) {
  const PGXPValue* vert = GetPtr(addr);                       // shadow at SOURCE ADDRESS
  if (vert && (vert->flags & VALID_XY) == VALID_XY && vert->value == value) { // EXACT value validate
    *out_x = TruncateVertexPosition(vert->x) + xOffs;
    *out_y = TruncateVertexPosition(vert->y) + yOffs;
    *out_w = vert->z / GTE::MAX_Z;
    if (IsWithinTolerance(*out_x, *out_y, x, y))               // garbage guard
      return (vert->flags & VALID_Z) == VALID_Z;
  }
  // vertex cache (match by value only) — OFF by default ("makes warping worse")
  // MISS: out = (x, y), w = 1  -> affine
  *out_x = x; *out_y = y; *out_w = 1.0f; return false;
}
```

**Takeaways that define our rewrite:**
1. Primary key is the **unique memory address** the vertex word came from.
2. `value` (the integer word) is only a **validation**, never the key.
3. Coverage comes from **complete propagation through memory**, not heuristics.
4. No (x,y) ring (value-cache is off by default). No nearest-match. No weld.
5. Miss → **affine** (`x, y, w=1`), always clean.
6. Seams don't exist because both joint verts are tracked end-to-end and project to
   the same precise value → they coincide. (No weld required.)

## 3. Our adaptation (we have NO CPU to hook)

The decompiled game runs as native code — there is no instruction stream. But we
have the **source**, so we instrument the exact points where vertex words move:

- **GTE projection** → already ours (`PsyX_GTE.cpp GTE_RotTransPers` already computes
  the precise `fx,fy` and pushes a FIFO `s_pgxpFifoX/Y/W`). Keep.
- **GTE store** (`gte_stsxy*` macros, ours, in `pc_port/include/.../inline_no_dmpsx.h`
  or wherever the `gte_stsxy3c / gte_stsxy3_g3 / gte_stsxy` live) → writes the integer
  to a dest address. Here we write the **shadow**: `Shadow_Store(destAddr, fifo.x,
  fifo.y, fifo.w, integerValue)`. (Today this is `PGXP_StoreAddr`/`PGXP_MapPut`; add
  the integer value.)
- **Vertex copy** (`poly->xN = screenXy_0[field]` in the drawers) → the missing
  piece. After each copy, **propagate the shadow**: `Shadow_Copy(&poly->xN,
  &screenXy_0[field])` which does `shadow[dst] = shadow[src]` if present. This puts
  the **prim-field address** into the shadow, so the GPU side resolves by address.
- **GPU draw** (`MakeVertex` path in `PsyX_GPU.cpp`) → `GetPreciseVertex(primFieldAddr,
  drawnIntegerValue, ...)` exactly like DuckStation: address lookup + value validate +
  tolerance → precise, else affine.

This is address-exact and deterministic: a vertex is either propagated (precise) or
not (clean affine). No collisions, no collapse, no oscillation.

## 4. Data structures

```c
struct ShadowEntry { uintptr_t key; u32 gen; u32 value; float x, y, w; };
// one big open-addressed hash table keyed by native address of the vertex WORD.
// `value` = the packed integer (s16 x | s16 y << 16) that lives at key.
// `gen`   = frame generation (bumped once/frame) for stale-entry rejection.
```

Sizing: must hold every projected vertex word AND every copied prim-field word for
one frame (~230k verts → up to ~0.5–1M words). Use `2^20`–`2^21` open-addressed,
~16-probe. (Memory is cheap; correctness first. Tune after it works.)

Keep the precise **FIFO** in `PsyX_GTE.cpp` as the bridge from "GTE projected this"
to "the store macro knows the address."

## 5. Implementation steps (in order, each independently testable)

> Build: `"C:/msys64/usr/bin/bash.exe" -lc 'export MSYSTEM=MINGW64 && cmake --build
> /c/Claude/silenthill/silent-hill-decomp/pc_port/build 2>&1'`. Exe relink fails
> "Permission denied" while the game is running — that's fine, user closes it.
> Log: `pc_port/build/SilentHill.log`. PGXP toggles: console `pgxp 0/1`, F1.

**Step 0 — keep OFF byte-identical.** All shadow work is gated by `g_PsxUsePgxp`.
PGXP-off path must be untouched/affine. Verify first.

**Step 1 — Shadow store + value.** Add `value` to the shadow entry. In the
`gte_stsxy*` macros, after writing the integer to the dest address, call
`Shadow_Store(destAddr, fifoX, fifoY, fifoW, *(u32*)destAddr)`. Confirm direct-store
prims (effect quads via `gte_stsxy3_g3` → prim field) resolve via address+value at
draw (these need NO copy step — the GTE writes straight into the prim).

**Step 2 — GPU lookup = GetPreciseVertex.** Rewrite the `MakeVertex` resolve to:
look up shadow[primFieldAddr]; if present, `gen==cur`, `value==drawnValue`, and
within 2px tolerance → use precise (`+ draw offset`); else affine (`x,y,w=1`).
Delete the parked-set/ring/closest/slot/weld tiers. After this step only direct-store
prims are precise; everything copied is affine (expected) — confirm NOTHING warps or
collapses (clean affine fallback), env may look mostly affine. That's the safe floor.

**Step 3 — Copy propagation in the core drawer.** In
`src/bodyprog/gfx/bodyprog_80055028.c` `Gfx_MeshDraw`, at the two emit sites where
`poly->x0..x3 = scratchData->screenXy_0[field_10..13]` (≈ lines 2188 and ~2855),
after each assignment add `Shadow_Copy(&poly->xN, &scratchData->screenXy_0[field_1X])`
(behind `#ifdef SH_PC_PORT`, gated by `g_PsxUsePgxp`). This is where the existing
`PsyX_SetNextPrimPgxp` bridges are — replace them. Now world meshes + characters
resolve by address. Verify: env detail correct, NO collapse, Harry's faces solid.

**Step 4 — Remaining copy sites.** Same propagation at: `func_8005AC50` GT3/GT4
emit, the FT4 "second render path" (~3040), and any other `screenXy_0 → prim` copy.
Grep `screenXy_0` and the old `PsyX_SetNextPrimPgxp` call sites for the full list.
Each instrumented site = exact coverage; each missed site = clean affine.

**Step 5 — Billboards stay affine.** `Gfx_BillboardDraw` builds quad corners in
screen space (never GTE-projected) — do NOT propagate; leave affine (current
`PsyX_SetNextPrimAffine` behaviour). 2D/HUD likewise affine.

**Step 6 — Delete the dead machinery.** Remove `PgxpVtx`/parked table/`PgxpPrimStore`/
`PGXP_BeginPrim`, the ring (`PGXP_LookupHinted` and its push), `WeldVertex` + all weld
console cmds/globals, `g_pgxpCharSnap`/CHARSNAP (no longer needed — full PGXP is the
only mode and it's correct), slot plumbing. Keep `g_primPgxpForceAffine` for billboards.

**Step 7 — Tune + verify seams gone.** With full coverage, the two bone-joint verts
(same world position) project to the same precise value and coincide → seams vanish
with no weld. Confirm on Harry. Re-add a coverage probe (`det% / affine%`) only if a
gap remains; chase it to the un-instrumented copy site.

## 6. Key facts learned tonight (don't re-derive)

- **GTE clamping/garbage** (`PsyX_GTE.cpp` ~316): the prim stores the GTE's *clamped*
  integer (`Lm_G1`), but PGXP keeps the *unclamped* float. For verts behind camera /
  extreme depth the unclamped float is **garbage** — that's why the 2px tolerance
  guard (`IsWithinTolerance`) is REQUIRED even on an exact address match. Without it
  the scene shatters. (DuckStation has the same guard.)
- The precise FIFO (`s_pgxpFifoX/Y/W`) already mirrors the GTE SXY FIFO so the store
  macro can attach precise to an address. Reuse it.
- `Gfx_MeshDraw` copy order IS aligned (`poly->x0..x3 = screenXy_0[field_10..13]` in
  order, no winding swap) — verified. So address propagation there is straightforward.
- `det=0` on small tables means parked tier unused (overflow). Irrelevant after this
  rewrite (no parked tier).
- DuckStation's vertex-cache (value-only fallback) is OFF by default and makes
  warping worse — do not add an (x,y) fallback.
- Coverage gap → affine is acceptable and safe; never add a heuristic to fill it.

## 7. Files

- `pc_port/PsyCross/src/gpu/PsyX_GPU.cpp` — shadow table, `Shadow_Store/Copy`,
  `GetPreciseVertex`, delete parked/ring/closest/weld.
- `pc_port/PsyCross/src/gte/PsyX_GTE.cpp` — FIFO (keep), gate (keep).
- gte store macros header (`gte_stsxy*`) — `Shadow_Store` on store.
- `pc_port/PsyCross/src/render/PsyX_render.cpp` — shader already texture-only PGXP;
  no change expected (verify `ppw` semantics: position is W-independent, W only
  perspective-divides varyings).
- `src/bodyprog/gfx/bodyprog_80055028.c` — copy propagation (Gfx_MeshDraw, ~2188,
  ~2855), `func_8005AC50`, FT4 path.
- `pc_port/src/pc_console_cmd.c` — drop WELD/WELDW/CHARTEX/CHARSNAP, keep `pgxp`.

## 8. Success criteria

PGXP on: environment intricate scenes (church/pews/crucifix) crisp, no gaps/flicker;
Harry full-PGXP has NO flat collapse, NO seams, NO wobble, looks like clean affine
with stable sub-pixel positions. PGXP off byte-identical to affine. No new crashes.

---

## 9. Near-plane clipping (PgxpNearClipEmit)

*(Was `PGXP_NearClip_Design.md`, folded in 2026-07-17. Implemented 2026-07-06.)*

### Symptom

With PGXP on, geometry very close to the camera warps/smears like affine mapping
(hallway corner and locker close-ups in first person). Normal viewing distance is
perspective-correct; the warp appears only as the camera approaches within ~a
character radius of the surface.

### Root cause

`GTE_RotTransPers` (pc_port/PsyCross/src/gte/PsyX_GTE.cpp ~316):

- In-front vertices (`SZ3 > 0`) get a full-precision float projection and a
  positive W → perspective path. This already covers the "close but in front"
  case (the old Lm_E saturation bug was fixed earlier).
- Vertices **at/behind the near plane** (`SZ3 == 0`) have *no valid projection*
  — the code stores `W = 0`, and `GetPreciseVertex` (PsyX_GPU.cpp ~190) sends
  them down the affine path.

A polygon that *straddles* the camera plane (wall/floor plane continuing past the
eye — guaranteed when the FPS camera leans into a corner) therefore renders with
MIXED per-vertex modes: some perspective (ppw>0), some affine (ppw=0). The
interpolation across the poly is inconsistent → the affine-looking smear.

This is not a regression: PSX hardware has the same failure (no near clipping;
SH1 works around it by dynamically subdividing map geometry near the camera,
tuned for cameras that never got this close). PGXP just makes the rest of the
frame clean enough that the near-warp stands out, and FPS mode creates camera
positions the original game never produced.

### Why per-vertex tricks cannot fix it

A vertex behind the eye has no meaningful screen position or 1/W; any value
assigned to it produces some wrong interpolation. The only correct treatment is
to CLIP the primitive against a near plane and generate new vertices at the
intersection — what every real 3D pipeline does before the divide.

### Approach

Clip at prim-assembly time in the GL backend (PsyX_GPU.cpp), in view space, using
the view-space FIFO (`VsEntry`: GTE RTPS MAC1/2/3 = view-space x,y,z, address-keyed
+ value-validated). Sutherland–Hodgman against `z = NEAR` in view space, interpolate
per-vertex UV/RGB/view-pos along each crossing edge, fan-triangulate, re-project the
new vertices with the PGXP path's formula (`sx = OFX + vsx * (H / vsz)` etc.), and
feed the triangles through the normal vertex-emission path (same OT bucket, same
texture/blend state) so painter's-order and semi-transparency are unaffected.

### As-built deltas (decided at implementation)

- View-space validity marker rides `GrVertex.ny` (unread by every shader): a
  behind-the-eye vertex legitimately has `vsz <= 0`, so "has a vs entry" can't be
  inferred from the position itself.
- Eligibility requires only the view-space entries (not the PGXP shadow): kept
  in-front vertices reuse their GTE-precise projection when present (`ppw > 0`,
  bit-identical to the unclipped case so shared edges with neighbouring unclipped
  polys can't crack) and are re-projected from view space otherwise.
- The whole-poly affine drop in MakeVertexTriangle/Quad spares clip-eligible polys
  (it would destroy the in-front vertices' precise data before the clipper runs);
  vs fill was moved above the PGXP block to enable the test.
- Clipping runs per emitted triangle (post-TriangulateQuad) via `PgxpNearClipEmit`
  at all 8 3D-poly sites; a quad grows to at most 12 verts, guarded against
  vertex-buffer overflow. **Quad diagonals can't crack**: both triangles hold
  bit-identical copies of the shared verts, so the lerp yields identical clip verts.
- Re-projected verts **skip the `g_PgxpEdgeMax` clamp**: with a true W the GPU
  clips far-off-screen positions exactly in homogeneous space; clamping would drag
  the vertex and distort the visible part.
- NEAR default 16.0 GTE units (flat, not `C2_H/16`): an invisible cut right at the
  eye. Tunable via console `pgxpnearz`; toggle `pgxpnearclip` (default ON with
  PGXP). OFF path byte-identical.
- Fully-behind polys (all `vsz < NEAR`) are left untouched, not dropped — the
  GTE/game culls them anyway, and matching today's behavior keeps the change minimal.
- The view-space FIFO gate is `(g_PsyX_UsePerPixelFlashlight || g_PsxUsePgxp)` in
  PsyX_GTE.cpp `PGXP_StoreAddr` + `GTE_RotTransPers` + `Shadow_Copy`.
- `[PGXP] cov` log line gained `clip=N` (polys clipped in the 60-frame window).

### Console knobs

`pgxpnearclip 0/1` (default ON with PGXP), `pgxpnearz <units>` (NEAR, default 16.0).
OFF path byte-identical.
