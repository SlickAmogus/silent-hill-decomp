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

- Fog still renders visually (vertex color attenuation, fog overlay quad, clear color)
- All geometry within loaded chunks renders regardless of distance or angle
- Objects no longer pop in/out when turning the camera
- Togglable via `disable_culling` in `config.cfg`
