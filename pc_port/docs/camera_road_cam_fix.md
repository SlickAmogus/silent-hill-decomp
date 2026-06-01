# Road/Chase Camera Mis-Framing — Root Cause & Fix

## Symptom

For months the PC port's fixed/chase cameras (streets, alleys) framed Harry
wrong: facing one direction the shot looked correct, facing the opposite
direction Harry's body dropped out of frame and the camera showed mostly sky.
At the very first spot in the game (opening foggy street, Harry facing −Z) the
raw camera pitched up into the treetops.

It was worked around for a long time with a hand-tuned `s_camCorrections` table
in `src/bodyprog/sys/game_main.c` — per-spot pos/lookAt/pitch nudges applied as
a post-pass on the final camera matrix. The corrections varied wildly from spot
to spot and never got it perfectly 1:1 with the original game, which was the
tell that they were papering over a single systematic error rather than fixing
it.

## Root cause

`Math_RotMatrixZxyNeg` in `pc_port/src/math_impl.c` (a hand-written PC
reimplementation of the PSX rotation-matrix builder) produced the **wrong
matrix**. It built the *transposed* element layout with **all three angles
negated**, which yields:

```
m[1][2] = sy*sz + cy*sx*cz      (PC, wrong)
```

The matrix the rest of the engine expects (and that `vwMatrixToAngleYXZ` is the
exact inverse of) is the standard `Ry * Rx * Rz`:

```
m[1][2] = -sx                   (correct)
```

`-sx` depends only on pitch. The buggy `sy*sz + cy*sx*cz` **entangles pitch with
yaw**: it equals `-sx` only when `cos(yaw) = 1`, and it **flips the pitch sign**
when `cos(yaw) = -1` — i.e. when the camera faces −Z. So:

- Facing +Z: camera correct.
- Facing −Z: camera pitch inverted → it looks up at the sky instead of down at
  Harry.

This is also why it looked camera-specific and hid for so long: **character and
object models sit at pitch ≈ 0**, where the bad `cy*sx*cz` term vanishes, so
models rendered fine. Only the camera view matrix carries a large pitch, so only
the camera visibly broke — and only at −Z-facing shots.

`Math_RotMatrixZxyNeg` is shared by ~19 call sites (camera, `Vw_SetLookAtMatrix`,
map-object and item rendering), so the fix corrects the whole engine, matching
PSX everywhere; the camera is just where it was most visible.

## Fix

Rewrite `Math_RotMatrixZxyNeg` to build the standard `Ry * Rx * Rz` matrix with
non-negated angles (commit `17a3eb5cb`):

```c
mat->m[0][0] =  cy*cz + sy*sx*sz;   mat->m[0][1] = -cy*sz + sy*sx*cz;   mat->m[0][2] = sy*cx;
mat->m[1][0] =  cx*sz;              mat->m[1][1] =  cx*cz;              mat->m[1][2] = -sx;
mat->m[2][0] = -sy*cz + cy*sx*sz;   mat->m[2][1] =  sy*sz + cy*sx*cz;   mat->m[2][2] = cy*cx;
```

(`sx = sin(rx)`, `cx = cos(rx)`, etc., NOT negated.)

A separate, also-real fix preceded it (commit `92a55f647`): the PC build had
disabled the road-height clamp (`lim_rd_min_hy`/`lim_rd_max_hy`) for WIDE/HUGE
road areas in `vcMakeIdealCamPosUseVC_ROAD_DATA`, so the camera rested ~0.4 m too
low. That clamp was restored to its faithful PSX form. Both fixes are needed.

## How it was found (forensic method)

Real-PSX (USA) reference values were read out of RAM in DuckStation's debugger
at the exact opening spot (Harry unmoved, camera settled) and compared to the PC
port **layer by layer down the camera pipeline**. Wherever the numbers first
diverged is where the bug was.

Addresses (USA), from `configs/USA/sym.bodyprog.txt` (`vcWork = 0x800B9CD0`) plus
`VC_WORK` field offsets in `include/bodyprog/view/structs.h`:

| field | offset | address |
|-------|--------|---------|
| `cam_pos`        | +0x50 | `0x800B9D20` |
| `watch_tgt_pos`  | +0x7C | `0x800B9D4C` |
| `cam_mat_ang`    | +0x8E | `0x800B9D5E` |
| `ofs_cam_ang`    | +0xB8 | `0x800B9D88` |
| `base_cam_ang`   | +0xC8 | `0x800B9D98` |
| `cam_mat`        | +0x98 | `0x800B9D68` |
| Harry position (`g_SysWork.playerWork.player.position`) | 0x800B9FC0 +0x4C +0x18 | `0x800BA024` |

