# Graphics Effects — Feasibility Study (PsyCross renderer)

Research note (no implementation). Assesses adding optional graphical effects to the
Silent Hill PC port's PsyCross OpenGL renderer. All paths relative to
`pc_port/PsyCross/` unless noted.

## Renderer baseline — the constraints that decide everything

- **API:** OpenGL **3.3 Core** (requested at `src/render/PsyX_render.cpp:436`, then the
  minor version is walked down until a context is created), SDL2 window/context, GLAD
  loader. GLES path also exists.
- **Output target:** the world + UI draw **straight to the default backbuffer (FBO 0)**,
  which is the SDL-chosen **RGBA8 / 32-bit**. There is **no offscreen scene FBO**, **no
  fullscreen-quad post-process pass**, and **no MSAA requested**
  (`SDL_GL_MULTISAMPLEBUFFERS`/`SAMPLES` are never set).
- **Color:** PSX 15-bit (BGR555) color + a 4×4 ordered **dither** are emulated **in the
  fragment shader** (`PsyX_render.cpp:685-724`), already gated by `g_cfg_psxDither`
  (default 1) + the per-frame `g_PsxDitherSuppressed`. PSX VRAM is a CPU `ushort[1024×512]`
  array uploaded as a `GL_RG32F` texture; the shader does the 5-bit CLUT/texture decode.
  Frame-feedback surfaces that round-trip through VRAM are hard-truncated to 5 bits/channel.
- **Lighting:** computed **game-side per-vertex** on the PSX GTE (`gte_ncds`/`gte_nccs`) and
  **baked into vertex RGB** before the renderer sees it. The shader only ever gets gouraud
  `v_color` — **no per-pixel normals, no view/world position**. (Geometry *does* carry
  per-vertex normals game-side — `bodyprog_80055028.c:3546` — they're just consumed by the
  GTE and discarded.)
- **Depth:** **flat-per-primitive** (one constant ordering-table bucket value per prim,
  `PsyX_GPU.cpp:619/644/686/724`), painter's-algorithm ordering, and **never preserved as a
  sampleable texture** (every FBO nulls its depth attachment, `:1234/1265/1300`). This is
  not "missing a copy" — it's structurally unusable for screen-space depth effects.
- **Present path:** `PsyX_EndScene` (`src/PsyX_main.cpp:934`) → `GR_CaptureLastFrame`
  (`PsyX_render.cpp:2034`, a working **capture-to-texture template**) → `g_PsyX_PostCaptureHook`
  (`PsyX_main.cpp:964`, a ready **post-pass insertion point**) → `GR_SwapWindow` (bare
  `SDL_GL_SwapWindow`).

**Dividing line:** an effect is cheap if it needs only the final color image; it's blocked
if it needs geometry data (depth / normals / motion vectors), because none of that reaches
any shader.

## Per-effect verdicts

