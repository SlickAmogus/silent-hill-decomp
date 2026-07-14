# Texture Residency + Custom Textures (PNG) — Task Spec

> **Status — Audit/status report.** Current source has resident textures and texture packs enabled by default; loose TIM/PNG overrides remain opt-in and experimental. Pre-implementation statements below are historical. See the [feature catalog](../../features.md) and [documentation index](README.md).

Status: **IMPLEMENTED 2026-07-08 — awaiting in-game verification.**
- Phase 0 landed: game `b08927245` + PsyCross `7ffe8b9` (PNG/TIM overrides render; stb_image
  vendored; PNG discovery; draw-time wiring with u_texOffset + alpha discard).
- Phase 1 landed as the **expanded-pool variant of Route B** (see §10 addendum at the bottom):
  virtual pool slots with synthetic bit-15 CLUT keys backed by persistent per-slot GL textures;
  interior keep-4/steal machinery bypassed; config `resident_textures` (default 1, 0 = exact
  old behavior). Covers BOTH map classes (exterior APU rainbow class included).

Prepared 2026-07-08.
Supersedes/absorbs the scope of `project_vram_pool_removal_task` (memory) and folds in
custom-texture (PNG) loading. Read alongside memory `[[project_color_banding_clut]]`,
`[[project_vram_pool_removal_task]]`, `[[project_graphics_options]]`.

---

## 0. TL;DR

Two goals that share ONE foundation:

1. **Kill the VRAM streaming limit** so a whole map's textures stay resident — this is the
   root of the recurring interior **flat-texture / rainbow** bugs (undersized chunk-texture
   pool → the steal/evict loop).
2. **Custom textures (PNG + TIM) via loose files** — when `allow_loose_files = 1`, load a
   modder's replacement texture instead of the disc one.

Both are solved cleanly by the **per-material persistent GL texture** design (Route B below).
There is **already partial infrastructure** for #2 in the tree (`hires_override.c`, the
PsyCross `overrideTexture` hook, `allow_loose_files`, fsqueue `[LOOSE/HIRES]` registration) —
but the draw-time lookup is **unwired**, so registered overrides never draw.

**PR #38 + PsyCross#5** attempt to wire this up and add PNG. The *idea* is sound and the real
code is small, but **the PR branch is broken** (diffs the entire tree as "added" — see §5) and
must **not** be merged. Salvage the pieces, re-implement clean on `pc-port`.

---

## 1. Why (the problem)

The recurring interior "goes flat / rainbow" bugs are all symptoms of one thing: **a map's
textures don't fit in the emulated 1 MB PSX VRAM**, so the engine streams and *steals* pages.

- PsyCross emulates VRAM as **one 1024×512 GL texture** `g_vramTexture` (`PsyX_render.h`
  `VRAM_WIDTH/HEIGHT`). Every textured prim samples it (`PsyX_GPU.cpp:1404`
  `textureId = g_vramTexture`); TIMs upload into it via `LoadImage`→`glTexSubImage` at their
  tpage coords. Addressing is PSX-format: tpage X bits0-3 (×64), tpage Y 1 bit (×256) →
  1024×512; clut Y 9 bits (0-511, capped at `VRAM_HEIGHT`, ~`PsyX_GPU.cpp:2007`).
- Interior chunk-texture **pool** = `g_Map.chunkTextures.fullPageTextures[8]` +
  `halfPageTextures[2]` (`terrain.h:51-52`), placed in leftover VRAM rows by `Ipd_TexturesInit`
  (`bodyprog_80040B74.c:528`, tpage Y 8-10 + 21-27). Maps hold ~16 resident chunks but only 10
  pages → the `Ipd_MaterialsLoad` **steal loop** (`bodyprog_80040B74.c:~1721`) evicts farther
  chunks → flat/rainbow.
- **Stopgap already shipped** (`42623ad88`): interior sync now force-restores a material's page
  when a stolen slot returns, so prims no longer get stuck flat. Buys time; does not remove the
  limit.

A modern PC has no reason to stream. Make the whole map resident and the entire bug class,
plus the steal/evict machinery, disappears.

---

## 2. Current architecture (verified 2026-07-08)

