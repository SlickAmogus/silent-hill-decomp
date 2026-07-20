# Oversized Character Models — Staged Plan

Goal: loose replacement models with MORE geometry than the original, the way the
texture system supports larger-than-original textures. Research + adversarial
audit completed 2026-07-20 (every claim re-verified against source; file:line
references below are from that pass).

## Verdict: staged. Stage 1 = minimal headroom + converter grow-mode. Stage 2 = sidecar "ILMX" only if evidence demands. Banked pool windows REJECTED; HD skinned renderer deferred indefinitely.

## Stage 1 — IMPLEMENTED 2026-07-20, AWAITING IN-GAME TEST

Shipped: (1) `SH_PC_PORT`-guarded pool widening in `bodyprog.h` (258 slots /
261 shade cap; PSX layout unchanged), (2) the oversized loose-ILM loader
`pc_port/src/pc_big_lm.c` + the `fsqueue_3.c` size-gate lift, (3) the V1–V11
validator `pc_port/src/lm_validate.c`, (4) `--grow` mode in
`pc_port/tools/ilm_obj.py`. Known remaining gap: the C# converter twin has NOT
been updated to match `ilm_obj.py`.

## Experiment 0 — foundation play-test (zero repo changes) — PASSED 2026-07-19

The loose-ILM byte-replace path has never been play-verified, and everything
below sits on it. A probe file is at `gamedata/load/CHARA/HERO.ILM`: pristine
HERO.ILM with part 02HEAD1 vertex 0 moved Y −200 (one byte differs, same size).

- PASS: Harry has a tall spike out of his head that FOLLOWS head animation
  (look up/down, walk, attack). Loose byte-replace log line present, no hang.
- FAIL signatures: infinite load = the Sync-skip contract (`fsqueue_2.c:68-83`);
  no spike = a silent-ignore path; garbage model = fix-up on loose bytes.
- Landmine to keep excluded: ANY texture-pack `CHARA/HERO.png` sets
  `pngOverride=1` and silently disables the ILM byte-replace (`fsqueue_3.c:576-605`).

If Experiment 0 fails, the failure mode picks the loader fix; do that before any
struct work.

## Why the ceilings sit where they do

- Prim corner indices are u8 ABSOLUTE slots into the shared scratch pool: the
  format can address 256 slots; today's pool holds 90 (XY `screenXy_0`,
  `bodyprog.h:300-332`). Slot 255 is unusable as a quad's 4th corner (0xFF =
  triangle sentinel). Stage 1 ceiling ≈ 254 unique verts character-wide (~2.8x),
  unbounded prim/tessellation density on those verts.
- Parts per skeleton: 56 (`Skeleton_Init`, `world_draw.c:1423`), and
  `Skeleton_BoneModelAssign` has NO bound check (`bodyprog_bone_80044F14.c:139-151`)
  — the ≤56 validator rule is the only guard.
- Animatable bones: `activeBones` is a u32 MASK — bone ≥ 32 aliases mod 32
  (`bodyprog_anim_800445A4.c:151,163`). Treat 32 as hard.
- A model MAY have more parts than the ANM has bones if extra parts name an
  EXISTING bone index (works today by design; two-digit name prefix = bone).
- Fixed PSX-RAM slabs are sized from the compiled-in file table, not the actual
  file (`fsqueue_3.c:608`; slabs `fsqueue.h:73-74,129-139`): +1 byte overruns the
  next chara's slot silently. Hence PC-owned mallocs, never grown slabs.

## Stage 1 contents (order matters; ~500-700 lines C, ~300-500 Python)

1. **T8 scratch widening + asserts (FIRST, independently testable).** Audit every
   access to `screenXy_0/screenZ_168/field_18C/field_21C/field_252/field_2B8` in
   `bodyprog_80055028.c`; parameterize both scratch struct variants; PC-only
   `offsetof` pins + `sizeof <= 4096` (`g_PsxScratchpad` is already 4096,
   `psx_memory.c:25`; fix the stale `[1024]` extern in `common.h:31`). PSX builds
   keep current dims via macros (byte-identical decomp).
   **Pool CAPs (audit F1): 258 for XY/Z/normal/fog (256 + 2 slack for the
   3-at-a-time strides), 261 for `field_2B8`** (`func_800574D4:1925-1930` copies
   the shade stream 4 bytes per step). Three in-exe scratch base assignments
   (`bodyprog_80055028.c:1848,3455,3660`) + the stack instance in
   `Gfx_BillboardDraw:4629`; map DLLs declare their own transient structs and do
   not couple (verified across all ~10).
