# PGXP Implementation Plan & Research

> **Status — Historical/superseded plan.** PGXP is now runtime-effective, experimental, and default off. `USE_PGXP=0` is vestigial; current GTE/GPU/shader paths are built unconditionally and runtime-gated. See the [feature catalog](../../features.md), [operational reference](Console_And_Debug_Reference.md), and [documentation index](README.md).

Original status of this plan: **PROPOSAL — awaiting approval.** Nothing was implemented in that snapshot.

---

## 1. Historical snapshot at the time of this plan

PGXP in our tree is **scaffolding only — not functionally implemented.** The
float-precision framework an earlier session built (GTE float twins
`g_FP_SXYZ`, a `PGXPVData` cache, `ApplyVertexPGXP`) was **reverted** and is
gone from the current code. What remains:

| Piece | State |
|---|---|
| `use_pgxp` config key + launcher "Use PGXP" radio | present, wired |
| `g_PsxUsePgxp` runtime global (set from config in `main_pc.c:444`) | present |
| `u_pgxpEnabled` shader uniform | present, pushed every shader bind |
| `USE_PGXP` compile macro | `=0`, **vestigial — no `#if USE_PGXP` branches exist anywhere** |
| `USE_EXTENDED_PRIM_POINTERS=1` → `P_TAG.pgxp_index:16` field | present, **but unused** (no `PGXP_GetIndex`/cache reads it) |
| GTE float-precision compute | **absent** — `GTE_RotTransPers` is pure integer (`>>16` truncation + `Lm_G1` clamp) |
| Vertex precise-coord cache / `ApplyVertexPGXP` | **absent** |
| Shader perspective correction | **absent** — `u_pgxpEnabled` only gates `v_is3d` (dither/bilinear), not geometry |

Net: today every prim renders via the **affine** path (`ApplyGtePerVertexDepth`
→ SZ quantization-64 depth, the current Z-fighting mitigation). Toggling
`use_pgxp` currently changes *nothing visible except dither gating*.

**Important consequence:** the old memory note recommending a **separate
`SilentHillPC_PGXP.exe`** was written when there *were* compile-time `#if
USE_PGXP` branches that couldn't be runtime-gated. **Those branches no longer
exist.** The codebase is already structured for a single-exe runtime toggle.

---

## 2. What PGXP fixes (why it's wanted)

