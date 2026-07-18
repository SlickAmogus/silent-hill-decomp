# HDR / RTX HDR Support — Feasibility Note

**Date:** 2026-07-17 · **Status:** research/feasibility only — nothing implemented.

## The ask

A user on a Windows OLED HDR monitor wants to use **RTX HDR** (NVIDIA's driver-level
SDR→HDR AI filter) with the port. It doesn't work today.

## Why it doesn't work

RTX HDR and Windows Auto HDR hook a game's **DirectX or Vulkan** presentation. They
cannot see an **OpenGL** swapchain. PsyCross presents via SDL2 + OpenGL
(`SDL_GL_SwapWindow`), so RTX HDR has nothing to latch onto. This is a
*presentation-API* problem, not a rendering one — the requester reached for Vulkan
only because it's a "supported renderer," but D3D11 is equally supported and far
cheaper for us.

## Framing: the content is SDR

PsyCross is a software PSX-GPU emulator. It composites in emulated 15-bit PSX VRAM
and **dithers/quantizes to 5 bits per channel**; every buffer is 8-bit RGBA8; the F3
"tonemap" runs on already-clamped 8-bit LDR (cosmetic). There is no HDR content and
no float/linear stage. So the goal is **not** to author HDR — it's to let RTX HDR
expand our SDR image, which only requires presenting through an API it recognizes.

## Options, cheapest first

### 1. No code — NVIDIA driver setting (have the user try first)

NVIDIA drivers can shim an OpenGL app onto a DXGI swapchain:
- **NVIDIA Control Panel → Manage 3D Settings → "OpenGL/Vulkan present method" →
  "Prefer layered on DXGI Swapchain"** (also in the NVIDIA App on newer drivers).
- Run the game **borderless / fullscreen**, then enable RTX HDR.

Works for many OpenGL titles with zero game change, but is finicky depending on how
the app creates its window/context. [Special K](https://wiki.special-k.info/en/HDR/Retrofit)
can also force OpenGL→D3D11 for the same effect.

### 2. Native fix — GL→D3D11 interop present layer (recommended if the shim is unreliable)

Keep 100% of the OpenGL rendering; replace only the final present:
- Create a D3D11 device + **DXGI flip-model swapchain** on SDL's `HWND`.
- Render the final GL frame to an offscreen texture, share it via
  **`WGL_NV_DX_interop2`**, and present through DXGI instead of `SDL_GL_SwapWindow`.
- The port then looks like a native DX11 app and RTX HDR / Auto HDR hook it reliably.
  **We do not implement HDR** — RTX HDR does the SDR→HDR expansion.
- Config-gated, Windows-only. Insertion point: the existing
  `GR_PostProcess` / `GR_DrawFullscreenTexture` / `GR_SwapWindow` path in
  `pc_port/PsyCross/src/render/PsyX_render.cpp`.
- **Effort:** ~a week for a working toggle; more for resize / vsync / fullscreen edges.
- **Bonus:** a 10-bit or fp16 swapchain also removes the colour banding this
  5-bit-dithered game shows on a big OLED, even in plain SDR.

### 3. Full Vulkan renderer — NOT recommended for this

A Vulkan backend is a real rewrite of PsyCross's GL backend (~3,700-LOC single file
`PsyX_render.cpp`, behind the clean `GR_*` seam that `PsyX_GPU.cpp` already uses; ~6
GLSL shaders → SPIR-V; `glBlitFramebuffer` → `vkCmdBlit`/`vkCmdResolve`). It's bounded
to one file, but roughly **3 person-months**, and it produces **no better RTX HDR
result** than the D3D11 bridge. Only worth it as a broader modernization goal.

## Recommendation

1. Have the requester try the NVIDIA "Prefer layered on DXGI Swapchain" present method
   + borderless — may work today, no build.
2. If unreliable, implement the **GL→D3D11 interop DXGI present** (option 2): small,
   Windows-only, and it also fixes banding. Not Vulkan.

## Caveats / notes

- SDL2 has no HDR/colorspace concept; SDL3's HDR works only through its own GPU/renderer
  API, not a raw GL context — an SDL3 upgrade alone would not solve this.
- RTX HDR / Auto HDR require borderless or fullscreen-exclusive and a **flip-model**
  swapchain.
- Steam Deck OLED / Linux HDR is Vulkan + gamescope only (would need a GL→Vulkan interop
  present); out of scope for this Windows user, but note the D3D11 bridge is Windows-only.

Sources: [PCGamingWiki — HDR](https://www.pcgamingwiki.com/wiki/Glossary:High_dynamic_range_(HDR)),
[Special K — HDR Retrofit](https://wiki.special-k.info/en/HDR/Retrofit),
[Microsoft — Use DirectX with Advanced Color](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range),
[Guru3D — NVIDIA RTX HDR](https://www.guru3d.com/story/nvidia-introduces-rtx-hdr-functionality-to-enhance-visuals-in-unsupported-games-through-ai-filter/).