| Effect | Verdict | Effort |
|---|---|---|
| **"32-bit color"** | Backbuffer is *already* RGBA8. The meaningful option = expose the existing dither flag as a **"disable dithering / smooth gradients"** toggle (direct-drawn geometry already renders full-precision). Truly widening VRAM to 24-bit is M and not worth it (only affects 5-bit feedback surfaces, where truncation is period-accurate). | **S** |
| **Antialiasing — MSAA** | Add `SDL_GL_SAMPLES`/`MULTISAMPLEBUFFERS` before context create → polygon edges AA "for free" (world draws to FBO 0); textures, UI, and dither stay pixel-sharp — **the best AA fit for this renderer**. One real validation task: the multisample→single-sample **resolve blits** in `GR_CaptureLastFrame` (`:2067`) and `GR_StoreFrameBuffer` (`:2139`). Fallback if a driver balks: a multisample FBO + explicit resolve. | **S–M** |
| AA — FXAA/SMAA | **Not recommended** — luma-edge blur smears the deliberate dither, nearest-filtered textures, and 2D UI; also needs the absent post-pass infra. | — |
| **Post-process filters** (color-grade, vignette, grain, scanlines, sharpen, CRT, bilinear, PSX dither+downsample) | Needs a fullscreen post pass (none today), **but** the capture-to-texture template (`:2034`) + the `g_PsyX_PostCaptureHook` slot already exist → **~1 day one-time plumbing** (one tiny shader program + a `gl_VertexID` fullscreen triangle + reuse the captured texture). After that, **each filter is S** (a few lines of GLSL + a `g_cfg_*` uniform). The dither matrix is already written (`:685`). | **M once, then S each** |
| **Per-pixel flashlight** | The SH "flashlight cone" is actually **fog/distance-driven**, not a true spotlight. Realistic win = **M**: thread the existing per-vertex normals + light dir/attenuation/color-matrix into the shader and evaluate the *existing* lighting per-fragment → kills gouraud banding, keeps the look. A *true* per-pixel spotlight is **L** — needs per-fragment normals **and** view position (new attributes/varyings), and coarse PSX normals + painter's ordering fight it; it also departs from the art direction. | **M (smooth) / L (true)** |
| **Tonemap / color-grade, Bloom** | The realistic **"modern look"** wins — pure color post passes. Tonemap/grade = highest payoff/lowest cost; bloom = best modern payoff for moderate effort. Both ride the one-time post-pass infra above. | **M** |
| **SSAO, SSR, real DoF, motion blur, TAA** | **Blocked.** All need a sampleable depth buffer and/or normals/motion vectors. Depth here is flat-per-prim and never captured; no normals/positions/velocity reach any shader. Only crude fakes are possible (edge-blur "DoF", full-frame smear) and they fight the aesthetic. | skip |
| **Ray tracing** (RT shadows/reflections/GI) | **Impractical — category mismatch.** Would require replacing GL 3.3 with a Vulkan/DX12 + RTX pipeline, building acceleration structures over non-retained painter's-order geometry, and authoring PBR materials/normals the PSX assets entirely lack. A modern engine grafted onto a PS1 emulation layer; visually incoherent over flat-shaded 256-color geometry. | skip |

## Recommended roadmap (if pursued)

1. **Dither / "32-bit color" toggle** — near-free; surface the existing `g_cfg_psxDither` as
   a user-facing "smooth gradients" option (+ optional console hot-toggle like PGXP).
2. **MSAA** — frequently requested, S–M, the correct AA for this pipeline; just verify the
   two resolve blits.
3. **One-time post-process pass** — unlocks color-grade/tonemap, vignette, grain, scanlines,
   sharpen, CRT, bilinear as **S each**, plus bloom for the full "modern look."
4. **Per-pixel flashlight smoothing** — M, only if the gouraud lighting banding bothers us.

**Off the table without a depth/normal G-buffer:** SSAO, SSR, real DoF, true motion blur,
TAA, ray tracing.

### Key files
- Context/SDL attrs: `PsyX_render.cpp:383-462`, `:494-497`
- Dither + gate: `PsyX_render.cpp:685-724`, `:170-171`
- Shaders (4 embedded programs): `PsyX_render.cpp:841-946`; compile `:1008`
- VRAM format/pack: `PsyX_render.h:102-111`, `PsyX_render.cpp:1621/1722`
- Capture-to-texture template / present: `PsyX_render.cpp:2034-2098`; swap `:2341`
- Present path + post-pass hook point: `PsyX_main.cpp:934-967` (`g_PsyX_PostCaptureHook` `:964`)
- Flat-per-prim depth: `PsyX_GPU.cpp:619/644/686/724`; FBO depth nulled `PsyX_render.cpp:1234/1265/1300`
- Game-side lighting bake + normals: `bodyprog_80055028.c:3273-3591`, `:4186-4255`; flashlight `map_effects.c:410`