PSX has no Z-buffer (painter's algorithm via the Ordering Table) and no
sub-pixel rasterization — the GTE truncates projected screen coords to 16-bit
integers and does affine (non-perspective-correct) texture mapping. On a PC
GPU that gives:

- **Texture/vertex "swimming" / wobble** — vertices snap to integer pixels.
- **Affine texture warping** — UVs interpolated without perspective on large
  near polys (floors, walls), the classic PSX texture "swim".
- **Z-fighting** — we synthesize GL depth from truncated SZ; coplanar faces
  straddle quantization boundaries (~19% even with quantization-64). See
  `depth_zfighting_history`.

PGXP keeps the **original sub-pixel / float-precision** projected coordinates
and a per-vertex **W** (perspective divisor) and feeds them to the GPU, which
then rasterizes with sub-pixel precision and perspective-correct interpolation
— eliminating all three.

---

## 3. Architecture: our approach vs DuckStation

**DuckStation's PGXP (`duckstation/src/core/cpu_pgxp.cpp`) is NOT directly
portable to us.** DuckStation is an *interpreter/recompiler emulator*: it hooks
PSX CPU instructions (`CPU_LW`, `CPU_SW`, `CPU_MFC2/MTC2`, …) and tracks
high-precision values *through PSX RAM by address*, then `GetPreciseVertex(addr,
…)` looks them up when the GPU draws. That whole memory-tracking machine exists
because an emulator only sees integer register/memory traffic.

**We are a native decomp** — our GTE ops are direct C calls (`gte_rtps`/
`gte_rtpt` → `GTE_RotTransPers` in `PsyX_GTE.cpp`). We have the float inputs and
can compute the precise projection *at the call site*. No memory tracking, no
instruction hooks. This is the **PsyX-native** approach (what the reverted
framework attempted, and what pcsx-redux's GL renderer does).

**DuckStation is still valuable as a reference for:** the perspective-divide
math, the W reconstruction, the validity/fallback heuristics (when a precise
vertex is missing, fall back to affine instead of NaN), and tuning constants.
We will read `cpu_pgxp.cpp`'s `GTE_RTPS`/`MakeValid`/vertex logic for the math,
not copy its structure.

**No disc data extraction is required — PGXP is a rendering algorithm, not
data.** (Confirmed; nothing to pull from `disc_extract`.)

---

## 4. The data path we will build (PsyX-native)

```
GTE_RotTransPers (PsyX_GTE.cpp)
  └─ also compute, per projected vertex, in FLOAT (no >>16, no Lm_G1 clamp):
        fSX = OFX/65536 + IR1 * (H / SZ_f)          ← precise screen X (sub-pixel)
        fSY = OFY/65536 + IR2 * (H / SZ_f)          ← precise screen Y
        fW  = view-space depth  (∝ SZ_f / H)         ← perspective divisor
     store into a small ring cache slot; remember slot index in the SXY register lane
        ↓ (gte_stsxy* store carries the slot indices into the prim's vertices)
P_TAG.pgxp_index  (already a field; stamped by addPrim)
        ↓
MakeVertexQuad/Tri/Rect (PsyX_GPU.cpp)
  └─ if g_PsxUsePgxp && prim has valid cache slots:
        write GrVertex.px/py/pw = fSX/fSY/fW   (NEW float fields)
     else (2D prim / hand-emitted / cache miss):
        write px/py = integer x/y, pw = 0  → shader treats as affine 2D
        ↓
Vertex shader (PsyX_render.cpp)
  └─ if u_pgxpEnabled && pw > 0:
        gl_Position = vec4(ndc(px,py) * pw, depth_ndc * pw, pw)   ← perspective-correct
     else:
        gl_Position = Projection * vec4(px,py, z, 1.0)            ← current affine path
```

The GPU's built-in perspective-correct varying interpolation then does the rest
(UV/color divided by W, interpolated, multiplied back) — no fragment-shader
changes needed for the texture-swim fix. Depth precision comes for free because
`gl_Position.z` is now derived from a continuous float, not quantized SZ.

---

## 5. Detailed work breakdown

### Phase A — GTE float-precision compute (`PsyX_GTE.cpp`)
- In `GTE_RotTransPers`, after the integer SX/SY, compute `fSX,fSY,fW` in
  double/float without truncation or `Lm_G1` clamp. Reference DuckStation
  `GTE_RTPS` for the exact divide ordering.
- Add float-twin registers `g_pgxpSXY[3]` (ring of last 3, mirroring
  `SXY0/1/2`) + `g_pgxpSZ`. Shift them in lockstep with `C2_SXY0/1/2`.
- Cost: a handful of float ops per vertex per `rtps`/`rtpt`. Negligible.

### Phase B — precise-vertex cache + prim plumbing
- Small fixed ring buffer `s_pgxpCache[N]` of `{float px,py,pw; u32 stamp}`.
- `GTE_RotTransPers` writes the newest vertex to the ring, returns/stores its
  index in the SXY lane that the `gte_stsxy*` store reads.
- The game-side `gte_stsxy3_g3` / `gte_stsxy*` macros (already custom for us in
  `inline_c.h` / `inline_no_dmpsx.h`) carry the per-vertex cache index into the
  prim alongside the integer XY. `addPrim` already stamps `P_TAG.pgxp_index`
  (field exists). Wire index → cache lookup.
- Stamp a frame/seq counter so a stale index from a previous frame is detected
  as a **miss** (→ affine fallback, never NaN).

### Phase C — `GrVertex` + attribute upload (`PsyX_render.h`, `PsyX_GPU.cpp`, `PsyX_render.cpp`)
- Extend `GrVertex` with `float px, py, pw;` (precise screen X/Y + W). (Struct
  grows 12 B; acceptable. Alternatively pack into the unused `a_extra.zw` /
  widen `a_zw` to a real vec4 fed from these — decide at impl time.)
- `MakeVertexQuad/Triangle/Rect/LineArray`: populate px/py/pw from the cache
  when `g_PsxUsePgxp` and the prim has valid slots; else affine fallback values.
- Bind a new vertex attribute (or repurpose `a_zw` as a true vec4) and
  `glVertexAttribPointer` it.

### Phase D — vertex shader (`PsyX_render.cpp`)
- Replace the single `GTE_PERSPECTIVE_CORRECTION` line with a runtime branch on
  `u_pgxpEnabled` + `pw > 0`:
  - PGXP: build clip-space `gl_Position` from precise `px,py`, `depth`, and `pw`
    so the GPU interpolates perspective-correctly.
  - affine: keep today's `Projection * vec4(a_position.xy, a_zw.x, 1.0)`.
- Keep `v_is3d` logic. No fragment-shader change required for the core fix.

### Phase E — fallback / robustness (the part the old attempt skipped)
- 2D HUD/UI prims, screen-fade tiles, warning-screen / `libgs_stub` hand-emitted
  prims: ensure they hit the affine branch (pw=0). This is what caused NaN
  explosions last time.
- Cache-miss → affine, never garbage.
- Verify on the known stress maps: map0_s02 store shelves (Z-fight), big floors
  (affine swim), HUD/inventory (must stay flat), cutscene letterbox.

### Phase F — config / launcher
- **Already done.** `use_pgxp` toggle + `g_PsxUsePgxp` + `u_pgxpEnabled` uniform
  already runtime-switch. No separate exe, no rebuild to toggle.

---

## 6. Separate executable? — recommendation: **NO**

Not needed and not worth the maintenance. The compile-time `#if USE_PGXP`
branches that originally forced a second exe are gone; everything routes through
the `g_PsxUsePgxp` runtime flag and `u_pgxpEnabled` uniform. One binary, config
toggle. (We can delete the vestigial `USE_PGXP` macro to avoid future confusion,
or keep it pinned at 1 once PGXP code is unconditionally compiled in and
runtime-gated.)

---

## 7. Configuration options (pick one — A recommended)

- **Option A — config toggle + launcher radio (RECOMMENDED).** Already wired.
  `use_pgxp = 0/1` in `config.cfg`; launcher radio writes it. Applied at boot.
  Zero extra work, single exe. Default OFF so the PSX-faithful affine look is
  the default and PGXP is opt-in.
- **Option B — in-game options-menu toggle (hot-swap).** Nicer UX (flip during
  play). Requires the runtime path to switch shader uniform + vertex population
  live (cheap, since both paths are runtime) and a menu entry. Add on top of A
  later if desired.
- **Option C — separate `SilentHillPC_PGXP.exe`.** Not recommended; obsolete
  given the runtime gating. Only revisit if a future perf concern makes us want
  PGXP code fully `#if`-compiled out of the default binary.

Default-OFF in all options (PGXP changes the aesthetic; some prefer authentic
PSX warping).

---

## 8. Risks & mitigations
- **NaN/garbage from un-cached prims** (the prior failure): mitigated by Phase E
  strict affine fallback on miss/2D.
- **W formula tuning**: the exact `pw` scaling needs empirical calibration vs a
  reference (DuckStation side-by-side). Budget iteration here.
- **Perf**: per-vertex float compute in `GTE_RotTransPers` + one extra attribute.
  Expected negligible at our geometry counts (~36k polys/frame seen in logs).
- **Map DLLs**: unaffected — no PGXP dependency in map code. Both paths share
  DLLs; no DLL rebuild needed to toggle.

## 9. Files that will change (no new disc data)
- `pc_port/PsyCross/src/gte/PsyX_GTE.cpp` — float compute + twins (Phase A)
- `pc_port/PsyCross/src/gpu/PsyX_GPU.cpp` — cache + MakeVertex* population (B,C)
- `pc_port/PsyCross/include/PsyX/PsyX_render.h` — `GrVertex` fields (C)
- `pc_port/PsyCross/src/render/PsyX_render.cpp` — attribute bind + shader (C,D)
- `pc_port/include/psyq/inline_c.h`, `pc_port/include/inline_no_dmpsx.h` —
  carry cache index through `gte_stsxy*` (B)
- (config/launcher: already done)

## 10. Estimated effort
Phase A+B (compute + plumbing): the bulk. C+D (struct/shader): moderate. E
(fallback hardening + map testing): the long tail (iterate with captures).
Single focused implementation pass to a first visible result, then calibration.