### VRAM / texture path
- Single `g_vramTexture` emulation (above). No per-TIM GL handles today.
- The **`overrideTexture` hook already exists** in PsyCross (`PsyX_GPU.cpp`):
  - `overrideTexture / overrideTextureWidth / overrideTextureHeight` globals (`450-452`).
  - In `AddSplit` (`1406-1410`): `if (textured && overrideTexture != 0)` → force
    `texFormat = TF_32_BIT_RGBA`, `textureId = overrideTexture` (the direct-sample RGBA shader,
    already used for `DR_PSYX_TEX`). `textureId` is part of the split key, so batches open/close
    at matching prims — no batching rework needed.
  - Fed today only from a `DR_PSYX_TEX` prim (`~2428`, `overrideTexture = psytex->code[0]…`).
    **Nothing emits that packet for hi-res overrides**, so the path is dormant.

### Custom-texture infrastructure (already in tree, partial)
- `pc_port/src/hires_override.c` (+ `hires_override.h`): registers a loose **TIM** as a
  full-res RGBA8 GL texture, keyed by `(vramX,vramY,vramW,vramH, clutX,clutY, bitDepth)`.
  - `HiresOverride_RegisterFromTim(...)` — parses TIM (4/8/16/24bpp), uploads RGBA8 GL texture.
  - `HiresOverride_LookupByTpageClut(tpage, clut, *outNativeW, *outNativeH)` — matches the
    incoming draw's tpage/clut to a registered override; returns the GL texture. **Has no
    callers** → overrides never render.
- `pc_port/src/pc_config.c`: `allow_loose_files` (`g_PcConfig.allowLooseFiles`, default 0) —
  "scan `gamedata/load/{folder}/{name}` before CD read".
- `src/main/fsqueue_3.c`: loose-file interception. Logs `[LOOSE/INIT|LOOSE|LOOSE/HIRES|
  LOOSE/MISS|LOOSE/WARN|LOOSE/SUMMARY]`. Byte-replaces small files; defers oversized ("hi-res")
  TIMs to `PostLoadTim` → `HiresOverride_RegisterFromTim`. `SH_LOOSE_VERBOSE=1` logs every miss.

**Net:** the plumbing to *register* loose overrides exists; the plumbing to *draw* them and to
accept *PNG* input does not.

---

## 3. Two routes for VRAM residency

### Route A — enlarge emulated VRAM + extend addressing (pragmatic, medium)
Bump `VRAM_HEIGHT` (e.g. 512→2048), grow `fullPageTextures[]/halfPageTextures[]`, place new
pages in the extra rows. GL handles a bigger texture trivially.
- **Catch:** tpage Y is 1 bit / clut Y 9 bits → can't address past 512 rows in PSX format. The
  tpage/clut encode/decode must be **widened on the PC path** (game `field_E → prim field_6_0`;
  PsyCross tpage→UV + clut→coord decode). This is *exactly* the tpage/clut class that produced
  the rainbow — do it very carefully.
- Gets "whole map resident" with the least rearchitecture; keeps the single-VRAM model.

