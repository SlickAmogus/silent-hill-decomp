# Render backends (OpenGL / Direct3D 11 / Vulkan)

The port has always drawn through one OpenGL path. It still does — but that path
can now run against an **OpenGL ES 3.0** context supplied by
[ANGLE](https://github.com/google/angle), which translates it to **Direct3D 11**
or **Vulkan** underneath. One renderer, several devices.

Native OpenGL remains the default and is completely untouched by any of this.
Selecting it runs exactly the code that shipped before.

## Choosing one

`config.cfg`:

```ini
renderer = gl        # default: native OpenGL 3.3 (unchanged)
renderer = d3d11     # Direct3D 11 via ANGLE
renderer = vulkan    # Vulkan via ANGLE
renderer = gles      # OpenGL ES 3.0 from the native driver
renderer = warp      # Direct3D 11 WARP — Microsoft's CPU rasterizer
renderer = software  # SwiftShader — ANGLE's CPU rasterizer, no GPU at all
```

`dx11`, `directx11`, `dx12`, `d3d12`, `dx9`, `d3d9`, `vk`, `opengl`,
`swiftshader` and `cpu` are accepted as aliases. ANGLE has no D3D12 backend and
has removed its D3D9 one, so `d3d12` and `d3d9` both resolve to `d3d11`.

Anything unrecognised falls back to `gl`. A typo cannot stop the game booting.

## Why bother, if OpenGL already works

Mostly so **third-party tools can attach**. ReShade, RTSS, Special K, OBS game
capture and the Steam overlay all hook DXGI and Vulkan far more thoroughly than
they hook `opengl32` — ReShade's OpenGL support in particular is a DLL-wrapper
arrangement that does not see an ANGLE context at all, while its DXGI path sees
the swapchain ANGLE creates. Running on `d3d11` gives those tools a genuine
`ID3D11Device` and `IDXGISwapChain` to work with.

Secondary reasons: some drivers are simply better maintained on D3D11 than on
their GL path, and `warp` / `software` give a way to run with no usable GPU
driver at all.

It is **not** primarily a performance feature. Translation costs something;
whether that is offset by a better driver path varies by machine.

## Installing ANGLE

The translated backends need ANGLE's runtime beside the executable. It is not
vendored — it is ~15 MB of third-party binary with its own release cadence.

Drop the DLLs into `pc_port/angle/` and CMake deploys the whole directory next
to the exe on build. Or just copy them beside a released `SilentHillPC.exe`.

Needed:

| File | Required for |
|---|---|
| `libEGL.dll` | all translated backends |
| `libGLESv2.dll` | all translated backends |
| `d3dcompiler_47.dll` | `d3d11` / `warp`, if Windows lacks it |
| `vulkan-1.dll` | `vulkan` (only if not already on the system) |
| `vk_swiftshader.dll` + `vk_swiftshader_icd.json` | `software` |

### Where to get a build — this matters

**Not every ANGLE build contains every backend.** They are compile-time `gn`
flags, and a build made with `angle_enable_vulkan=false` will refuse
`renderer = vulkan` no matter what you do. This was verified the hard way:

- [mmozeiko/build-angle](https://github.com/mmozeiko/build-angle/releases/latest)
  — weekly x64/arm64 builds, small and convenient, but built with
  `angle_enable_vulkan=false angle_enable_gl=false angle_enable_d3d9=false`.
  **D3D11 only.** Good enough if `d3d11` is all you want.
- **An [Electron](https://github.com/electron/electron/releases) release zip**
  (`electron-vX-win32-x64.zip`) — Chromium's own ANGLE, with D3D11 **and**
  Vulkan **and** SwiftShader. It also carries `vulkan-1.dll`,
  `vk_swiftshader.dll` and `vk_swiftshader_icd.json`. This is the build to use
  if you want all backends. Extract the files in the table above from the zip
  root.

Modern Chrome and Edge no longer help: they link ANGLE statically into
`chrome.dll` / `msedge.dll` and ship no `libEGL.dll` at all.

## What the log tells you

The adapter line names the device that was really selected, which is the only
reliable confirmation:

```
*Render backend: Vulkan (via ANGLE)
*Video adapter: ANGLE (NVIDIA, Vulkan 1.4.341 (NVIDIA GeForce RTX 5060), NVIDIA-610.74.0.0) by Google Inc.
*OpenGL version: OpenGL ES 3.0 (ANGLE 2.1.28633 ...)
*GL caps: polygonMode=0 getTexImage=0 mapBuffer=0 drawBuffer=0 clearDepthD=0 texLevelParam=0 noperspective=1
```

If a backend cannot start, the reason is logged with the EGL error code and the
game continues on native OpenGL rather than failing to launch:

```
ANGLE: eglInitialize failed for 'vulkan' (egl error 0x3004) - that device is not available on this machine
Backend 'vulkan' could not be initialised through ANGLE; falling back to native OpenGL
```

## Known differences on a translated backend

| Feature | Status |
|---|---|
| Debug wireframe (`polygonMode`) | **Unavailable.** ES has no `glPolygonMode`. Debug-only. |
| Affine texture warping | Preserved — ANGLE exposes `NV_shader_noperspective_interpolation`. If a driver ever lacks it, affine falls back to perspective-correct, and only on PGXP-projected geometry (the legacy path has `w == 1`, where the two are identical anyway). |
| Hi-res texture slot reuse | Always re-specifies the texture rather than sub-imaging, because `glGetTexLevelParameteriv` is ES 3.1+. Slightly more upload traffic, same image. |
| Shadow-map border clamp | `CLAMP_TO_EDGE` instead of `CLAMP_TO_BORDER`. No visible difference — the shader rejects out-of-frustum receivers before sampling. |
| Everything else | Same code, same shaders, same output. |

## How it works, for the next person in here

`GR_ResolveBackend` (PsyX_render.cpp) decides the backend **before the window
exists**, because that choice changes how the window is created.

For translated backends PsyCross creates the EGL display, surface and context
**itself** (`PsyX_Angle_Create`, PsyX_backend.cpp), against the `HWND` from
SDL's window, and SDL is left owning only the window.

That is deliberate and was not the first design. SDL2 can load ANGLE on its own
— but it creates the display with plain `eglGetDisplay`, and on that path
**ANGLE ignores `ANGLE_DEFAULT_PLATFORM` entirely**: every backend silently came
up as D3D11, its Windows default. Verified by setting the variable from the
parent process with the game reading it — no effect. Selecting the device
requires `eglGetPlatformDisplayEXT` with `EGL_PLATFORM_ANGLE_TYPE_ANGLE`
attributes, and SDL exposes no way to pass them. Hence the local EGL bootstrap.

Two consequences worth remembering:

- `SDL_GL_SwapWindow` and `SDL_GL_GetProcAddress` do nothing useful when ANGLE
  is active. Anything that presents a frame or resolves a GL symbol must go
  through `PsyX_Angle_Swap` / `PsyX_Angle_GetProcAddress` — see `GR_SwapWindow`
  and the FMV player, which presents its own frames.
- Desktop-only GL entry points resolve to **NULL** under ANGLE rather than
  failing loudly. `GR_ProbeCapabilities` records which ones, and every call site
  branches on `g_grCaps`. Calling an unguarded one is an immediate jump to
  address 0 — that is exactly how the first working build crashed
  (`glClearDepthf`, which is ES-only and therefore *not* loaded by glad's
  desktop pass; `GR_InitialiseGLExt` now runs the ES2 loader as well).

Shaders needed almost no work: PsyCross's were already written in a
version-agnostic dialect with `varying`/`attribute`/`texture2D` remapped by
`#define`, so only the `#version` line is chosen at runtime. PC-port code that
compiles its own overlay shaders (debug overlay, FMV blit, achievement toast and
browser) gets the right preamble from `PsyX_Shader_Preamble`.

## Native backends (not implemented, and why)

A real D3D11/D3D12/Vulkan renderer — no ANGLE — would mean reimplementing every
GL call site: ~500 in `PsyX_render.cpp` and ~500 more across `pc_port/src`
(`dbg_overlay.c` alone has 171), plus translating every shader to HLSL/SPIR-V.
That is months of work and a large regression risk against a port that currently
looks right.

If it is ever wanted, the seam already exists: `GR_*` in `PsyX_render.h` is the
complete renderer interface, `g_grCaps` already describes capability differences
at runtime, and the shaders being GLSL means `glslang` + `SPIRV-Cross` can
generate the HLSL/SPIR-V rather than hand-porting it. Nothing here forecloses
that; it just is not the cheap way to get D3D11 and Vulkan, which is what this
is.
