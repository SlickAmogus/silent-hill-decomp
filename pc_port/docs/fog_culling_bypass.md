# Fog-Based Draw Distance Culling Bypass

## Background

The original PSX game uses fog distance as a draw distance optimization. When fog is enabled, any geometry beyond `fogFarDistance_10` is culled entirely — the PSX doesn't waste cycles rendering what fog would fully hide. This works well on the PSX where draw calls are expensive and the low resolution hides pop-in.

On PC this causes objects (trees, fences, world geometry) to visibly pop in and out based on camera angle, since the fog distance is relatively short.

## How It Works on PSX

The fog system has two roles:

1. **Visual fog**: Blends vertex colors toward `fogColor_1C` based on depth, and draws a 2D fog overlay quad
2. **Draw distance culling**: Clamps the far clip distance of every renderer to `fogFarDistance_10`

The culling happens in `bodyprog_80055028.c` at 5 locations, all following the same pattern:

```c
// Without fog: use full draw distance
// With fog: clamp to fog far distance
distance = g_WorldEnvWork.isFogEnabled_1
    ? MIN(baseDistance, g_WorldEnvWork.fogFarDistance_10)
    : baseDistance;
```

The affected renderers and their line numbers (approximate, may shift with upstream merges):

| Location | Function | What it renders |
|----------|----------|----------------|
| ~line 1201 | `func_80056D8C` | Fog overlay geometry |
| ~line 1855 | `func_8005801C` | 3D world models (buildings, ground, objects) |
| ~line 2764 | `func_80059C48` | Additional world geometry |
| ~line 3267 | `func_8005AA08` | Mesh rendering |
| ~line 3729 | `func_8005B62C` | Billboard effects (trees, light shafts) |

## How the PC Port Bypasses It

### 1. Config option

`pc_port/include/pc_config.h` — added `disableCulling` field:
```c
typedef struct {
    int windowWidth;
    int windowHeight;
    int fullscreen;
    int disableCulling; /* 1 = render all objects regardless of view angle */
    char mapName[64];
} s_PcConfig;
```

Set in `config.cfg`:
```
disable_culling = 1
```

### 2. FOG_FAR_DIST() macro

At the top of `bodyprog_80055028.c`:
```c
#ifdef SH_PC_PORT
#define FOG_FAR_DIST() (g_PcConfig.disableCulling ? 0x7FFFFFFF : g_WorldEnvWork.fogFarDistance_10)
#else
#define FOG_FAR_DIST() (g_WorldEnvWork.fogFarDistance_10)
#endif
```

When `disableCulling` is active, `FOG_FAR_DIST()` returns `0x7FFFFFFF` (max int), so `MIN(baseDistance, FOG_FAR_DIST())` always returns `baseDistance` — the fog clamp has no effect.

All 5 clamp sites were changed from:
```c
g_WorldEnvWork.fogFarDistance_10
```
to:
```c
FOG_FAR_DIST()
```

The setter at `WorldEnv_FogDistanceSet` (line ~364) is NOT changed — `fogFarDistance_10` is still stored correctly for use by the visual fog calculations.

### 3. Within-chunk model buffer culling bypass

In `bodyprog_80040A64.c`, `Gfx_IpdChunkDraw` normally uses a subcell lookup table and `func_80044420` (spatial/frustum check) to decide which model buffers within a chunk to render. With `disableCulling`, it iterates ALL model buffers:

```c
if (g_DebugCamEnabled || g_PcConfig.disableCulling) {
    // Render ALL model buffers, skip subcell/spatial culling
    for (i = 0; i < ipdHdr->modelBufferCount_9; i++) { ... }
} else {
    // Original PSX path: subcell lookup + func_80044420 gate
}
```

### 4. Chunk position match bypass

`Ipd_CellPositionMatchCheck` normally only renders chunks matching the player's current cell (or all chunks on exterior maps). With `disableCulling`, all loaded chunks render:

```c
if (g_DebugCamEnabled || g_PcConfig.disableCulling) return true;
```

## Result

- All geometry within loaded chunks renders regardless of distance or angle
- Objects no longer pop in/out when turning the camera
- Togglable via `disable_culling` in `config.cfg`