2. **Loader** (`pc_big_lm.c`): loose gate lifts to registered-buffer capacity;
   probe/registry/validation with native fallback + loud `[BIGLM]` logs; redirect
   hooks at `world_draw.c:964/1034/1344`. **Registry keyed by `modelFileIdx`
   resolved AT the :1344 hook, never charaId (audit F3)** — `CHARA_FILE_INFOS` is
   retargeted at runtime (Mumbler CLD3/CLD4 swap `chara_data_info.c:68-92`; pool
   DummyNurse→PRS2). **T6 (audit F4): `WorldGfx_CharaLmBufferAssign`
   (`world_draw.c:1243-1265`) must skip PC-owned pointers, and the :1344
   substitution must be unconditional on the incoming lmHdr** (slot-reuse path
   :1287-1289 hands a previous chara's pointer to the next). Ship the
   bump-pointer patch in the same commit — heap corruption for the next stock
   chara otherwise. Pool sizing from real file at `pc_chara_pool.c:124`.
3. **Validator** (shared with `lm_reformat.c`): per mesh
   `vertexOffset+vertexCount <= 256` (same for normals); shade-stream bounds;
   file arrays followed by ≥8 slack bytes, shade ≥3 (audit F1 supersedes the
   old "%3==0" rule); big-model mallocs +16B tail; quad corner-3 != 0xFF;
   section offsets in file; `modelOrder` in range; ≤56 parts; bone prefix <
   ANM boneCount; **`field_B_4 != 0` parts must have vertexOffset==0 &&
   normalOffset==0 (audit F2** — the third draw chain `func_80059D50:3461`
   ignores window offsets entirely). The walkers stay uncheck­ed; the validator
   IS the safety story.
4. **Converter grow-mode** (`ilm_obj.py` then the C# twin): lift the topology
   refusal into a rewrite path within u8/pool limits — recompute offsets,
   preserve seam slots + foreign-ref ordering, pad per F1, run the same
   validator, print a per-part budget report.

Standalone bugfixes that ship regardless (audit F6: implement BOTH as one
postLoadType gate on the PNG-probe block): the permanent HiresPending slot leak
per oversized loose ILM (`fsqueue_3.c:633`, popped only by TIM post-load) and
the PNG-stem landmine above.

### Stage 1 acceptance tests
1. Stock regression: widened build, no loose files — frames identical across the
   lit chain, unlit chain, third chain, a billboard, and a cutscene tint scene.
2. Same-size replace: Experiment 0 formalized.
3. Grown model: one part subdivided (+30-100 verts) renders correctly, seams do
   not tear during walk/turn/attack, and an UNMODIFIED chara loading after the
   modded one in the same map group is intact (the T6 test). Must include a
   FIRST part (offset-0 → `func_800574D4` path) and a third-chain part
   (`field_B_4 != 0`) — audit addition.
4. Rejection paths: oversized-invalid / bad-window / bad-slack files → `[BIGLM]`
   log + native model, HiresPending count unchanged.
5. Coexistence: texture pack + loose ILM together; two same-charaId NPCs;
   cutscene chara swap; PGXP on/off.

## Stage 2 — sidecar ILMX (u16 indices, effectively unlimited under 56-part/32-bone)

Build ONLY when all hold: Stage 1 shipped and play-verified; a real community
model hits the 254-slot ceiling; the parity harness exists (stock ILM → ILMX →
pixel-identical to native before any grown content); measured per-vertex cost
confirms CPU headroom. Hook in `func_80057090`, bind at ProcessLoad, materials
after `Lm_MaterialFlagsApply`, PGXP PROP registration in the clones.

## Gameplay is geometry-blind (verified)

Aim assist, bullet/melee hits = collision cylinders/boxes only
(`pc_combat.c:527-575`); flashlight shadows operate on submitted GPU prims;
anim system touches `boneCoords` matrices only. Registries hold pointers, never
copies. No gameplay path reads model internals (only `water.c:659-661`
header-level material apply and `chara_spawn.c` pass-through).