### Route B — per-material persistent GL textures (clean end-state, larger) — RECOMMENDED
Give each chunk/material its **own** GL texture, uploaded once at load, never evicted; the draw
path binds the material's texture (its own UV space + 4bpp/CLUT decode) instead of sampling
shared VRAM.
- Kills pool/steal/eviction entirely; unlimited residency.
- Reuses the existing `overrideTexture`/`TF_32_BIT_RGBA` delivery mechanism (that's already a
  per-prim "bind this RGBA texture" path — generalize it from "hi-res override only" to "every
  material").
- **This is also the custom-texture foundation:** once a material owns a GL texture, a custom
  PNG is just "fill this material's texture from the PNG instead of decoding the TIM," and the
  **multi-tpage limitation disappears** (each material has its own UV space — no tpage packing).

**Recommendation:** Route B. It's the PC-native end-state and it *unifies* both goals. Route A
is the faster stopgap-to-residency but leaves the tpage/clut fragility and doesn't fix custom
world textures. Either is a project, not a patch. **Keep the streaming loader (disc→IPD)** in
both — only texture *residency* changes, not *loading*.

---

## 4. Custom textures (PNG) scope

Goal: with `allow_loose_files = 1`, `gamedata/load/<DISC_FOLDER>/<DISC_NAME>.png` (or `.TIM`)
replaces the disc texture. Any resolution — native UVs map 0..1 over the original, so 2×/4×/8×
upscales "just work."

Pieces required (small, independent of the VRAM route):
1. **PNG decode** — vendor `stb_image.h` (public domain, header-only; `STBI_ONLY_PNG` +
   `STBI_NO_STDIO`) into `pc_port/include/`. In `hires_override.c`, sniff the PNG magic in
   `RegisterFromTim` and decode to RGBA8 (keep TIM working). Alpha semantics: PNG has a real
   8-bit alpha; `0 = hole`, `~128 = STP-style 50% blend on semi-transparent prims`, `255 =
   opaque`. (TIMs stay 1-bit: colour-0 = transparent.)
2. **Draw-time wiring (PsyCross)** — call `HiresOverride_LookupByTpageClut(tpage, clut)` before
   `AddSplit` in the poly/sprite handlers and set `overrideTexture` + `overrideTextureWidth/
   Height` (= the **original** TIM's native pixel size, so UVs map 0..1 over the replacement).
   Reset in `ClearSplits()` so it can't leak across frames. Make the lookup a **weak stub** so
   non-SH hosts link unchanged. Add `if (fragColor.a < 0.5) discard;` to `gte_shader_32_rgba`
   for colour-0/alpha transparency (opaque prims ignore GL blending; soft alpha ≥ 0.5 still
   blends on semi-transparent prims). — **This is exactly PsyCross#5 (92 lines); port it.**
3. **PNG discovery (fsqueue_3.c)** — a loose `<discname>.png` always registers as an override
   (never a byte-replace), regardless of size; the disc file still loads so the engine picks the
   native VRAM rect, then `PostLoadTim` registers the PNG against it. `.png` takes precedence
   over a same-name loose `.TIM`.

**Known limitation of the tpage/clut-keyed override (v1):** a surface wider than one tpage (e.g.
a 320px background drawn as two tpages, and — critically — packed world/interior atlases) samples
the whole override per prim → wrong region. So the override path alone covers **single-tpage
assets** (items, HUD, sprites, character textures) but **not world/interior** — which are the
VRAM-task surfaces. Route B removes this limitation for free (per-material UV space). Sequence
accordingly (§6).

---

## 5. PR #38 assessment (SlickAmogus/silent-hill-decomp#38 + PsyCross#5)

**Verdict: sound idea, do NOT merge the branch. Re-implement the 3 real pieces clean.**

What it intends (correct and matches §4):
- `stb_image.h` PNG decode in `hires_override.c`; PNG discovery in `fsqueue_3.c`; PsyCross#5 wires
  the dormant `overrideTexture` path from `HiresOverride_LookupByTpageClut` + adds the alpha
  discard. PsyCross#5 is a clean **+92/-4** change and is genuinely good — port it (or re-derive
  it; it's small).

Why it's "screwed up" (what you sensed):
- The PR branch is diffed against the **wrong/old base**: **all 2,117 files show as `added`**
  (~376K+ line additions). Core decomp files come through as brand-new:
  `player_control.c` +11,619 **added**, `bodyprog_8005E0DC.c` +2,951 **added**,
  `hires_override.c` +341 **added**, etc.
- Consequence 1: **unmergeable / unreviewable** — GitHub can't even render the diff (>300 files).
- Consequence 2 (**hazard**): merging would **overwrite our current files with the author's
  divergent copies** — including `bodyprog_8005E0DC.c` (today's muzzle-shadow fix `ef6dfd065`),
  `player_control.c` (all the free-aim / alt-cam work), etc. It would silently revert months of
  work. Treat the PR as reference only.
- Consequence 3: the actual PNG/discovery deltas are buried and can't be cherry-picked as-is;
  they must be re-applied by hand onto current `hires_override.c` / `fsqueue_3.c`.

Its functional ceiling (even if cleanly landed): single-tpage assets only (the multi-tpage
limitation in §4). It does **not** address VRAM residency or custom **world** textures.

**Salvage plan:** re-vendor `stb_image.h`; hand-port the PNG sniff/decode into our
`hires_override.c`; hand-port the `.png` discovery into our `fsqueue_3.c`; port PsyCross#5's 92
lines. All small, all on top of current `pc-port`. Verify byte-identical output with
`allow_loose_files = 0`.

---

## 6. Recommended sequencing for the session

**Phase 0 — Custom textures for single-tpage assets (small, low-risk, ship first).**
Salvage PR#38's three pieces (§4.1–4.3) clean on `pc-port`. Delivers modder PNG/TIM replacement
for items, HUD, sprites, character textures immediately. Gate: `allow_loose_files`. Must be
**byte-identical when off** (weak stub, no override registered → zero behaviour change). Verify
with the boot-logo TIM (the PR's own test) + a character texture.

**Phase 1 — VRAM residency rework (the main event), Route B recommended.**
Move chunk/material textures to per-material persistent GL textures; delete the pool/steal/evict
path; bind per-material in the draw path (generalize `overrideTexture`). Fixes flat/rainbow for
good. Then extend custom textures to **world/interior** surfaces on the same per-material path
(removes the multi-tpage limitation). Keep disc→IPD streaming *loading*; change only *residency*.
Consider Route A only if Route B proves too invasive for the timebox — but A leaves the tpage/clut
fragility and doesn't unlock custom world textures.

Land Phase 0 and Phase 1 as separate reviewable steps. Each must be byte-identical with its
feature off.

---

## 7. Key files

Game side:
- `pc_port/src/hires_override.c` / `pc_port/include/hires_override.h` — override registry + decode.
- `src/main/fsqueue_3.c` — loose-file interception + `PostLoadTim` registration.
- `pc_port/src/pc_config.c` / `pc_port/include/pc_config.h` — `allow_loose_files`.
- `src/bodyprog/gfx/bodyprog_80040B74.c` — `Ipd_TexturesInit` (`~528`), `Ipd_MaterialsLoad` steal
  loop (`~1721`); the interior chunk-texture pool.
- `src/bodyprog/gfx/terrain.h` (`~51-52`) — `chunkTextures.fullPageTextures[8]/halfPageTextures[2]`.

PsyCross side:
- `src/gpu/PsyX_GPU.cpp` — `g_vramTexture`, `AddSplit` (`~1400`), `overrideTexture` path
  (`450-452`, `1406-1410`, `2428`), `ClearSplits`.
- `src/render/PsyX_render.h` — `VRAM_WIDTH/HEIGHT`, `TexFormat`, `BlendMode`.
- `src/render/PsyX_render.cpp` — shaders incl. `gte_shader_32_rgba` (add the alpha discard here).

External refs:
- PR SlickAmogus/silent-hill-decomp#38 (game side — reference only, do not merge).
- PR SlickAmogus/PsyCross#5 (`+92/-4`, the draw-time wiring — port/re-derive).
- `stb_image.h` (public domain PNG decoder to vendor).

---

## 8. Gotchas / discipline

- **Byte-identical when off.** Both features must produce byte-identical frames with
  `allow_loose_files = 0` and (for residency) any new path disabled. This is the port's standard;
  verify, don't assume.
- **The tpage/clut rainbow class.** Any change to VRAM height or tpage/clut encode/decode is the
  exact class that caused the interior rainbow (`[[project_color_banding_clut]]`). If you touch
  addressing (Route A), expect it and test interiors hard.
- **Keep the disc→IPD streaming loader.** Only residency changes. Don't turn `gamedata/load/`
  into a runtime disc-extract path (see the XA note — loose files are an *override*, not the
  asset source).
- **UV mapping:** native PSX UVs (0..`vramSpanInPixels`) map to 0..1 of the replacement, so any
  uniform upscale works; feed `overrideTextureWidth/Height` the **original** native pixel size.
- **Alpha:** PNG 8-bit alpha — `<0.5 discard` for holes; ≥0.5 soft alpha blends on
  semi-transparent prims (SRC_ALPHA = STP-style 50%). TIM stays 1-bit (colour-0 transparent).
- **Do NOT merge PR#38** — it will clobber current work (§5). Reference only.
- **Census the SH_PC_PORT blocks** if you ever rebase against upstream during this
  (`[[feedback_upstream_merge_census]]`).
- The stopgap (`42623ad88`) can stay until Phase 1 removes the pool; don't rip it out early.

---

## 9. Open design questions (decide in-session)

1. Route A vs B for Phase 1 — recommend B; confirm against the timebox.
2. Texture-pack layout: mirror the disc folder/name (`gamedata/load/<FOLDER>/<NAME>.png`) — keep
   the existing convention; document it.
3. Filtering/mips for upscaled PNGs (currently `GL_LINEAR`, `CLAMP_TO_EDGE`, no mips). Mips would
   help minified world textures — decide once per-material exists.
4. Override table capacity: `MAX_HIRES_OVERRIDES = 256` today — fine for single-tpage assets;
   Route B makes it per-material (revisit sizing).
5. How custom **world** textures are keyed once materials own textures (material id vs tpage/clut).
6. Optional follow-up the PR floated: a dump/upscale toolchain for authoring packs. Out of scope
   for the core task; note it.

---

## 10. Addendum — what was actually built (2026-07-08)

Phase 1 shipped as an **expanded-pool variant of Route B**, informed by a 9-agent code survey
(key findings below). Design:

- `terrain.h`: pool grows by `PC_TEXPOOL_FULL_EXTRA` (128) + `PC_TEXPOOL_HALF_EXTRA` (48)
  VIRTUAL slots appended after the 10 physical ones. Claim order = list order, so physical
  slots fill first: assignment is vanilla-identical until vanilla capacity is exceeded.
- Virtual slot identity: real pool tpage byte (harmless VRAM fallback) + synthetic CLUT
  `clutY = 512 + slotId` → prim clut halfword bit 15 set — no valid PSX prim carries that, so
  the (tpage, clut) override key cannot collide. O(1) lookup fast path in
  `HiresOverride_LookupByTpageClut` (`slotId = ((clut>>6) & 0x3FF) - 512`).
- `Fs_QueuePostLoadTim`: `clutY >= 512` ⇒ skip both VRAM `LoadImage`s (the rect aliases a
  physical page) and decode the TIM — or a loose PNG/TIM replacement, which takes precedence —
  into the slot's persistent GL texture (`HiresOverride_PoolSlotRegister`, replace-in-place on
  slot reuse). Native-res decodes sample NEAREST; resolution-changed replacements LINEAR.
  **This removes the multi-tpage/custom-world-texture limitation: interior + exterior terrain
  textures are moddable per material TIM.**
- `Ipd_ChunkMaterialsApply`: interiors texture EVERY loaded chunk (whole map resident);
  keep-4 + steal + `g_PcInteriorMatSync` shims remain only as the `resident_textures=0` path.
  Exteriors keep the vanilla distance loop; expanded capacity alone fixes the APU
  cross-area collision class.
- PsyCross: FT4 clutY>511 drop guard exempts prims the override table claims (map code that
  bakes pool tpage/clut into its own prims, e.g. map4_s03 Texture_InfoGet consumers).

Survey facts that shaped it (full reports in the session workflow journal):
- Prim tpage/clut have exactly ONE game-side writer (`Model_MaterialFlagsApply`) and are baked
  into persistent chunk prim buffers, re-applied only on change — so keys must be stable, and
  the per-prim override lookup (Phase 0) is the correct bind point.
- At `PostLoadTim` the whole TIM (pixels + CLUT + depth + target rects) is in one live CPU
  buffer (`FS_BUFFER_9`), reused by the next read — decode-once is possible exactly there,
  never later. Steal-and-return always re-streams from disc; no CPU copy exists.
- Interior/exterior is a STREAMING class (`MapFlag_Interior`), not geography; both share one
  pool/allocator/draw gate. Global LM pins physical slots 0-3 (count=4 clamp). 2D events and
  map FX deliberately upload into pool pages (tpage 21 etc.) — physical slots must keep
  uploading to VRAM; virtual slots make terrain immune to those stomps.
- Interior material CLUTs are static per TIM; the lit-path clut offset (`field_14C`) is
  provably 0 for terrain (all `Ipd_ChunkDraw` call sites pass arg5=0). Palette dynamics exist
  only on the character path — correctly out of scope.
- Pool CLUT columns live at VRAM row 0 x=0..160; tpage 27 is Harry's texture (spec's "21-27"
  was off by one; both half slots share tpage 26).

Known limits / follow-ups:
- Pool exhaustion (>136 full / >50 half distinct TIMs in one map) logs `[POOLTEX] pool
  exhausted`; affected chunks stay undrawn like vanilla out-of-pool chunks. Bump the
  constants if any map ever trips it.
- Materials on the 10 physical slots still sample VRAM (vanilla): the first ~8 full-page TIM
  names per map are not GL-resident and keep vanilla stomp/steal exposure (exterior distance
  churn only; interiors no longer steal). A follow-up could claim virtual slots first — needs
  a decision on global-LM/`Texture_InfoGet` interactions.
- Hi-res rect-table entries registered against a physical pool page go stale if the engine
  reuses that slot for a different TIM (pre-existing Phase 0 limitation; exteriors only).