---

# PC Fog Rendering System

## Problem

The PSX fog system has two visual components:
1. **Vertex color fog** (dpcs/dpcl): Blends vertex colors toward fogColor based on depth
2. **Fog overlays**: Semi-transparent POLY_G4 quads drawn on top of textured geometry, using `SetPriority` mask bits so the overlay only covers opaque texels

PsyCross doesn't support PSX mask bits (`SetPriority` is stubbed). Without them:
- Fog overlays render over transparent areas (visible rectangles on fences/barbed wire)
- Fog overlays render over the background (brightens the sky beyond fogColor)
- Normal geometry gets `setSemiTrans` + overlay = translucent + gray tint

## Solution: Shader-Based Per-Primitive Fog

Instead of the PSX multi-pass overlay approach, the PC port encodes a fog factor per primitive and blends in the fragment shader.

### PsyCross shader changes (`PsyX_render.cpp`)

- `g_PsyX_FogColor[3]` global: set from game's `fogColor_1C` each frame
- `u_fogColor` uniform in all PSX fragment shaders
- Vertex shader: `v_fogAmount = clamp(a_extra.z / 127.0, 0.0, 1.0)` reads fog from `GrVertex._p0`
- Fragment shader (after `fragColor *= v_color`): `fragColor.rgb = mix(fragColor.rgb, u_fogColor, v_fogAmount)`

### PsyCross GPU changes (`PsyX_GPU.cpp`, `libgpu.h`)

- `setPolyGT4`/`setPolyGT3` macros zero `p1` (default: no fog)
- All `MakeColour*` functions initialize `_p0 = 0`
- `ProcessGouraudPoly` copies `poly->p1` → `vertex._p0` for GT3/GT4

### Game-side fog encoding (`bodyprog_80055028.c`)

**Billboards** (`func_8005B62C`): Compute fog from `func_80055A50(depth)` + `fogIntensity_18`, store `(fogAmt * 127) >> 12` in `poly_gt4->p1`. The `var_s1` group-level fog dimming is forced to `Q12(1.0)` on PC to avoid double-darkening.

**Fences/barbed wire** (0x8000 flag in `func_8005801C`): Skip `SetPriority` + fog overlay. Compute fog from `field_252` ramp + `field_4` intensity, store in `poly3->p1`.

**World geometry** (non-0x8000 in `func_8005801C`): Skip `setSemiTrans` + fog overlay (poly1/poly2). Render textured poly opaque. Compute fog via `PC_FACE_FOG_FACTOR` macro (max of 4 vertex fog ramps + base intensity), store in `poly3->p1`.

**2D fog overlay quad** (`func_80056D8C`): Skipped entirely on PC — the per-primitive shader fog replaces it.

### fog color sync (`game_main.c`)

`g_PsyX_FogColor` is set from `WorldEnvWork.fogColor_1C` each frame during the GsSortClear section, alongside the background clear color override.

### `PC_FACE_FOG_FACTOR` macro

```c
#define PC_FACE_FOG_FACTOR(sd) do { \
    s32 _f0 = (sd)->field_252[(sd)->field_380.s_0.field_10]; \
    s32 _f1 = (sd)->field_252[(sd)->field_380.s_0.field_11]; \
    s32 _f2 = (sd)->field_252[(sd)->field_380.s_0.field_12]; \
    s32 _f3 = (sd)->field_252[(sd)->field_380.s_0.field_13]; \
    s32 _mx = _f0; if (_f1>_mx) _mx=_f1; if (_f2>_mx) _mx=_f2; if (_f3>_mx) _mx=_f3; \
    s32 _fa = _mx * 16 + (sd)->field_380.s_0.field_4; \
    if (_fa > 0x1000) _fa = 0x1000; if (_fa < 0) _fa = 0; \
    poly3->p1 = (u8)((_fa * 127) >> 12); \
} while(0)
```

## Debug controls

- Numpad `.`: Cycle fog intensity (100% → 75% → 50% → 25% → OFF → 100%). Currently only the OFF toggle is functional; intensity scaling needs rework since `fogIntensity_18` is set by the game each frame after the debug override runs.