Values are little-endian; positions are signed 32-bit Q19.12 (÷4096 = metres),
angles are signed 16-bit (4096 = 360°). To read a 16-bit field out of a 32-bit
word, split the word into low/high halves and treat values ≥ 0x8000 as negative
(`v − 0x10000`).

Pipeline: `cam_pos`+`watch_tgt_pos` → `ofs_cam_ang` → `Math_RotMatrixZxyNeg`
(build matrix) → `vwMatrixToAngleYXZ` (read back) → render.

1. **Pose** (`cam_pos`, `watch_tgt_pos`): PSX cam.y −10227 / look-at.y 2048
   **matched** PC exactly. So the camera was positioned and aimed like PSX, yet
   rendered differently → the bug is downstream of the pose.
2. **Final render angles** (`cam_mat_ang`): pitch was **+266 on PC vs −256 on
   PSX** — same magnitude, opposite sign (down vs up). Bug = inverted pitch in
   the pose→matrix conversion.
3. **Input angle** (`ofs_cam_ang`): **−266 PC vs −255 PSX — matched.** So the
   angle entering the matrix builder was correct; the inversion happened inside
   the build/read-back, i.e. between `Math_RotMatrixZxyNeg` and
   `vwMatrixToAngleYXZ`.
4. **Round-trip test:** angle→matrix→angle should be identity. PSX: −255 in,
   −256 out (identity). PC: −266 in, +266 out (flipped). `vwMatrixToAngleYXZ` is
   untouched decomp; `Math_RotMatrixZxyNeg` is the PC reimpl → the builder was
   wrong.
5. **Confirmation:** read the real PSX `cam_mat` (all 9 elements) and compared to
   the corrected `Ry*Rx*Rz` formula computed for the same `ofs`. All 9 matched
   within 1–2 units of rounding (notably `m[1][2] = +1562` PSX vs `−1626` from
   the buggy PC build).

Lesson: don't reason about rotation sign/handedness conventions in the abstract —
read the hardware's actual matrix and match it numerically.

## Follow-up: in-place TransposeMatrix (SETTLE-mode cameras)

After the matrix fix, fixed/overhead cameras in `SETTLE` mode (e.g. the alley
shot where the camera sits in a corner and Harry walks underneath) still
pitched up into the sky. Root cause: `TransposeMatrix` (`pc_port/src/stubs/
libgs_stub.c`) wrote each element directly from `m0`, with no temporary, so when
called **in-place** (`m0 == m1`) it overwrote the off-diagonal source elements
before reading them — producing a non-rotation matrix. `vcRenewalCamMatAng` does
exactly this:

```c
Math_RotMatrixZxyNeg(&base_cam_ang, &base_matT);
TransposeMatrix(&base_matT, &base_matT);   // in-place → corrupted
```

The corrupted `base_matT` fed `ApplyMatrixSV` in `vcMakeOfsCamTgtAng`, so the
offset angle came out wrong (alley cam: `ofs_cam_ang.y ≈ 1108` / 97° instead of
~393), and the composed view pitched up. **CHASE cams (including the opening
street) were immune** because their `base = (0,0,0)` makes `base_matT` the
identity, whose in-place transpose is harmless — which is why the matrix fix
above corrected almost everything but these `base ≠ 0` shots stayed broken.

Verified with the same RAM-compare method at a position-matched alley spot: PSX
`ofs_cam_ang = (99,321)` matched the offset recomputed from `base`+`watch`
(`88,312`), but the port's `(117,1108)` did not — isolating the bad `base_matT`.

Fix (commit `42973c70e`): copy `m0` into a temp before writing, so in-place use
is correct.

## Status / cleanup (done)

- Root cause fixed in `Math_RotMatrixZxyNeg`; in-place `TransposeMatrix` fixed;
  road cam-height clamp restored. Verified 1:1 with PSX in-game.
- The `s_camCorrections` table + `struct CamCorrection` + its matching/smoothing
  logic and the `g_PcRoadCamCorrections` gate were **deleted** — the engine's own
  PSX-faithful camera is correct, so the band-aids are obsolete. The `[CAMPITCH]`
  / `[CAMMAT]` debug traces were removed too.
- **Kept** as live-tuning tools: the manual numpad nudge + BAD/GOOD position
  logging (`[…-DELTA]` lines log raw nudge values), and KP_0 raw-cam mode.

## Known minor follow-up

One `VC_MV_FIX_ANG` shot (alley3 "it's getting darker" line) frames Harry
slightly off — angle/FOV/height all match PSX, only the camera XZ position is
~2.4m off, traced to `offset_dist` (Harry's distance to the road limit box) in
`vcMakeIdealCamPosForFixAngCam`, i.e. the runtime `cur_near_road.rd` box. Minor,
self-resolves on moving, unrelated to the two engine bugs above. To be chased
with the same RAM-compare method.
