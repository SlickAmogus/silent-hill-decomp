# Silent Hill Decomp — Function Naming & Documentation Proposals

This document proposes names, parameter names, and descriptions for `func_XXXXXXXX` functions
in the Silent Hill PS1 decompilation, based on analysis of the code and knowledge from the PC port.

**Conventions followed:**
- PascalCase functions with subsystem prefixes (e.g., `Player_`, `Gfx_`, `Collision_`)
- camelCase parameters
- `g_` for globals, `s_` for structs, `e_` for enums
- Doxygen `/** @brief */` style comments, Allman brace style, 4-space indent

---

## Table of Contents

1. [GFX Rendering (`bodyprog_80055028.c`)](#1-gfx-rendering)
2. [Effects & Flashlight (`bodyprog_8003E5E8.c`)](#2-effects--flashlight)
3. [World GFX & Character Models (`bodyprog_8003BE50.c`)](#3-world-gfx--character-models)
4. [Animation & Skeleton (`bodyprog_anim_800445A4.c`)](#4-animation--skeleton)
5. [Player System (`bodyprog_800706E4.c`)](#5-player-system)
6. [Collision System (`bodyprog_800697EC.c`)](#6-collision-system)
7. [Game State Machine (`game_sys_states.c`)](#7-game-state-machine)
8. [Loading Screen (`load_screen.c`)](#8-loading-screen)
9. [View / Camera System (`view/`)](#9-view--camera-system)
10. [Event Scripting (`bodyprog_80085D78.c`)](#10-event-scripting)
11. [Vibration / Haptics (`bodyprog_80089090.c`)](#11-vibration--haptics)
12. [Combat System (`bodyprog_combat_*.c`)](#12-combat-system)
13. [World Effects (`bodyprog_8005E0DC.c`)](#13-world-effects)
14. [Visual Effects (`bodyprog_800652F4.c`)](#14-visual-effects)
15. [BGM / Music (`bodyprog_bgm_80087EA8.c`)](#15-bgm--music)
16. [Map Screen (`bodyprog_mapscreen_*.c`)](#16-map-screen)
17. [Character Spawning (`chara_spawn.c`)](#17-character-spawning)
18. [Items / Inventory (`items/`)](#18-items--inventory)

---

## 1. GFX Rendering

**File:** `src/bodyprog/gfx/bodyprog_80055028.c`

### func_80055330 → WorldEnv_LightingParamsSet
**Line:** 135
**Original:**
```c
void func_80055330(u8 arg0, s32 arg1, u8 arg2, s32 tintR, s32 tintG, s32 tintB, q23_8 brightness);
```
**Proposed:**
```c
/** @brief Configures world environment lighting: mode, tint color, brightness, and GTE color matrix.
 *
 * @param envMode Environment lighting mode (0=no directional, 1=positional/specular, 2=directional).
 * @param lightScale Q12 scale factor for the color matrix.
 * @param maxShade Maximum shade value cap.
 * @param tintR Red channel of world tint color (full precision, divided by 32 for CVECTOR).
 * @param tintG Green channel of world tint color.
 * @param tintB Blue channel of world tint color.
 * @param brightness Screen brightness overlay value (Q23.8).
 */
void WorldEnv_LightingParamsSet(u8 envMode, s32 lightScale, u8 maxShade, s32 tintR, s32 tintG, s32 tintB, q23_8 brightness);
```

### func_800553E0 → WorldEnv_OverlayEffectSet
**Line:** 166
**Original:**
```c
void func_800553E0(u32 arg0, u8 arg1, u8 arg2, u8 arg3, u8 arg4, u8 arg5, u8 arg6);
```
**Proposed:**
```c
/** @brief Enables or disables the screen overlay effect and forwards config to the overlay handler.
 *
 * @param effectEnabled Nonzero to enable the overlay effect.
 * @param arg1..arg6 Configuration values passed to func_80040E7C.
 */
void WorldEnv_OverlayEffectSet(u32 effectEnabled, u8 arg1, u8 arg2, u8 arg3, u8 arg4, u8 arg5, u8 arg6);
```

### func_80055434 → WorldEnv_LightPositionGet
**Line:** 176
**Original:**
```c
void func_80055434(VECTOR3* vec);
```
**Proposed:**
```c
/** @brief Copies the current world light source position into the caller's vector.
 *
 * @param vec Output vector to receive the Q19.12 light position.
 */
void WorldEnv_LightPositionGet(VECTOR3* vec);
```

### func_8005545C → WorldEnv_LightAnglesGet
**Line:** 181
**Original:**
```c
s32 func_8005545C(SVECTOR* vec);
```
**Proposed:**
```c
/** @brief Copies computed light angles and returns the light alpha parameter.
 *
 * @param vec Output SVECTOR for the light direction angles.
 * @return Light alpha parameter (field_54).
 */
s32 WorldEnv_LightAnglesGet(SVECTOR* vec);
```

### func_80055490 → WorldEnv_LightRotationGet
**Line:** 187
**Original:**
```c
s32 func_80055490(SVECTOR* arg0);
```
**Proposed:**
```c
/** @brief Copies the light rotation vector and returns the light alpha parameter.
 *
 * @param rot Output SVECTOR for the light rotation direction.
 * @return Light alpha parameter (field_54).
 */
s32 WorldEnv_LightRotationGet(SVECTOR* rot);
```

### func_800554C4 → WorldEnv_LightSourceSet
**Line:** 193
**Original:**
```c
void func_800554C4(s32 arg0, s16 arg1, GsCOORDINATE2* coord0, GsCOORDINATE2* coord1, SVECTOR* rot, q19_12 x, q19_12 y, q19_12 z, s_WaterZone* waterZones);
```
**Proposed:**
```c
/** @brief Sets the world environment light source with position, rotation, and intensity.
 *
 * Optionally transforms rotation/position through coordinate hierarchies.
 * Computes derived light angles and builds per-axis lighting ramp tables.
 *
 * @param alpha Light intensity/alpha value.
 * @param waterLevel Water surface height offset.
 * @param rotCoord Optional coordinate hierarchy for rotation transform; NULL for direct.
 * @param posCoord Optional coordinate hierarchy for position transform; NULL for direct.
 * @param rot Light rotation direction vector.
 * @param x Q19.12 light source X position.
 * @param y Q19.12 light source Y position.
 * @param z Q19.12 light source Z position.
 * @param waterZones Pointer to water zone data.
 */
void WorldEnv_LightSourceSet(s32 alpha, s16 waterLevel, GsCOORDINATE2* rotCoord, GsCOORDINATE2* posCoord, SVECTOR* rot, q19_12 x, q19_12 y, q19_12 z, s_WaterZone* waterZones);
```

### func_80055648 → WorldEnv_LightRampBuild
**Line:** 243
**Original:**
```c
void func_80055648(s32 arg0, SVECTOR* arg1);
```
**Proposed:**
```c
/** @brief Builds per-axis lighting attenuation ramp tables for env modes 1 and 2.
 *
 * @param alpha Light intensity scale factor.
 * @param lightDir Light direction SVECTOR.
 */
void WorldEnv_LightRampBuild(s32 alpha, SVECTOR* lightDir);
```

### func_800557DC → WorldEnv_LightDepthGet
**Line:** 291
**Original:**
```c
s32 func_800557DC(void);
```
**Proposed:**
```c
/** @brief Computes the depth (Z) of the light source after camera transform.
 *
 * @return Light depth as Q19.12.
 */
s32 WorldEnv_LightDepthGet(void);
```

### func_80055814 → WorldEnv_FogIntensityFromDepth
**Line:** 299
**Original:**
```c
void func_80055814(s32 arg0);
```
**Proposed:**
```c
/** @brief Sets fogIntensity_18 from the complement of the fog ramp at the given depth.
 *
 * @param depth Depth value to evaluate against the fog ramp.
 */
void WorldEnv_FogIntensityFromDepth(s32 depth);
```

### func_800559A8 → WorldEnv_FogRampInterpolate
**Line:** 363
**Original:**
```c
s32 func_800559A8(s32 arg0);
```
**Proposed:**
```c
/** @brief Evaluates the fog ramp with sub-entry linear interpolation.
 *
 * @param depth Depth value to evaluate.
 * @return Q12 fog density value (0 = no fog, Q12(1.0) = fully fogged).
 */
s32 WorldEnv_FogRampInterpolate(s32 depth);
```

### func_80055A50 → WorldEnv_FogRampLookup
**Line:** 407
**Original:**
```c
u8 func_80055A50(s32 arg0);
```
**Proposed:**
```c
/** @brief Direct (non-interpolated) fog ramp table lookup.
 *
 * @param depth Depth value to evaluate.
 * @return Fog density as u8 (0-255).
 */
u8 WorldEnv_FogRampLookup(s32 depth);
```

### func_80055A90 → WorldEnv_FogColorCompute
**Line:** 421
**Original:**
```c
void func_80055A90(CVECTOR* arg0, CVECTOR* arg1, u8 arg2, s32 arg3);
```
**Proposed:**
```c
/** @brief Computes fog-blended and tint-blended colors for a vertex at a given depth.
 *
 * Uses GTE dpcs/dpcl for fog and tint blending.
 *
 * @param fogResult Output fog-blended color.
 * @param tintResult Output tint/shade-blended color.
 * @param shadeLevel Shade/brightness level for tint pass (0-127).
 * @param depth Depth for fog ramp evaluation.
 */
void WorldEnv_FogColorCompute(CVECTOR* fogResult, CVECTOR* tintResult, u8 shadeLevel, s32 depth);
```

### func_80055B74 → WorldEnv_FogBlendColor
**Line:** 466
**Original:**
```c
void func_80055B74(CVECTOR* result, CVECTOR* color, s32 arg2);
```
**Proposed:**
```c
/** @brief Blends an input color toward the fog color based on depth.
 *
 * @param result Output blended color.
 * @param color Input color to blend toward fog.
 * @param depth Depth for fog evaluation.
 */
void WorldEnv_FogBlendColor(CVECTOR* result, CVECTOR* color, s32 depth);
```

### func_80055C3C → WorldEnv_FogBlendColorWithPosition
**Line:** 495
**Original:**
```c
void func_80055C3C(CVECTOR* result, CVECTOR* color, s32 arg2, s32 arg3, s32 arg4, s32 arg5);
```
**Proposed:**
```c
/** @brief Blends color toward fog considering both depth and positional shade.
 *
 * @param result Output blended color.
 * @param color Input color.
 * @param posX Q19.12 X position for shade calculation.
 * @param posY Q19.12 Y position.
 * @param posZ Q19.12 Z position.
 * @param depth Depth value for fog ramp evaluation.
 */
void WorldEnv_FogBlendColorWithPosition(CVECTOR* result, CVECTOR* color, s32 posX, s32 posY, s32 posZ, s32 depth);
```

### func_80055D78 → WorldEnv_PositionalShadeLookup
**Line:** 534
**Original:**
```c
u8 func_80055D78(q19_12 posX, q19_12 posY, q19_12 posZ);
```
**Proposed:**
```c
/** @brief Computes a shade value for a world position relative to the environment light.
 *
 * Returns 0 when env mode is 0 (uniform lighting). Iterates axis ramp tables
 * to find minimum shade contribution.
 *
 * @param posX Q19.12 world X position.
 * @param posY Q19.12 world Y position.
 * @param posZ Q19.12 world Z position.
 * @return Shade value clamped to [0, maxShade].
 */
u8 WorldEnv_PositionalShadeLookup(q19_12 posX, q19_12 posY, q19_12 posZ);
```

### func_80055E90 → WorldEnv_TintColorApply
**Line:** 581
**Original:**
```c
void func_80055E90(CVECTOR* color, u8 fadeAmount);
```
**Proposed:**
```c
/** @brief Applies world tint color to an existing color using GTE depth cue.
 *
 * @param color In/out color to tint.
 * @param fadeAmount 0-127 shade level; higher values reduce tint.
 */
void WorldEnv_TintColorApply(CVECTOR* color, u8 fadeAmount);
```

### func_80055ECC → WorldEnv_TintColorWithSpecular
**Line:** 599
**Original:**
```c
void func_80055ECC(CVECTOR* color, SVECTOR3* arg1, SVECTOR3* arg2, MATRIX* mat);
```
**Proposed:**
```c
/** @brief Combines tint color with specular/environment lighting.
 *
 * @param color In/out color.
 * @param normal Surface normal direction.
 * @param position Vertex/surface position.
 * @param mat View/world matrix for the lighting transform.
 */
void WorldEnv_TintColorWithSpecular(CVECTOR* color, SVECTOR3* normal, SVECTOR3* position, MATRIX* mat);
```

### func_80055F08 → WorldEnv_SpecularShadeCompute
**Line:** 604
**Original:**
```c
u8 func_80055F08(SVECTOR3* arg0, SVECTOR3* arg1, MATRIX* mat);
```
**Proposed:**
```c
/** @brief Computes a specular/environment shade value for a surface.
 *
 * @param position Vertex position (model space).
 * @param normal Surface normal.
 * @param mat View/world matrix.
 * @return Shade value (u8).
 */
u8 WorldEnv_SpecularShadeCompute(SVECTOR3* position, SVECTOR3* normal, MATRIX* mat);
```

### func_800563E8 → Lm_MaterialTextureOffsetApply
**Line:** 761
**Original:**
```c
void func_800563E8(s_LmHeader* lmHdr, s32 arg1, s32 arg2, s32 arg3);
```
**Proposed:**
```c
/** @brief Applies texture page and CLUT offset adjustments to all materials in an LM model.
 *
 * @param lmHdr LM header containing the materials.
 * @param tPageOffset Offset added to texture page X coordinate.
 * @param clutYOffset CLUT Y offset (divided by 16).
 * @param clutXOffset CLUT X offset (shifted left 6).
 */
void Lm_MaterialTextureOffsetApply(s_LmHeader* lmHdr, s32 tPageOffset, s32 clutYOffset, s32 clutXOffset);
```

### func_800566B4 → Lm_MaterialsBatchLoad
**Line:** 866
**Original:**
```c
void func_800566B4(s_LmHeader* lmHdr, s_FsImageDesc* images, s8 unused, s32 startIdx, s32 blendMode);
```
**Proposed:**
```c
/** @brief Loads all material textures for an LM model in batch from the file system.
 *
 * @param lmHdr LM header with materials to load.
 * @param images Array of image descriptors to fill.
 * @param unused Unused parameter.
 * @param startIdx Starting file index for FS search.
 * @param blendMode Semi-transparency blend mode.
 */
void Lm_MaterialsBatchLoad(s_LmHeader* lmHdr, s_FsImageDesc* images, s8 unused, s32 startIdx, s32 blendMode);
```

### func_80056D8C → Gfx_FogQuadDraw
**Line:** 1095
**Original:**
```c
void func_80056D8C(s16 arg0, s16 arg1, s16 arg2, s16 arg3, s32 arg4, s32 arg5, GsOT* arg6, s32 arg7);
```
**Proposed:**
```c
/** @brief Draws a screen-aligned fog overlay quad at a given depth.
 *
 * Computes fog density from depth, tints by fog color, inserts semi-transparent
 * POLY_G4 into the ordering table. Clipped to screen bounds.
 *
 * @param x0 Top-left screen X.
 * @param y0 Top-left screen Y.
 * @param x1 Bottom-right screen X.
 * @param y1 Bottom-right screen Y.
 * @param depth World depth value for fog density lookup.
 * @param otDepth OT priority depth for insertion.
 * @param ot Ordering table to insert into.
 * @param depthShift OT depth shift factor.
 */
void Gfx_FogQuadDraw(s16 x0, s16 y0, s16 x1, s16 y1, s32 depth, s32 otDepth, GsOT* ot, s32 depthShift);
```

### func_80057090 → Model_RenderDispatch
**Line:** 1197
**Original:**
```c
void func_80057090(s_ModelInfo* modelInfo, GsOT* arg1, s32 arg2, MATRIX* mat0, MATRIX* mat1, u16 arg5);
```
**Proposed:**
```c
/** @brief Top-level model rendering dispatch.
 *
 * Determines OT priority, checks for special modes, and dispatches to the
 * appropriate rendering path: B (field_B_0=1) or C (field_B_0=0).
 *
 * @param modelInfo Model to render.
 * @param ot Ordering table.
 * @param otShift OT depth shift.
 * @param viewMatrix Model's world-to-view matrix.
 * @param envMatrix Optional environment lighting matrix; NULL to skip.
 * @param texPageBase Texture page base offset for path B.
 */
void Model_RenderDispatch(s_ModelInfo* modelInfo, GsOT* ot, s32 otShift, MATRIX* viewMatrix, MATRIX* envMatrix, u16 texPageBase);
```

### func_800571D0 → Model_OtPriorityGet
**Line:** 1235
**Original:**
```c
s32 func_800571D0(u32 arg0);
```
**Proposed:**
```c
/** @brief Maps a 3-bit model priority code to an ordering table offset.
 *
 * Returns: 0→2, 1→0, 2→4, 3→33, 4→66, 5→99.
 *
 * @param priorityCode 0-5 priority value.
 * @return OT offset.
 */
s32 Model_OtPriorityGet(u32 priorityCode);
```

### func_80057228 → WorldEnv_LightTransform
**Line:** 1260
**Original:**
```c
void func_80057228(MATRIX* mat, s32 alpha, SVECTOR* arg2, VECTOR3* arg3);
```
**Proposed:**
```c
/** @brief Transforms world light direction and position into view space.
 *
 * @param mat View/world matrix.
 * @param alpha Light intensity scale.
 * @param lightDir World-space light direction.
 * @param lightPos Q19.12 world-space light position.
 */
void WorldEnv_LightTransform(MATRIX* mat, s32 alpha, SVECTOR* lightDir, VECTOR3* lightPos);
```

### func_80057344 → Model_RenderPathC
**Line:** 1284
**Original:**
```c
void func_80057344(s_ModelInfo* modelInfo, GsOT_TAG* otTag, void* arg2, MATRIX* mat);
```
**Proposed:**
```c
/** @brief Renders a model using path C (field_B_0=0 pipeline).
 *
 * For each mesh: copies vertex/normal data to scratchpad, optionally computes
 * environment lighting, transforms vertices via GTE, and assembles GPU primitives.
 *
 * @param modelInfo Model to render.
 * @param otTag OT tag base for primitive insertion.
 * @param otShift Depth shift for OT insertion.
 * @param mat World-to-view matrix.
 */
void Model_RenderPathC(s_ModelInfo* modelInfo, GsOT_TAG* otTag, void* otShift, MATRIX* mat);
```

### func_800574D4 → Mesh_CopyToScratchpad
**Line:** 1331
**Original:**
```c
void func_800574D4(s_MeshHeader* meshHdr, s_GteScratchData* scratchData);
```
**Proposed:**
```c
/** @brief Copies mesh vertex XY, Z, and auxiliary data to scratchpad at offset 0.
 *
 * @param meshHdr Mesh to copy.
 * @param scratchData Scratchpad destination.
 */
void Mesh_CopyToScratchpad(s_MeshHeader* meshHdr, s_GteScratchData* scratchData);
```

### func_8005759C → Mesh_CopyToScratchpadAtOffset
**Line:** 1370
**Original:**
```c
void func_8005759C(s_MeshHeader* meshHdr, s_GteScratchData* scratchData, s32 vertOffset, s32 normalOffset);
```
**Proposed:**
```c
/** @brief Copies mesh data to scratchpad at specified vertex/normal offsets.
 *
 * Allows multiple meshes to share the scratchpad with different offsets.
 *
 * @param meshHdr Mesh to copy.
 * @param scratchData Scratchpad destination.
 * @param vertOffset Start index for vertex data.
 * @param normalOffset Start index for normal data.
 */
void Mesh_CopyToScratchpadAtOffset(s_MeshHeader* meshHdr, s_GteScratchData* scratchData, s32 vertOffset, s32 normalOffset);
```

### func_80057658 → Mesh_SpecularLightingCompute
**Line:** 1398
**Original:**
```c
void func_80057658(s_MeshHeader* meshHdr, s32 offset, s_GteScratchData* scratchData, SVECTOR3* arg3, SVECTOR* arg4);
```
**Proposed:**
```c
/** @brief Computes per-vertex specular/positional lighting for path C env mode 1.
 *
 * @param meshHdr Mesh with normals to evaluate.
 * @param offset Shade buffer offset.
 * @param scratchData Scratchpad with vertex positions.
 * @param lightDir View-space light direction.
 * @param lightPos View-space light position.
 */
void Mesh_SpecularLightingCompute(s_MeshHeader* meshHdr, s32 offset, s_GteScratchData* scratchData, SVECTOR3* lightDir, SVECTOR* lightPos);
```

### func_80057A3C → Mesh_DirectionalLightingCompute
**Line:** 1532
**Original:**
```c
void func_80057A3C(s_MeshHeader* meshHdr, s32 offset, s_GteScratchData* scratchData, SVECTOR3* lightVec);
```
**Proposed:**
```c
/** @brief Computes per-vertex directional (Lambert) lighting for path C env mode 2.
 *
 * @param meshHdr Mesh to light.
 * @param offset Shade buffer offset.
 * @param scratchData Scratchpad with normal data.
 * @param lightVec View-space directional light vector.
 */
void Mesh_DirectionalLightingCompute(s_MeshHeader* meshHdr, s32 offset, s_GteScratchData* scratchData, SVECTOR3* lightVec);
```

### func_80057B7C → Mesh_VertexTransformProject
**Line:** 1576
**Original:**
```c
void func_80057B7C(s_MeshHeader* meshHdr, s32 offset, s_GteScratchData* scratchData, MATRIX* mat);
```
**Proposed:**
```c
/** @brief Transforms and projects mesh vertices from model to screen space via GTE rtpt.
 *
 * @param meshHdr Mesh to transform.
 * @param offset Vertex offset in scratchpad.
 * @param scratchData Scratchpad buffer.
 * @param mat World-to-screen matrix.
 */
void Mesh_VertexTransformProject(s_MeshHeader* meshHdr, s32 offset, s_GteScratchData* scratchData, MATRIX* mat);
```

### func_8005801C → Mesh_PrimitiveAssemblePathC
**Line:** 1672
**Original:**
```c
void func_8005801C(s_MeshHeader* meshHdr, s_GteScratchData* scratchData, GsOT_TAG* tag, s32 arg3);
```
**Proposed:**
```c
/** @brief Assembles GPU primitives from transformed mesh data for path C rendering.
 *
 * Handles four sub-paths: env+fog, env+no-fog, no-env+fog, no-env+no-fog.
 * Performs backface culling, depth culling, and screen-bounds checks.
 *
 * @param meshHdr Mesh primitives to assemble.
 * @param scratchData Scratchpad with projected vertices, depths, shade, fog densities.
 * @param tag Ordering table tag array.
 * @param otShift Depth shift for OT insertion.
 */
void Mesh_PrimitiveAssemblePathC(s_MeshHeader* meshHdr, s_GteScratchData* scratchData, GsOT_TAG* tag, s32 otShift);
```

### func_80059D50 → Model_RenderSpecialMode
**Line:** 2500
**Original:**
```c
void func_80059D50(s32 arg0, s_ModelInfo* modelInfo, MATRIX* mat, s32 arg3, GsOT_TAG* tag);
```
**Proposed:**
```c
/** @brief Renders a model using flat-color special mode (field_B_4 values 1-3).
 *
 * @param mode Rendering mode (1=dark overlay, 2=bright overlay, 3=dark overlay).
 * @param modelInfo Model to render.
 * @param mat World-to-view matrix.
 * @param otShift Depth shift.
 * @param tag OT tag array.
 */
void Model_RenderSpecialMode(s32 mode, s_ModelInfo* modelInfo, MATRIX* mat, s32 otShift, GsOT_TAG* tag);
```

### func_80059E34 → Mesh_PrimitiveAssembleFlat
**Line:** 2518
**Original:**
```c
void func_80059E34(u32 arg0, s_MeshHeader* meshHdr, s_GteScratchData* scratchData, s32 arg3, GsOT_TAG* tag);
```
**Proposed:**
```c
/** @brief Assembles POLY_FT4 primitives with a flat packed color by rendering mode.
 *
 * Used for shadow/silhouette rendering and special effects.
 *
 * @param mode Flat rendering mode (1, 2, or 3).
 * @param meshHdr Mesh to assemble.
 * @param scratchData Scratchpad with projected vertices and depths.
 * @param otShift Depth shift for OT insertion.
 * @param tag OT tag array.
 */
void Mesh_PrimitiveAssembleFlat(u32 mode, s_MeshHeader* meshHdr, s_GteScratchData* scratchData, s32 otShift, GsOT_TAG* tag);
```

### func_8005A21C → Model_RenderPathB
**Line:** 2651
**Original:**
```c
void func_8005A21C(s_ModelInfo* modelInfo, GsOT_TAG* otTag, void* arg2, MATRIX* mat);
```
**Proposed:**
```c
/** @brief Renders a model using path B (field_B_0=1 pipeline, character models).
 *
 * Computes fog visibility, sets up color/lighting per env mode, transforms vertices,
 * computes per-normal lighting, and assembles GT3/GT4 primitives.
 *
 * @param modelInfo Model to render.
 * @param otTag OT tag base.
 * @param otShift Depth shift.
 * @param mat World-to-view matrix.
 */
void Model_RenderPathB(s_ModelInfo* modelInfo, GsOT_TAG* otTag, void* otShift, MATRIX* mat);
```

### func_8005A42C → Model_ColorSetupEnv0
**Line:** 2716
**Original:**
```c
void func_8005A42C(s_GteScratchData* scratchData, q19_12 alpha);
```
**Proposed:**
```c
/** @brief Sets up flat vertex color for path B when env mode is 0 (no directional light).
 *
 * @param scratchData Scratchpad to store computed base color.
 * @param visibility Q12 fog-derived visibility factor.
 */
void Model_ColorSetupEnv0(s_GteScratchData* scratchData, q19_12 visibility);
```

### func_8005A478 → Model_ColorSetupEnv1
**Line:** 2726
**Original:**
```c
void func_8005A478(s_GteScratchData* scratchData, q19_12 alpha);
```
**Proposed:**
```c
/** @brief Sets up GTE lighting for path B env mode 1 (specular/positional lighting).
 *
 * @param scratchData Scratchpad for intermediate calculations.
 * @param visibility Q12 fog-derived visibility factor.
 */
void Model_ColorSetupEnv1(s_GteScratchData* scratchData, q19_12 visibility);
```

### func_8005A838 → Model_ColorSetupEnv2
**Line:** 2856
**Original:**
```c
void func_8005A838(s_GteScratchData* scratchData, s32 scale);
```
**Proposed:**
```c
/** @brief Sets up GTE lighting for path B env mode 2 (directional lighting).
 *
 * @param scratchData Scratchpad (unused beyond scope).
 * @param scale Q12 visibility/intensity scale.
 */
void Model_ColorSetupEnv2(s_GteScratchData* scratchData, s32 scale);
```

### func_8005A900 → Mesh_VertexTransformPathB
**Line:** 2871
**Original:**
```c
void func_8005A900(s_MeshHeader* meshHdr, s32 offset, s_GteScratchData* scratchData, MATRIX* mat);
```
**Proposed:**
```c
/** @brief Transforms mesh vertices to screen space for path B rendering.
 *
 * @param meshHdr Mesh to transform.
 * @param offset Vertex buffer offset in scratchpad.
 * @param scratchData Scratchpad for output.
 * @param mat Model-to-screen matrix.
 */
void Mesh_VertexTransformPathB(s_MeshHeader* meshHdr, s32 offset, s_GteScratchData* scratchData, MATRIX* mat);
```

### func_8005AA08 → Mesh_NormalLightingCompute
**Line:** 2906
**Original:**
```c
u8 func_8005AA08(s_MeshHeader* meshHdr, s32 arg1, s_GteScratchData2* scratchData);
```
**Proposed:**
```c
/** @brief Computes per-normal lighting colors for path B using GTE nct.
 *
 * Expands packed 8-bit normals to 13-bit fixed-point, processes 3 per GTE call,
 * stores RGB color triples into scratchData.
 *
 * @param meshHdr Mesh with normals to light.
 * @param normalOffset Offset into the lighting color buffer.
 * @param scratchData Scratchpad with normal expansion and color output buffers.
 */
u8 Mesh_NormalLightingCompute(s_MeshHeader* meshHdr, s32 normalOffset, s_GteScratchData2* scratchData);
```

### func_8005AC50 → Mesh_PrimitiveAssemblePathB
**Line:** 2979
**Original:**
```c
void func_8005AC50(s_MeshHeader* meshHdr, s_GteScratchData2* scratchData, GsOT_TAG* ot, s32 arg3);
```
**Proposed:**
```c
/** @brief Assembles GT3/GT4 primitives from path B transformed/lit mesh data.
 *
 * Determines tri vs quad by checking vertex index 3 == 0xFF. Uses per-normal
 * colors when env mode != 0, otherwise flat color. Backface culling via GTE nclip.
 *
 * @param meshHdr Mesh to assemble.
 * @param scratchData Scratchpad with projected vertices, depths, and lighting colors.
 * @param ot Ordering table tag array.
 * @param otShift Depth shift for OT insertion.
 */
void Mesh_PrimitiveAssemblePathB(s_MeshHeader* meshHdr, s_GteScratchData2* scratchData, GsOT_TAG* ot, s32 otShift);
```

### func_8005B378 → Texture_Reserve
**Line:** 3209
**Original:**
```c
void func_8005B378(s_Texture* tex, char* arg1);
```
**Proposed:**
```c
/** @brief Reserves a texture slot by setting its reference count and name.
 *
 * @param tex Texture slot to reserve.
 * @param name 8-character texture name.
 */
void Texture_Reserve(s_Texture* tex, char* name);
```

### func_8005B424 → Vector3_CopyOrClear
**Line:** 3245
**Original:**
```c
void func_8005B424(VECTOR3* vec0, VECTOR3* vec1);
```
**Proposed:**
```c
/** @brief Clears the destination vector, then copies the source if non-NULL.
 *
 * @param dest Destination vector (always zeroed first).
 * @param src Source vector to copy, or NULL to just clear.
 */
void Vector3_CopyOrClear(VECTOR3* dest, VECTOR3* src);
```

### func_8005B55C → Gfx_SnowParticlesInit
**Line:** 3297
**Original:**
```c
void func_8005B55C(GsCOORDINATE2* coord);
```
**Proposed:**
```c
/** @brief Initializes the snow/rain particle system.
 *
 * Precomputes position and direction vectors for 26 particle entries
 * using sin/cos from each particle's angle and radius.
 *
 * @param coord Coordinate system reference for particle positioning.
 */
void Gfx_SnowParticlesInit(GsCOORDINATE2* coord);
```

---

## 2. Effects & Flashlight

**File:** `src/bodyprog/gfx/bodyprog_8003E5E8.c`

### func_8003E740 → Gfx_FlameEffectDraw
**Line:** 94
**Original:**
```c
void func_8003E740(void);
```
**Proposed:**
```c
/** @brief Draws a flame/lighter effect attached to Harry's right hand bone.
 *
 * Calculates screen position from hand bone matrix, applies random jitter for flicker,
 * submits a semi-transparent POLY_FT4 with the flame texture.
 */
void Gfx_FlameEffectDraw(void);
```

### func_8003EDA8 → Gfx_LightingOverrideSet
**Line:** 329
**Original:**
```c
void func_8003EDA8(void);
```
**Proposed:**
```c
/** @brief Sets the flashlight system's override flag for a one-time lighting param override. */
void Gfx_LightingOverrideSet(void);
```

### func_8003EDB8 → Gfx_AmbientColorGet
**Line:** 334
**Original:**
```c
void func_8003EDB8(CVECTOR* color0, CVECTOR* color1);
```
**Proposed:**
```c
/** @brief Retrieves the two ambient colors for the current flashlight state.
 *
 * @param color0 Output: first ambient color.
 * @param color1 Output: second ambient color.
 */
void Gfx_AmbientColorGet(CVECTOR* color0, CVECTOR* color1);
```

### func_8003EE30 → Gfx_MapEffectsTransitionStart
**Line:** 340
**Original:**
```c
void func_8003EE30(s32 arg0, s32* arg1, s32 arg2, s32 arg3);
```
**Proposed:**
```c
/** @brief Begins a lighting/fog transition driven by an external variable.
 *
 * @param arg0 Unused.
 * @param transitionVar Pointer to a 32-bit variable that drives transition progress.
 * @param rangeStart Minimum value of the transition variable.
 * @param rangeEnd Maximum value (transition completes here).
 */
void Gfx_MapEffectsTransitionStart(s32 arg0, s32* transitionVar, s32 rangeStart, s32 rangeEnd);
```

### func_8003F4DC → Gfx_SpotlightDirectionCalc
**Line:** 509
**Original:**
```c
q19_12 func_8003F4DC(GsCOORDINATE2** coords, SVECTOR* rot, q19_12 alpha, s32 arg3, u32 arg4, s_SysWork* sysWork);
```
**Proposed:**
```c
/** @brief Calculates spotlight direction and source coordinate based on light mode.
 *
 * Mode 1 (flashlight) uses player bone coord. Modes 0, 2-5 use fixed angles.
 *
 * @param coords Output: pointer to light source coordinate (or NULL for fixed).
 * @param rot Output: light direction as unit vector from pitch/yaw.
 * @param alpha Light intensity alpha.
 * @param lightMode Light mode selector.
 * @param lightSource Light source type.
 * @param sysWork System work for bone coordinates.
 * @return Adjusted alpha value.
 */
q19_12 Gfx_SpotlightDirectionCalc(GsCOORDINATE2** coords, SVECTOR* rot, q19_12 alpha, s32 lightMode, u32 lightSource, s_SysWork* sysWork);
```

### func_8003F654 → Gfx_TransitionVariableRead
**Line:** 587
**Original:**
```c
u32 func_8003F654(s_SysWork_2388* arg0);
```
**Proposed:**
```c
/** @brief Reads the transition-driving variable, casting by primType_0.
 *
 * @param effectsState Effects state containing the pointer and type.
 * @return Current value of the transition variable.
 */
u32 Gfx_TransitionVariableRead(s_SysWork_2388* effectsState);
```

### func_8003F6F0 → Gfx_TransitionProgressCalc
**Line:** 614
**Original:**
```c
s32 func_8003F6F0(s32 arg0, s32 arg1, s32 arg2);
```
**Proposed:**
```c
/** @brief Calculates normalized transition progress as Q19.12 [0.0, 1.0].
 *
 * @param value Current value.
 * @param rangeStart Start of range.
 * @param rangeEnd End of range.
 * @return Normalized progress.
 */
s32 Gfx_TransitionProgressCalc(s32 value, s32 rangeStart, s32 rangeEnd);
```

### func_8003F838 → Gfx_MapEffectsInterpolate
**Line:** 650
**Original:**
```c
void func_8003F838(s_StructUnk3* arg0, s_StructUnk3* arg1, s_StructUnk3* arg2, q19_12 weight);
```
**Proposed:**
```c
/** @brief Interpolates between two map effects presets (fog, ambient, tint, etc.).
 *
 * Core function driving Silent Hill's atmosphere system.
 *
 * @param result Output interpolated effect.
 * @param from Source effect preset.
 * @param to Target effect preset.
 * @param weight Interpolation weight (Q19.12, 0.0=from, 1.0=to).
 */
void Gfx_MapEffectsInterpolate(s_StructUnk3* result, s_StructUnk3* from, s_StructUnk3* to, q19_12 weight);
```

### func_8003FCB0 → Gfx_AmbientColorsInterpolate
**Line:** 809
**Original:**
```c
void func_8003FCB0(const s_MapEffectsInfo* arg0, const s_MapEffectsInfo* arg1, const s_MapEffectsInfo* arg2, q19_12 alphaTo);
```
**Proposed:**
```c
/** @brief Interpolates two pairs of ambient light colors between effects presets.
 *
 * @param result Output effects info with interpolated colors.
 * @param from Source effects info.
 * @param to Target effects info.
 * @param weight Interpolation weight (Q19.12).
 */
void Gfx_AmbientColorsInterpolate(const s_MapEffectsInfo* result, const s_MapEffectsInfo* from, const s_MapEffectsInfo* to, q19_12 weight);
```

### func_8003FD38 → Gfx_FogParamsInterpolate
**Line:** 818
**Original:**
```c
void func_8003FD38(s_StructUnk3* arg0, s_StructUnk3* arg1, s_StructUnk3* arg2, q19_12 weight0, q19_12 weight1, q19_12 alphaTo);
```
**Proposed:**
```c
/** @brief Interpolates fog parameters between two effect presets.
 *
 * @param result Output interpolated fog.
 * @param from Source preset.
 * @param to Target preset.
 * @param weight0 Weight for brightness and view distance.
 * @param weight1 Weight for fog draw distance.
 * @param colorWeight Weight for fog color blending.
 */
void Gfx_FogParamsInterpolate(s_StructUnk3* result, s_StructUnk3* from, s_StructUnk3* to, q19_12 weight0, q19_12 weight1, q19_12 colorWeight);
```

### func_8003FE04 → Gfx_ScreenTintInterpolate
**Line:** 836
**Original:**
```c
void func_8003FE04(const s_MapEffectsInfo* arg0, const s_MapEffectsInfo* arg1, const s_MapEffectsInfo* arg2, q19_12 alphaTo);
```
**Proposed:**
```c
/** @brief Interpolates screen tint colors between two effects presets.
 *
 * @param result Output effects info.
 * @param from Source.
 * @param to Target.
 * @param weight Interpolation weight.
 */
void Gfx_ScreenTintInterpolate(const s_MapEffectsInfo* result, const s_MapEffectsInfo* from, const s_MapEffectsInfo* to, q19_12 weight);
```

### func_8003FEC0 → Gfx_FogDistanceCalc
**Line:** 855
**Original:**
```c
s32 func_8003FEC0(const s_MapEffectsInfo* arg0);
```
**Proposed:**
```c
/** @brief Calculates effective fog distance based on env mode and preset.
 *
 * @param effectsInfo Effects info containing the fog distance.
 * @return Effective fog distance (Q19.12). 20.0 = no fog.
 */
s32 Gfx_FogDistanceCalc(const s_MapEffectsInfo* effectsInfo);
```

### func_8003FF2C → Gfx_ApplyEffectsToRenderer
**Line:** 878
**Original:**
```c
void func_8003FF2C(s_StructUnk3* arg0);
```
**Proposed:**
```c
/** @brief Pushes computed map effects to the rendering subsystem.
 *
 * Final output stage: applies brightness, world tint, fog, screen tint, spotlight.
 *
 * @param effects Computed effects parameters to apply.
 */
void Gfx_ApplyEffectsToRenderer(s_StructUnk3* effects);
```

---

## 3. World GFX & Character Models

**File:** `src/bodyprog/gfx/bodyprog_8003BE50.c`

### func_8003BED0 → GameFs_BgItemProcessLoad
**Line:** 41
**Original:**
```c
void func_8003BED0(void);
```
**Proposed:**
```c
/** @brief Checks if background item LM file finished loading, fixes offsets and applies textures.
 *
 * Only runs once per load (guarded by isLoaded_2). Applies "TIM00" at tpage (0,15)
 * and "BG_ETC" at tpage (0,12).
 */
void GameFs_BgItemProcessLoad(void);
```

### func_8003CC7C → WorldObject_ModelDraw
**Line:** 506
**Original:**
```c
void func_8003CC7C(s_WorldObjectModel* arg0, MATRIX* arg1, MATRIX* arg2);
```
**Proposed:**
```c
/** @brief Draws a single world object model with LM index validation.
 *
 * @param model World object model containing model info and LM metadata.
 * @param viewMat View/projection matrix.
 * @param lightMat Light matrix.
 */
void WorldObject_ModelDraw(s_WorldObjectModel* model, MATRIX* viewMat, MATRIX* lightMat);
```

### func_8003D01C → WorldGfx_HeldItemShow
**Line:** 762
**Original:**
```c
void func_8003D01C(void);
```
**Proposed:**
```c
/** @brief Makes the player's held item model visible (clears bit 31 of bone field_0). */
void WorldGfx_HeldItemShow(void);
```

### func_8003D03C → WorldGfx_HeldItemHide
**Line:** 767
**Original:**
```c
void func_8003D03C(void);
```
**Proposed:**
```c
/** @brief Makes the player's held item model invisible (sets bit 31 of bone field_0). */
void WorldGfx_HeldItemHide(void);
```

### func_8003DA9C → WorldGfx_CharaDraw
**Line:** 1174
**Original:**
```c
void func_8003DA9C(e_CharacterId charaId, GsCOORDINATE2* coord, s32 arg2, q3_12 timer, s32 arg4);
```
**Proposed:**
```c
/** @brief Top-level character rendering entry point.
 *
 * Sets up lighting/tinting from world environment, optionally draws held item,
 * calls Skeleton_Draw for the character's skeleton.
 *
 * @param charaId Which character to draw (e.g., Chara_Harry).
 * @param coord Array of bone coordinate transforms from animation.
 * @param otIdx Ordering table layer index.
 * @param tintBlend Blend factor for world tint (Q3.12, 0=full tint, 1=no tint).
 * @param paletteIdx Palette index packed into rendering flags.
 */
void WorldGfx_CharaDraw(e_CharacterId charaId, GsCOORDINATE2* coord, s32 otIdx, q3_12 tintBlend, s32 paletteIdx);
```

### func_8003DD74 → WorldGfx_CharaRenderFlagsPack
**Line:** 1215
**Original:**
```c
s32 func_8003DD74(e_CharacterId charaId, s32 arg1);
```
**Proposed:**
```c
/** @brief Packs a palette index into a rendering flags format.
 *
 * @param charaId Character ID (unused).
 * @param paletteIdx Palette index to encode.
 * @return Packed rendering flags (palette in bits 10-15).
 */
s32 WorldGfx_CharaRenderFlagsPack(e_CharacterId charaId, s32 paletteIdx);
```

### func_8003DE60 → WorldGfx_HarryAttachmentSet
**Line:** 1273
**Original:**
```c
void func_8003DE60(s_Skeleton* skel, s32 arg1);
```
**Proposed:**
```c
/** @brief Sets which weapon/accessory models are visible on Harry's skeleton.
 *
 * @param skel Harry's skeleton.
 * @param attachmentBits Packed descriptor: low nibble = right-hand weapon, high nibble = left-hand accessory.
 */
void WorldGfx_HarryAttachmentSet(s_Skeleton* skel, s32 attachmentBits);
```

### func_8003DF84 → WorldGfx_CybilAttachmentSet
**Line:** 1338
**Original:**
```c
void func_8003DF84(s_Skeleton* skel, s32 arg1);
```
**Proposed:**
```c
/** @brief Sets which weapon/accessory models are visible on Cybil's skeleton.
 *
 * @param skel Cybil's skeleton.
 * @param attachmentBits Packed descriptor with two 4-bit indices.
 */
void WorldGfx_CybilAttachmentSet(s_Skeleton* skel, s32 attachmentBits);
```

### func_8003E08C → WorldGfx_MonsterCybilAttachmentSet
**Line:** 1382
**Original:**
```c
void func_8003E08C(s_Skeleton* skel, s32 arg1);
```
**Proposed:**
```c
/** @brief Sets which model parts are visible on Monster Cybil's skeleton.
 *
 * @param skel Monster Cybil's skeleton.
 * @param attachmentBits Packed descriptor with two 4-bit indices.
 */
void WorldGfx_MonsterCybilAttachmentSet(s_Skeleton* skel, s32 attachmentBits);
```

### func_8003E194 → WorldGfx_DahliaAttachmentSet
**Line:** 1426
**Original:**
```c
void func_8003E194(s_Skeleton* skel, s32 arg1);
```
**Proposed:**
```c
/** @brief Sets which model parts are visible on Dahlia's skeleton.
 *
 * @param skel Dahlia's skeleton.
 * @param attachmentBits Packed descriptor (low nibble only, 3 variants).
 */
void WorldGfx_DahliaAttachmentSet(s_Skeleton* skel, s32 attachmentBits);
```

### func_8003E238 → WorldGfx_KaufmannAttachmentSet
**Line:** 1457
**Original:**
```c
void func_8003E238(s_Skeleton* skel, s32 arg1);
```
**Proposed:**
```c
/** @brief Sets which model parts are visible on Kaufmann's skeleton.
 *
 * Most complex: 4 right-hand variants, 3 left-hand variants.
 *
 * @param skel Kaufmann's skeleton.
 * @param attachmentBits Packed descriptor with two 4-bit indices.
 */
void WorldGfx_KaufmannAttachmentSet(s_Skeleton* skel, s32 attachmentBits);
```

### func_8003E388 → WorldGfx_StalkerAttachmentSet
**Line:** 1520
**Original:**
```c
void func_8003E388(s_Skeleton* skel, s32 arg1);
```
**Proposed:**
```c
/** @brief Sets which model parts are visible on the Stalker enemy skeleton.
 *
 * @param skel Stalker's skeleton.
 * @param attachmentBits Packed descriptor (low nibble, 2 variants).
 */
void WorldGfx_StalkerAttachmentSet(s_Skeleton* skel, s32 attachmentBits);
```

### func_8003E414 → WorldGfx_SplitHeadAttachmentSet
**Line:** 1545
**Original:**
```c
void func_8003E414(s_Skeleton* skel, s32 arg1);
```
**Proposed:**
```c
/** @brief Sets which model parts are visible on the Split Head boss skeleton.
 *
 * @param skel Split Head's skeleton.
 * @param attachmentBits Attachment state (low 2 bits: 1=closed jaw, 2=open jaw).
 */
void WorldGfx_SplitHeadAttachmentSet(s_Skeleton* skel, s32 attachmentBits);
```

### func_8003E4A0 → WorldGfx_PuppetNurseAttachmentSet
**Line:** 1572
**Original:**
```c
void func_8003E4A0(s_Skeleton* skel, s32 arg1);
```
**Proposed:**
```c
/** @brief Sets which model parts are visible on the Puppet Nurse enemy skeleton.
 *
 * @param skel Puppet Nurse's skeleton.
 * @param attachmentBits Packed descriptor (low nibble, 3 variants).
 */
void WorldGfx_PuppetNurseAttachmentSet(s_Skeleton* skel, s32 attachmentBits);
```

### func_8003E544 → WorldGfx_PuppetDoctorAttachmentSet
**Line:** 1604
**Original:**
```c
void func_8003E544(s_Skeleton* skel, s32 arg1);
```
**Proposed:**
```c
/** @brief Sets which model parts are visible on the Puppet Doctor enemy skeleton.
 *
 * @param skel Puppet Doctor's skeleton.
 * @param attachmentBits Packed descriptor (low nibble, 3 variants).
 */
void WorldGfx_PuppetDoctorAttachmentSet(s_Skeleton* skel, s32 attachmentBits);
```

---

## 4. Animation & Skeleton

**File:** `src/bodyprog/gfx/bodyprog_anim_800445A4.c`

### func_80044918 → Anim_ActiveInfoGet
**Line:** 168
**Original:**
```c
s_AnimInfo* func_80044918(s_ModelAnim* anim);
```
**Proposed:**
```c
/** @brief Resolves the currently active s_AnimInfo entry for a model's animation state.
 *
 * Uses status_0 as index into animInfo_C, with optional secondary table (animInfo_10).
 *
 * @param anim The model's animation state.
 * @return Active s_AnimInfo entry.
 */
s_AnimInfo* Anim_ActiveInfoGet(s_ModelAnim* anim);
```

### func_80044950 → Anim_CharaPlaybackDispatch
**Line:** 191
**Original:**
```c
void func_80044950(s_SubCharacter* chara, s_AnmHeader* anmHdr, GsCOORDINATE2* coords);
```
**Proposed:**
```c
/** @brief Top-level animation tick dispatcher for NPCs/sub-characters.
 *
 * Resolves active anim info and invokes its playback function pointer.
 *
 * @param chara Sub-character to animate.
 * @param anmHdr Animation file header with bone count, keyframe data, bind poses.
 * @param coords Array of GsCOORDINATE2 bone transforms to update.
 */
void Anim_CharaPlaybackDispatch(s_SubCharacter* chara, s_AnmHeader* anmHdr, GsCOORDINATE2* coords);
```

### func_80044F14 → Bone_ApplyRotation
**Line:** 470
**Original:**
```c
void func_80044F14(GsCOORDINATE2* coord, q3_12 rotZ, q3_12 rotX, q19_12 rotY);
```
**Proposed:**
```c
/** @brief Applies a ZXY Euler rotation to a bone's coordinate matrix.
 *
 * Used for player upper-body flex (aiming, looking) and weapon bone positioning.
 *
 * @param coord The bone coordinate to rotate.
 * @param rotZ Z-axis rotation angle (Q3.12).
 * @param rotX X-axis rotation angle (Q3.12).
 * @param rotY Y-axis rotation angle (Q19.12).
 */
void Bone_ApplyRotation(GsCOORDINATE2* coord, q3_12 rotZ, q3_12 rotX, q19_12 rotY);
```

### func_80045014 → Skeleton_BoneFlagsClear
**Line:** 522
**Original:**
```c
void func_80045014(s_Skeleton* skel);
```
**Proposed:**
```c
/** @brief Clears all bone flags to zero in a skeleton, resetting visibility and state.
 *
 * @param skel Skeleton whose bone flags to clear.
 */
void Skeleton_BoneFlagsClear(s_Skeleton* skel);
```

### func_8004506C → Skeleton_AutoAssignModels
**Line:** 533
**Original:**
```c
void func_8004506C(s_Skeleton* skel, s_LmHeader* lmHdr);
```
**Proposed:**
```c
/** @brief Automatically assigns LM model data to skeleton bones based on LM header model count.
 *
 * @param skel Skeleton to set up.
 * @param lmHdr Loaded model (PLM) header with model count and data.
 */
void Skeleton_AutoAssignModels(s_Skeleton* skel, s_LmHeader* lmHdr);
```

### func_80045108 → Skeleton_SetupBoneModels
**Line:** 563
**Original:**
```c
void func_80045108(s_Skeleton* skel, s_LmHeader* lmHdr, s8* arg2, s32 arg3);
```
**Proposed:**
```c
/** @brief Sets up skeleton bone-to-model assignments and render order linked list.
 *
 * @param skel Skeleton to configure.
 * @param lmHdr LM header containing model data.
 * @param boneHierarchy Byte array describing model-to-bone mapping (terminated by -2).
 * @param isAppend If 0, resets skeleton first; nonzero appends to existing assignments.
 */
void Skeleton_SetupBoneModels(s_Skeleton* skel, s_LmHeader* lmHdr, s8* boneHierarchy, s32 isAppend);
```

### func_80045258 → Skeleton_BoneRenderOrderBuild
**Line:** 604
**Original:**
```c
void func_80045258(s_LinkedBone** boneOrd, s_LinkedBone* bones, s32 boneIdx, s_LmHeader* lmHdr);
```
**Proposed:**
```c
/** @brief Builds the skeleton's bone render order linked list from LM model order.
 *
 * @param boneOrd Pointer to linked list head to append to.
 * @param bones Array of s_LinkedBone entries.
 * @param boneCount Number of bones.
 * @param lmHdr LM header with modelOrder_10 array.
 */
void Skeleton_BoneRenderOrderBuild(s_LinkedBone** boneOrd, s_LinkedBone* bones, s32 boneCount, s_LmHeader* lmHdr);
```

### func_800452EC → Skeleton_BoneIdxFromModelName
**Line:** 625
**Original:**
```c
void func_800452EC(s_Skeleton* skel);
```
**Proposed:**
```c
/** @brief Parses two-digit decimal numbers from model header name strings into bone indices.
 *
 * @param skel Skeleton whose bones should have field_10 indices populated.
 */
void Skeleton_BoneIdxFromModelName(s_Skeleton* skel);
```

### func_80045360 → Skeleton_BoneIdxAssignFromList
**Line:** 655
**Original:**
```c
void func_80045360(s_Skeleton* skel, s8* arg1);
```
**Proposed:**
```c
/** @brief Directly assigns bone index values from a provided list.
 *
 * @param skel Skeleton to update.
 * @param boneIdxList Byte array of bone index values (terminated by -2).
 */
void Skeleton_BoneIdxAssignFromList(s_Skeleton* skel, s8* boneIdxList);
```

### func_800453E8 → Skeleton_SetAllBonesVisible
**Line:** 667
**Original:**
```c
void func_800453E8(s_Skeleton* skel, bool cond);
```
**Proposed:**
```c
/** @brief Sets or clears the visibility flag (bit 31) on all bones in a skeleton.
 *
 * @param skel Skeleton to update.
 * @param visible If true, bones become visible (bit 31 cleared); false hides them.
 */
void Skeleton_SetAllBonesVisible(s_Skeleton* skel, bool visible);
```

### func_80045468 → Skeleton_SetBonesVisibleByList
**Line:** 685
**Original:**
```c
void func_80045468(s_Skeleton* skel, s32* arg1, bool cond);
```
**Proposed:**
```c
/** @brief Sets visibility for specific bones identified by a bone index list.
 *
 * Used for weapon/accessory toggling on character models.
 *
 * @param skel Skeleton to update.
 * @param boneIdxList Compact bone index list (terminated by -2).
 * @param visible If true, shows bones; false hides them.
 */
void Skeleton_SetBonesVisibleByList(s_Skeleton* skel, s32* boneIdxList, bool visible);
```

### func_80045534 → Skeleton_Draw
**Line:** 711
**Original:**
```c
void func_80045534(s_Skeleton* skel, GsOT* ot, s32 arg2, GsCOORDINATE2* coord, q3_12 arg4, u16 arg5, s_FsImageDesc* images);
```
**Proposed:**
```c
/** @brief Main skeleton rendering loop.
 *
 * Iterates bone render order, computes world-space transforms, submits each bone's
 * model to GPU. Also computes screen-space bounding box for fog overlay.
 *
 * @param skel Skeleton to render.
 * @param ot Ordering table.
 * @param otIdx OT layer index.
 * @param coord Bone coordinate transforms from animation.
 * @param shadowRadius Character shadow size (Q3.12).
 * @param flags Rendering flags.
 * @param images Optional s_FsImageDesc array for bounding points (NULL-terminated).
 */
void Skeleton_Draw(s_Skeleton* skel, GsOT* ot, s32 otIdx, GsCOORDINATE2* coord, q3_12 shadowRadius, u16 flags, s_FsImageDesc* images);
```

---

## 5. Player System

**File:** `src/bodyprog/bodyprog_800706E4.c`

### func_800706E4 → Player_AnimSpeedGet
**Line:** 63
**Original:**
```c
s32 func_800706E4(s_Model* model);
```
**Proposed:**
```c
/** @brief Returns animation playback speed for the current player state.
 *
 * Used as variableFunc callback in s_AnimInfo. Handles boss grab states,
 * exhaustion, and analog stick deflection scaling.
 *
 * @param model Player model whose anim speed is queried.
 * @return Animation speed (Q19.12).
 */
s32 Player_AnimSpeedGet(s_Model* model);
```

### func_80070B84 → Player_WalkDistanceUpdate
**Line:** 206
**Original:**
```c
void func_80070B84(s_SubCharacter* chara, q19_12 moveDistMax, q19_12 arg2, s32 keyframeIdx);
```
**Proposed:**
```c
/** @brief Smoothly interpolates player movement distance toward target based on stick input.
 *
 * @param chara Player character.
 * @param moveDistMax Base maximum movement distance.
 * @param moveDistExtended Extended maximum distance (interpolated with stick).
 * @param keyframeIdx Minimum keyframe before acceleration applies.
 */
void Player_WalkDistanceUpdate(s_SubCharacter* chara, q19_12 moveDistMax, q19_12 moveDistExtended, s32 keyframeIdx);
```

### func_80070CF0 → Player_RunDistanceUpdate
**Line:** 249
**Original:**
```c
void func_80070CF0(s_SubCharacter* chara, q19_12 arg1, q19_12 moveDistMax, q19_12 moveDistForward, q19_12 modeDistBack);
```
**Proposed:**
```c
/** @brief Updates movement distance during running in specific keyframe windows.
 *
 * @param chara Player character.
 * @param moveDistMin Minimum distance threshold.
 * @param moveDistMax Maximum distance at full stick deflection.
 * @param moveDistForward Acceleration rate.
 * @param moveDistBack Deceleration rate.
 */
void Player_RunDistanceUpdate(s_SubCharacter* chara, q19_12 moveDistMin, q19_12 moveDistMax, q19_12 moveDistForward, q19_12 moveDistBack);
```

### func_80070DF0 → Player_KickStompUpdate
**Line:** 284
**Original:**
```c
void func_80070DF0(s_PlayerExtra* extra, s_SubCharacter* chara, s32 weaponAttack, s32 animStatus);
```
**Proposed:**
```c
/** @brief Handles kick/stomp attack state machine for downed enemies.
 *
 * Rotates toward target NPC, activates weapon hitbox, applies displacement,
 * transitions back to PlayerState_None on completion.
 *
 * @param extra Player extra data.
 * @param chara Player character.
 * @param weaponAttack Weapon attack index into D_800AD4C8.
 * @param animStatus Kick or stomp animation status to play.
 */
void Player_KickStompUpdate(s_PlayerExtra* extra, s_SubCharacter* chara, s32 weaponAttack, s32 animStatus);
```

### func_80071620 → Player_VocalSfxPlay
**Line:** 534
**Original:**
```c
bool func_80071620(u32 animStatus, s_SubCharacter* chara, s32 keyframeIdx, e_SfxId sfxId);
```
**Proposed:**
```c
/** @brief Plays a one-shot vocal/body sound at a specific animation keyframe.
 *
 * @param animStatus Required animation status for trigger.
 * @param chara Player character.
 * @param keyframeIdx Keyframe at which the sound triggers.
 * @param sfxId Sound effect to play.
 * @return true if the sound was triggered.
 */
bool Player_VocalSfxPlay(u32 animStatus, s_SubCharacter* chara, s32 keyframeIdx, e_SfxId sfxId);
```

### func_8007B924 → Player_MovementAudioAndExhaustion
**Line:** 6276
**Original:**
```c
void func_8007B924(s_SubCharacter* chara, s_PlayerExtra* extra);
```
**Proposed:**
```c
/** @brief Copies moveDistance to D_800C4550, manages exhaustion timer, dispatches footstep SFX.
 *
 * Critical function — removing its call breaks all player movement because
 * D_800C4550 never gets set.
 *
 * @param chara Player character.
 * @param extra Player extra data.
 */
void Player_MovementAudioAndExhaustion(s_SubCharacter* chara, s_PlayerExtra* extra);
```

### func_8007C0D8 → Player_CollisionAndMovement
**Line:** 6549
**Original:**
```c
void func_8007C0D8(s_SubCharacter* chara, s_PlayerExtra* extra, GsCOORDINATE2* coords);
```
**Proposed:**
```c
/** @brief Main collision-based movement function.
 *
 * Converts speed + heading into position delta, adjusts for terrain slope,
 * performs wall detection, applies position offset, detects ledge falls.
 *
 * @param chara Player character.
 * @param extra Player extra data.
 * @param coords Bone coordinate array to update.
 */
void Player_CollisionAndMovement(s_SubCharacter* chara, s_PlayerExtra* extra, GsCOORDINATE2* coords);
```

### func_8007D090 → Player_TorsoFlexUpdate
**Line:** 7114
**Original:**
```c
void func_8007D090(s_SubCharacter* chara, s_PlayerExtra* extra, GsCOORDINATE2* coords);
```
**Proposed:**
```c
/** @brief Applies upper-body flex rotation to torso and arms.
 *
 * During combat: points torso toward locked-on enemy.
 * During normal movement: interpolates flex back to zero.
 *
 * @param chara Player character.
 * @param extra Player extra data.
 * @param coords Bone coordinate array to modify.
 */
void Player_TorsoFlexUpdate(s_SubCharacter* chara, s_PlayerExtra* extra, GsCOORDINATE2* coords);
```

### func_8007D6F0 → Player_WallAheadCheck
**Line:** 7364
**Original:**
```c
s32 func_8007D6F0(s_SubCharacter* chara, s_800C45C8* arg1);
```
**Proposed:**
```c
/** @brief Casts two parallel rays forward to detect walls ahead of the player.
 *
 * @param chara Player character.
 * @param collisionData Output collision data.
 * @return Appropriate lower body state for wall response.
 */
s32 Player_WallAheadCheck(s_SubCharacter* chara, s_800C45C8* collisionData);
```

### func_8007E860 → Player_AnimInfoOverlayPatch
**Line:** 7841
**Original:**
```c
void func_8007E860(void);
```
**Proposed:**
```c
/** @brief Copies 8 map-specific animation entries into Harry's anim table at index 92. */
void Player_AnimInfoOverlayPatch(void);
```

### func_8007E8C0 → Player_MapOverlayAnimInit
**Line:** 7853
**Original:**
```c
void func_8007E8C0(void);
```
**Proposed:**
```c
/** @brief Initializes player animations for a new map overlay.
 *
 * Copies map-specific anim infos into Harry's table, resets exhaustion, collision box,
 * and player state.
 */
void Player_MapOverlayAnimInit(void);
```

### func_8007E9C4 → Player_StateReset
**Line:** 7887
**Original:**
```c
void func_8007E9C4(void);
```
**Proposed:**
```c
/** @brief Complete reset of all player state.
 *
 * Clears state, movement flags, input, damage, flex rotation, exhaustion, targeting.
 */
void Player_StateReset(void);
```

### func_8007F14C → Player_WeaponEquipSfxPlay
**Line:** 8135
**Original:**
```c
void func_8007F14C(u8 weaponAttack);
```
**Proposed:**
```c
/** @brief Plays the weapon equip/ready sound effect.
 *
 * @param weaponAttack Weapon attack ID to play equip sound for.
 */
void Player_WeaponEquipSfxPlay(u8 weaponAttack);
```

### func_8007F95C → Player_CanStompOrKick
**Line:** 8371
**Original:**
```c
bool func_8007F95C(void);
```
**Proposed:**
```c
/** @brief Checks if there is a downed enemy near Harry that can be kicked or stomped.
 *
 * Checks distance, height, health, and facing angle for all NPCs.
 *
 * @return true if a valid stomp/kick target exists.
 */
bool Player_CanStompOrKick(void);
```

### func_8007FB94 → Player_MapOverlayAnimSet
**Line:** 8473
**Original:**
```c
void func_8007FB94(s_SubCharacter* chara, s_PlayerExtra* extra, s32 animStatus);
```
**Proposed:**
```c
/** @brief Searches map overlay anim table for a matching status and sets the animation.
 *
 * @param chara Player character.
 * @param extra Player extra data.
 * @param animStatus Animation status to search for.
 */
void Player_MapOverlayAnimSet(s_SubCharacter* chara, s_PlayerExtra* extra, s32 animStatus);
```

### func_8007FC48 → Player_MapOverlayAnimSetActive
**Line:** 8508
**Original:**
```c
void func_8007FC48(s_SubCharacter* chara, s_PlayerExtra* extra, s32 animStatus);
```
**Proposed:**
```c
/** @brief Like Player_MapOverlayAnimSet but marks the animation as active (status+1).
 *
 * @param chara Player character.
 * @param extra Player extra data.
 * @param animStatus Animation status to search for.
 */
void Player_MapOverlayAnimSetActive(s_SubCharacter* chara, s_PlayerExtra* extra, s32 animStatus);
```

### func_8007FD2C → Player_Field104Get
**Line:** 8544
**Original:**
```c
s32 func_8007FD2C(void);
```
**Proposed:**
```c
/** @brief Getter for the player's field_104 property value. */
s32 Player_Field104Get(void);
```

### func_8007FD4C → Player_GrabStateReset
**Line:** 8554
**Original:**
```c
void func_8007FD4C(bool cond);
```
**Proposed:**
```c
/** @brief Resets all state related to enemy grab attacks.
 *
 * @param resetCollisionBox If true, restores collision box to defaults.
 */
void Player_GrabStateReset(bool resetCollisionBox);
```

### func_8007FDE0 → Player_FloorSfxGet
**Line:** 8585
**Original:**
```c
void func_8007FDE0(s8 arg0, e_SfxId* sfxId, s8* pitch0, s8* pitch1);
```
**Proposed:**
```c
/** @brief Determines footstep SFX and pitch variation based on floor material and map.
 *
 * @param floorType Floor material ID from collision data.
 * @param sfxId Output: footstep SFX to play.
 * @param pitch0 Output: pitch variation for walk footsteps.
 * @param pitch1 Output: pitch variation for run footsteps.
 */
void Player_FloorSfxGet(s8 floorType, e_SfxId* sfxId, s8* pitch0, s8* pitch1);
```

### func_800803FC → Player_SpawnPositionGet
**Line:** 8765
**Original:**
```c
void func_800803FC(VECTOR3* pos, s32 idx);
```
**Proposed:**
```c
/** @brief Retrieves a spawn position from the map overlay's spawn table.
 *
 * @param pos Output position vector.
 * @param idx Spawn point index.
 */
void Player_SpawnPositionGet(VECTOR3* pos, s32 idx);
```

### func_80080478 → Math_AngleBetweenPoints3D
**Line:** 8783
**Original:**
```c
q19_12 func_80080478(const VECTOR3* pos0, const VECTOR3* pos1);
```
**Proposed:**
```c
/** @brief Computes packed angle pair (horizontal + vertical pitch) between two 3D points.
 *
 * @param pos0 Source position.
 * @param pos1 Target position.
 * @return Packed angles: (pitch << 16) | horizontal.
 */
q19_12 Math_AngleBetweenPoints3D(const VECTOR3* pos0, const VECTOR3* pos1);
```

### func_80080540 → Math_Magnitude3D
**Line:** 8819
**Original:**
```c
s32 func_80080540(s32 arg0, s32 arg1, s32 arg2);
```
**Proposed:**
```c
/** @brief Computes sum of squared magnitudes of three Q19.12 values via MIPS 64-bit multiply.
 *
 * @param x First component.
 * @param y Second component.
 * @param z Third component.
 * @return Squared magnitude.
 */
s32 Math_Magnitude3D(s32 x, s32 y, s32 z);
```

### func_800805BC → Math_BoneWorldPositions
**Line:** 8861
**Original:**
```c
void func_800805BC(VECTOR3* pos, SVECTOR* rot, GsCOORDINATE2* rootCoord, s32 arg3);
```
**Proposed:**
```c
/** @brief Transforms local-space bone positions into world-space via GTE rotation/translation.
 *
 * @param pos Output array of world-space positions (Q19.12).
 * @param rot Input array of local-space offsets (SVECTOR).
 * @param rootCoord Root bone coordinate for hierarchy matrix.
 * @param count Number of positions to transform.
 */
void Math_BoneWorldPositions(VECTOR3* pos, SVECTOR* rot, GsCOORDINATE2* rootCoord, s32 count);
```

### func_800806AC → Collision_FloorTypeCheck
**Line:** 8886
**Original:**
```c
bool func_800806AC(s32 arg0, s32 arg1, s32 arg2, s32 arg3);
```
**Proposed:**
```c
/** @brief Checks if the floor at a position has a specific type and is above a Y threshold.
 *
 * @param floorBitmask Bitmask of accepted floor types.
 * @param posX X position.
 * @param posY Y height threshold.
 * @param posZ Z position.
 * @return true if floor type matches and is above posY.
 */
bool Collision_FloorTypeCheck(s32 floorBitmask, s32 posX, s32 posY, s32 posZ);
```

### func_8008074C → Collision_FloorTypeCheckAny
**Line:** 8919
**Original:**
```c
bool func_8008074C(s32 arg0, s32 arg1, s32 arg2, s32 arg3);
```
**Proposed:**
```c
/** @brief Checks floor type at position regardless of height.
 *
 * @param floorBitmask Bitmask of accepted floor types.
 * @param posX X position.
 * @param posY Unused (overridden internally).
 * @param posZ Z position.
 * @return true if floor type matches.
 */
bool Collision_FloorTypeCheckAny(s32 floorBitmask, s32 posX, s32 posY, s32 posZ);
```

### func_800808AC → Collision_FloorTypeGet
**Line:** 9007
**Original:**
```c
s32 func_800808AC(q19_12 posX, q19_12 posZ);
```
**Proposed:**
```c
/** @brief Returns the floor type at a given XZ position.
 *
 * @param posX X position (Q19.12).
 * @param posZ Z position (Q19.12).
 * @return Floor type from collision data.
 */
s32 Collision_FloorTypeGet(q19_12 posX, q19_12 posZ);
```

---

## 6. Collision System

**File:** `src/bodyprog/bodyprog_800697EC.c`

### func_800697EC → Collision_Init
**Line:** 23
**Original:**
```c
void func_800697EC(void);
```
**Proposed:**
```c
/** @brief Initializes the collision subsystem, resetting flags and clearing the query counter. */
void Collision_Init(void);
```

### func_80069810 → Collision_FlagsGet
**Line:** 29
**Original:**
```c
u16 func_80069810(void);
```
**Proposed:**
```c
/** @brief Returns the current collision flags. */
u16 Collision_FlagsGet(void);
```

### func_80069820 → Collision_FlagsSet
**Line:** 34
**Original:**
```c
void func_80069820(u16 flags);
```
**Proposed:**
```c
/** @brief Sets the collision flags, replacing previous flags entirely.
 *
 * @param flags New collision flags value.
 */
void Collision_FlagsSet(u16 flags);
```

### func_8006982C → Collision_FlagsSetBits
**Line:** 39
**Original:**
```c
void func_8006982C(u16 flags);
```
**Proposed:**
```c
/** @brief Enables additional collision flags by OR.
 *
 * @param flags Flag bits to enable.
 */
void Collision_FlagsSetBits(u16 flags);
```

### func_80069844 → Collision_FlagsClearBitsKeepBase
**Line:** 44
**Original:**
```c
void func_80069844(s32 arg0);
```
**Proposed:**
```c
/** @brief Clears specified flag bits while preserving bit 0 (base collision flag).
 *
 * @param flags Bitmask of flags to clear.
 */
void Collision_FlagsClearBitsKeepBase(s32 flags);
```

### func_80069860 → Collision_TriggerZonesUpdate
**Line:** 49
**Original:**
```c
void func_80069860(s32 arg0, s32 arg1, s_func_8006F8FC* arg2);
```
**Proposed:**
```c
/** @brief Scans trigger zones and collects those containing the given XZ position.
 *
 * @param posX Query X position (Q19.12).
 * @param posZ Query Z position (Q19.12).
 * @param zones Null-terminated array of trigger zone definitions.
 */
void Collision_TriggerZonesUpdate(s32 posX, s32 posZ, s_func_8006F8FC* zones);
```

### func_80069994 → IpdCollData_VisitedCounterIncrement
**Line:** 95
**Original:**
```c
void func_80069994(s_IpdCollisionData* collData);
```
**Proposed:**
```c
/** @brief Increments visited-counter for collision deduplication; wraps and clears at 252.
 *
 * @param collData IPD collision data block.
 */
void IpdCollData_VisitedCounterIncrement(s_IpdCollisionData* collData);
```

### func_800699E4 → IpdCollData_VisitedCounterIncrementSimple
**Line:** 112
**Original:**
```c
void func_800699E4(s_IpdCollisionData* collData);
```
**Proposed:**
```c
/** @brief Increments visited-counter without wrap-around or array clear.
 *
 * @param collData IPD collision data block.
 */
void IpdCollData_VisitedCounterIncrementSimple(s_IpdCollisionData* collData);
```

### func_80069B24 → Collision_WallDetect
**Line:** 166
**Original:**
```c
s32 func_80069B24(s_800C4590* arg0, VECTOR3* offset, s_SubCharacter* chara);
```
**Proposed:**
```c
/** @brief Top-level wall collision detection using scratchpad stack for performance.
 *
 * @param result Output collision result.
 * @param offset Movement offset vector.
 * @param chara Character being tested.
 * @return Wall collision result code.
 */
s32 Collision_WallDetect(s_800C4590* result, VECTOR3* offset, s_SubCharacter* chara);
```

### func_80069BA8 → Collision_WallResponse
**Line:** 177
**Original:**
```c
s32 func_80069Ba8(s_800C4590* arg0, VECTOR3* offset, s_SubCharacter* chara, s32 arg4);
```
**Proposed:**
```c
/** @brief Handles wall collision response by sampling ground at 9 points around character.
 *
 * @param result Collision result to modify.
 * @param offset Movement offset.
 * @param chara Character being tested.
 * @param collisionResult Initial collision pass result.
 * @return Wall response code.
 */
s32 Collision_WallResponse(s_800C4590* result, VECTOR3* offset, s_SubCharacter* chara, s32 collisionResult);
```

### func_80069DF0 → Collision_GroundProbeRadial
**Line:** 297
**Original:**
```c
void func_80069DF0(s_800C4590* arg0, const VECTOR3* pos, s32 arg2, s32 arg3);
```
**Proposed:**
```c
/** @brief Probes ground height at 16 radial points to determine terrain slope direction.
 *
 * Creates terrain avoidance force pushing away from ledges/steep drops.
 *
 * @param result Output: XZ offset vector (1/16 scale) pointing away from highest terrain.
 * @param pos Center position (Q19.12).
 * @param currentHeight Current character Y position.
 * @param angle Base angle offset for probe circle.
 */
void Collision_GroundProbeRadial(s_800C4590* result, const VECTOR3* pos, s32 currentHeight, s32 angle);
```

### func_80069FFC → Collision_OffsetApply
**Line:** 365
**Original:**
```c
s32 func_80069FFC(s_800C4590* arg0, VECTOR3* offset, s_SubCharacter* chara);
```
**Proposed:**
```c
/** @brief Applies collision detection for a character's movement offset.
 *
 * @param result Output collision result.
 * @param offset Movement offset to test.
 * @param chara Character performing movement.
 * @return Collision result code.
 */
s32 Collision_OffsetApply(s_800C4590* result, VECTOR3* offset, s_SubCharacter* chara);
```

### func_8006A178 → Collision_ResultSetDefault
**Line:** 408
**Original:**
```c
void func_8006A178(s_800C4590* arg0, q19_12 posX, q19_12 posY, q19_12 posZ, q19_12 heightY);
```
**Proposed:**
```c
/** @brief Initializes a collision result with position and height, clearing all flags.
 *
 * @param result Collision result to initialize.
 * @param posX X offset.
 * @param posY Y offset.
 * @param posZ Z offset.
 * @param heightY Ground height.
 */
void Collision_ResultSetDefault(s_800C4590* result, q19_12 posX, q19_12 posY, q19_12 posZ, q19_12 heightY);
```

### func_8006A1A4 → Collision_BuildActiveCharacterList
**Line:** 420
**Original:**
```c
s_SubCharacter** func_8006A1A4(s32* charaCount, s_SubCharacter* chara, bool arg2);
```
**Proposed:**
```c
/** @brief Builds a static array of active characters for collision testing.
 *
 * @param charaCount Output: number of active characters found.
 * @param excludeChara Character to exclude (typically the one being tested).
 * @param includePlayer If true, applies filtering rules.
 * @return Pointer to static array of character pointers.
 */
s_SubCharacter** Collision_BuildActiveCharacterList(s32* charaCount, s_SubCharacter* excludeChara, bool includePlayer);
```

### func_8006A3B4 → Collision_TestOffset
**Line:** 472
**Original:**
```c
s32 func_8006A3B4(s32 arg0, VECTOR* offset, s_func_8006AB50* arg2);
```
**Proposed:**
```c
/** @brief Tests a movement offset against collision data using scratchpad stack.
 *
 * @param arg0 Collision result struct.
 * @param offset Movement offset to test.
 * @param query Collision query parameters.
 * @return Collision result.
 */
s32 Collision_TestOffset(s32 arg0, VECTOR* offset, s_func_8006AB50* query);
```

### func_8006A940 → Collision_NpcMovementDampen
**Line:** 653
**Original:**
```c
void func_8006A940(VECTOR3* offset, s_func_8006AB50* arg1, s_SubCharacter** charas, s32 charaCount);
```
**Proposed:**
```c
/** @brief Reduces movement offset based on proximity to nearby characters.
 *
 * @param offset Movement offset to dampen (modified in place).
 * @param query Collision query with position/rotation.
 * @param charas Active character array.
 * @param charaCount Number of characters.
 */
void Collision_NpcMovementDampen(VECTOR3* offset, s_func_8006AB50* query, s_SubCharacter** charas, s32 charaCount);
```

### func_8006AB50 → Collision_QueryInit
**Line:** 724
**Original:**
```c
void func_8006AB50(s_func_8006CC44* arg0, VECTOR3* pos, s_func_8006AB50* arg2, s32 arg3);
```
**Proposed:**
```c
/** @brief Initializes collision query state for a new pass.
 *
 * @param state Collision state to initialize.
 * @param pos Position offset.
 * @param query Input query parameters.
 * @param arg3 Configuration flag.
 */
void Collision_QueryInit(s_func_8006CC44* state, VECTOR3* pos, s_func_8006AB50* query, s32 arg3);
```

### func_8006ABC0 → Collision_QueryDirectionCalc
**Line:** 745
**Original:**
```c
void func_8006ABC0(s_func_8006ABC0* result, VECTOR3* pos, s_func_8006AB50* arg2);
```
**Proposed:**
```c
/** @brief Calculates movement direction vector and distance from a position offset.
 *
 * @param result Output movement direction and position data.
 * @param pos Movement offset (Q19.12).
 * @param query Source position and rotation data.
 */
void Collision_QueryDirectionCalc(s_func_8006ABC0* result, VECTOR3* pos, s_func_8006AB50* query);
```

### func_8006AD44 → Collision_ProcessIpdData
**Line:** 781
**Original:**
```c
void func_8006AD44(s_func_8006CC44* arg0, s_IpdCollisionData* collData);
```
**Proposed:**
```c
/** @brief Processes an IPD collision data block against the current query.
 *
 * @param state Active collision state.
 * @param collData IPD collision data to test against.
 */
void Collision_ProcessIpdData(s_func_8006CC44* state, s_IpdCollisionData* collData);
```

### func_8006AEAC → Collision_BoundsCheckAndSetup
**Line:** 828
**Original:**
```c
bool func_8006AEAC(s_func_8006CC44* arg0, s_IpdCollisionData* collData);
```
**Proposed:**
```c
/** @brief Performs initial bounds check for collision query against IPD data.
 *
 * @param state Collision state to populate.
 * @param collData IPD collision data.
 * @return true if query is within bounds.
 */
bool Collision_BoundsCheckAndSetup(s_func_8006CC44* state, s_IpdCollisionData* collData);
```

### func_8006B004 → Collision_GridRangeCalc
**Line:** 863
**Original:**
```c
bool func_8006B004(s_func_8006CC44* arg0, s_IpdCollisionData* collData);
```
**Proposed:**
```c
/** @brief Calculates grid cell range intersected by the collision sweep line.
 *
 * @param state Collision state (receives cell range).
 * @param collData IPD collision data with grid dimensions.
 * @return false if sweep is entirely outside grid.
 */
bool Collision_GridRangeCalc(s_func_8006CC44* state, s_IpdCollisionData* collData);
```

### func_8006B6E8 → Collision_GroundHeightCellUpdate
**Line:** 1071
**Original:**
```c
void func_8006B6E8(s_func_8006CC44* arg0, s_IpdCollisionData_20* arg1);
```
**Proposed:**
```c
/** @brief Updates ground height tracking for a specific collision cell.
 *
 * @param state Collision state with edge data.
 * @param gridCell Grid cell being processed.
 */
void Collision_GroundHeightCellUpdate(s_func_8006CC44* state, s_IpdCollisionData_20* gridCell);
```

### func_8006B7E0 → Collision_GroundHeightShouldUpdate
**Line:** 1133
**Original:**
```c
bool func_8006B7E0(s_func_8006CC44_A8* arg0, s_func_8006CC44_CC_20* arg1);
```
**Proposed:**
```c
/** @brief Determines whether a candidate ground height should replace the current one.
 *
 * @param current Current best ground height entry.
 * @param candidate New candidate to compare.
 * @return true if candidate is closer and should replace.
 */
bool Collision_GroundHeightShouldUpdate(s_func_8006CC44_A8* current, s_func_8006CC44_CC_20* candidate);
```

### func_8006B9C8 → Collision_WallEdgeProcess
**Line:** 1230
**Original:**
```c
void func_8006B9C8(s_func_8006CC44* arg0);
```
**Proposed:**
```c
/** @brief Processes a detected wall edge for collision response.
 *
 * @param state Collision state with edge data.
 */
void Collision_WallEdgeProcess(s_func_8006CC44* state);
```

### func_8006BB50 → Collision_WallSlideResponse
**Line:** 1262
**Original:**
```c
void func_8006BB50(s_func_8006CC44* arg0, s32 arg1);
```
**Proposed:**
```c
/** @brief Calculates wall-sliding direction when hitting a wall edge.
 *
 * @param state Collision state.
 * @param responseType 0 for direct, 1 for soft/buffered response.
 */
void Collision_WallSlideResponse(s_func_8006CC44* state, s32 responseType);
```

### func_8006BC34 → Collision_HeightAboveEdge
**Line:** 1293
**Original:**
```c
s32 func_8006BC34(s_func_8006CC44* arg0);
```
**Proposed:**
```c
/** @brief Calculates height of query position above a collision edge.
 *
 * @param state Collision state with edge data.
 * @return Signed height difference (positive = above).
 */
s32 Collision_HeightAboveEdge(s_func_8006CC44* state);
```

### func_8006BCC4 → Collision_WallHitRegister
**Line:** 1334
**Original:**
```c
void func_8006BCC4(s_func_8006CC44_44* arg0, s8* arg1, u32 arg2, q3_12 deltaX, q3_12 deltaZ, s16 arg5);
```
**Proposed:**
```c
/** @brief Registers a wall hit in the collision hit accumulator.
 *
 * @param wallHits Wall hit accumulator.
 * @param hitCounter Collision data hit counter.
 * @param hitType Response slot (0=direct push, 1=slide, 2=secondary push).
 * @param deltaX Direction X.
 * @param deltaZ Direction Z.
 * @param pushDist Push distance.
 */
void Collision_WallHitRegister(s_func_8006CC44_44* wallHits, s8* hitCounter, u32 hitType, q3_12 deltaX, q3_12 deltaZ, s16 pushDist);
```

### func_8006BDDC → Collision_AngleRangeAccumulate
**Line:** 1377
**Original:**
```c
void func_8006BDDC(s_func_8006CC44_44_0* arg0, q3_12 rotX, q3_12 rotY);
```
**Proposed:**
```c
/** @brief Accumulates a wall normal angle into an angle range.
 *
 * @param angleRange Angle range to update.
 * @param rotX Angle X to add.
 * @param rotY Angle Y to add.
 */
void Collision_AngleRangeAccumulate(s_func_8006CC44_44_0* angleRange, q3_12 rotX, q3_12 rotY);
```

### func_8006BE40 → Collision_EdgeBlockResponse
**Line:** 1391
**Original:**
```c
void func_8006BE40(s_func_8006CC44* arg0);
```
**Proposed:**
```c
/** @brief Determines collision response for a blocking edge hit.
 *
 * Classifies hit as left vertex, right vertex, or edge face.
 *
 * @param state Collision state with edge data.
 */
void Collision_EdgeBlockResponse(s_func_8006CC44* state);
```

### func_8006BF88 → Collision_VertexSphereResponse
**Line:** 1470
**Original:**
```c
void func_8006BF88(s_func_8006CC44* arg0, SVECTOR3* arg1);
```
**Proposed:**
```c
/** @brief Handles collision response against a single edge vertex as a sphere.
 *
 * @param state Collision state.
 * @param vertex Position of the vertex.
 */
void Collision_VertexSphereResponse(s_func_8006CC44* state, SVECTOR3* vertex);
```

### func_8006C0C8 → Collision_EdgeFaceResponse
**Line:** 1494
**Original:**
```c
void func_8006C0C8(s_func_8006CC44* arg0, s16 arg1, s16 arg2);
```
**Proposed:**
```c
/** @brief Handles collision response when hitting the face of a wall edge.
 *
 * @param state Collision state.
 * @param hitTime Fractional time along movement where hit occurs.
 * @param hitPosition Position along the edge face.
 */
void Collision_EdgeFaceResponse(s_func_8006CC44* state, s16 hitTime, s16 hitPosition);
```

### func_8006C1B8 → Collision_ShouldReplaceHit
**Line:** 1516
**Original:**
```c
bool func_8006C1B8(u32 arg0, s16 arg1, s_func_8006CC44* arg2);
```
**Proposed:**
```c
/** @brief Determines whether a new collision hit should replace an existing one.
 *
 * @param hitType Type of new hit (1=edge, 2=vertex).
 * @param hitTime Fractional time for new hit.
 * @param state Current collision state with existing hit data.
 * @return true if new hit should replace.
 */
bool Collision_ShouldReplaceHit(u32 hitType, s16 hitTime, s_func_8006CC44* state);
```

### func_8006C248 → Collision_LineSphereIntersect
**Line:** 1562
**Original:**
```c
s16 func_8006C248(s32 arg0, s16 arg1, q3_12 deltaX, q3_12 deltaZ, s16 arg4);
```
**Proposed:**
```c
/** @brief Tests intersection between a movement line segment and a collision sphere.
 *
 * @param rotMatrix Packed GTE rotation for line direction.
 * @param lineLength Length of line segment.
 * @param deltaX Vector from line start to sphere center X.
 * @param deltaZ Vector from line start to sphere center Z.
 * @param radius Sphere collision radius.
 * @return Fractional intersection position (Q19.12), or NO_VALUE if none.
 */
s16 Collision_LineSphereIntersect(s32 rotMatrix, s16 lineLength, q3_12 deltaX, q3_12 deltaZ, s16 radius);
```

### func_8006C3D4 → Collision_CircleObstacleSetup
**Line:** 1614
**Original:**
```c
bool func_8006C3D4(s_func_8006CC44* arg0, s_IpdCollisionData* collData, s32 idx);
```
**Proposed:**
```c
/** @brief Sets up collision data for a circular obstacle (column/pillar).
 *
 * @param state Collision state to populate.
 * @param collData IPD collision data containing the obstacle.
 * @param idx Obstacle index.
 * @return false if obstacle's collision flag is inactive.
 */
bool Collision_CircleObstacleSetup(s_func_8006CC44* state, s_IpdCollisionData* collData, s32 idx);
```

### func_8006C45C → Collision_CircleObstacleResponse
**Line:** 1635
**Original:**
```c
void func_8006C45C(s_func_8006CC44* arg0);
```
**Proposed:**
```c
/** @brief Handles collision response against a circular obstacle.
 *
 * @param state Collision state.
 */
void Collision_CircleObstacleResponse(s_func_8006CC44* state);
```

### func_8006C794 → Collision_CircleObstaclePush
**Line:** 1735
**Original:**
```c
void func_8006C794(s_func_8006CC44* arg0, s32 arg1, s32 dist);
```
**Proposed:**
```c
/** @brief Registers a radial push response when inside a circular obstacle's zone.
 *
 * @param state Collision state.
 * @param pushType 0 for immediate, 1 for buffered.
 * @param dist Current distance to obstacle center.
 */
void Collision_CircleObstaclePush(s_func_8006CC44* state, s32 pushType, s32 dist);
```

### func_8006CB90 → Collision_SlopeFactorCalc
**Line:** 1855
**Original:**
```c
s16 func_8006CB90(s_func_8006CC44* arg0);
```
**Proposed:**
```c
/** @brief Calculates slope scaling factor (1.0=flat, <1.0=steep).
 *
 * @param state Collision state with ground height data.
 * @return Slope factor (Q19.12).
 */
s16 Collision_SlopeFactorCalc(s_func_8006CC44* state);
```

### func_8006CC44 → Collision_GroundHeightInterpolate
**Line:** 1875
**Original:**
```c
s32 func_8006CC44(q23_8 x, q23_8 z, s_func_8006CC44* arg2);
```
**Proposed:**
```c
/** @brief Interpolates ground height at XZ using collision surface normal.
 *
 * @param x X position (Q23.8).
 * @param z Z position (Q23.8).
 * @param state Collision state with ground plane data.
 * @return Interpolated ground height.
 */
s32 Collision_GroundHeightInterpolate(q23_8 x, q23_8 z, s_func_8006CC44* state);
```

### func_8006CC9C → Collision_CharacterCollisionTest
**Line:** 1887
**Original:**
```c
void func_8006CC9C(s_func_8006CC44* arg0);
```
**Proposed:**
```c
/** @brief Tests collision query against a character's collision cylinder.
 *
 * @param state Collision state with character data.
 */
void Collision_CharacterCollisionTest(s_func_8006CC44* state);
```

### func_8006D01C → Collision_ResolveMovement
**Line:** 1994
**Original:**
```c
void func_8006D01C(VECTOR3* arg0, VECTOR3* arg1, s16 arg2, s_func_8006CC44* arg3);
```
**Proposed:**
```c
/** @brief Resolves final movement offset after collision detection.
 *
 * Splits movement at hit point, projects remainder along wall surface.
 *
 * @param resolvedOffset Output: safe movement to apply.
 * @param remainingOffset In/out: remaining movement after collision.
 * @param slopeFactor Slope scaling factor.
 * @param state Collision state with hit data.
 */
void Collision_ResolveMovement(VECTOR3* resolvedOffset, VECTOR3* remainingOffset, s16 slopeFactor, s_func_8006CC44* state);
```

### func_8006D2B4 → Collision_WallDeflect
**Line:** 2076
**Original:**
```c
void func_8006D2B4(VECTOR3* arg0, s_func_8006CC44_44* arg1);
```
**Proposed:**
```c
/** @brief Deflects a movement vector based on accumulated wall hits.
 *
 * @param offset Movement offset to deflect (modified in place).
 * @param wallHits Accumulated wall hit data with angle ranges.
 */
void Collision_WallDeflect(VECTOR3* offset, s_func_8006CC44_44* wallHits);
```

### func_8006D600 → Collision_ConstrainToArc
**Line:** 2236
**Original:**
```c
void func_8006D600(VECTOR3* pos, s32 arg1, s32 arg2, s32 arg3, s32 arg4);
```
**Proposed:**
```c
/** @brief Constrains a movement vector to an allowed angular arc.
 *
 * @param pos Movement vector to constrain (modified in place).
 * @param wallAngle Center angle of the wall.
 * @param arcStart Start angle of allowed arc.
 * @param arcEnd End angle of allowed arc.
 * @param pushDist Maximum push distance along wall.
 */
void Collision_ConstrainToArc(VECTOR3* pos, s32 wallAngle, s32 arcStart, s32 arcEnd, s32 pushDist);
```

### func_8006D774 → Collision_RetrySetup
**Line:** 2302
**Original:**
```c
void func_8006D774(s_func_8006CC44* arg0, VECTOR3* arg1, VECTOR3* arg2);
```
**Proposed:**
```c
/** @brief Prepares collision state for retry pass after wall deflection.
 *
 * @param state Collision state to reset.
 * @param resolvedOffset Movement already applied.
 * @param remainingOffset Remaining movement after deflection.
 */
void Collision_RetrySetup(s_func_8006CC44* state, VECTOR3* resolvedOffset, VECTOR3* remainingOffset);
```

### func_8006DA08 → Ray_TraceWithCharacters
**Line:** 2390
**Original:**
```c
bool func_8006DA08(s_RayData* ray, VECTOR3* from, VECTOR3* dir, s_SubCharacter* chara);
```
**Proposed:**
```c
/** @brief Ray trace against world geometry and active characters.
 *
 * @param ray Output hit result.
 * @param from Ray origin (Q19.12).
 * @param dir Ray direction (Q19.12).
 * @param excludeChara Character to exclude (typically attacker).
 * @return true if ray hit something.
 */
bool Ray_TraceWithCharacters(s_RayData* ray, VECTOR3* from, VECTOR3* dir, s_SubCharacter* excludeChara);
```

### func_8006DB3C → Ray_TraceExcludeCharacter
**Line:** 2435
**Original:**
```c
bool func_8006DB3C(s_RayData* ray, VECTOR3* from, VECTOR3* dir, s_SubCharacter* chara);
```
**Proposed:**
```c
/** @brief Ray trace with player filter applied.
 *
 * @param ray Output hit result.
 * @param from Ray origin.
 * @param dir Ray direction.
 * @param chara Character to exclude.
 * @return true if hit.
 */
bool Ray_TraceExcludeCharacter(s_RayData* ray, VECTOR3* from, VECTOR3* dir, s_SubCharacter* chara);
```

### func_8006DC18 → Ray_TraceWorldOnly
**Line:** 2462
**Original:**
```c
bool func_8006DC18(s_RayData* ray, VECTOR3* vec1, VECTOR3* vec2);
```
**Proposed:**
```c
/** @brief Ray trace against world geometry only (no characters).
 *
 * @param ray Output hit result.
 * @param from Ray origin.
 * @param dir Ray direction.
 * @return true if hit.
 */
bool Ray_TraceWorldOnly(s_RayData* ray, VECTOR3* from, VECTOR3* dir);
```

### func_8006E0AC → Ray_SetupIpdCollisionBounds
**Line:** 2592
**Original:**
```c
void func_8006E0AC(s_RayState* state, s_IpdCollisionData* arg1);
```
**Proposed:**
```c
/** @brief Sets up ray state coordinates relative to IPD collision data origin.
 *
 * @param state Ray state to populate.
 * @param collData IPD collision data to test against.
 */
void Ray_SetupIpdCollisionBounds(s_RayState* state, s_IpdCollisionData* collData);
```

### func_8006E150 → Ray_GridCellTraversal
**Line:** 2608
**Original:**
```c
void func_8006E150(s_func_8006E490* arg0, DVECTOR arg1, DVECTOR arg2);
```
**Proposed:**
```c
/** @brief DDA grid traversal to find all cells a ray crosses (max 20).
 *
 * @param gridState Grid traversal state (receives visited cells).
 * @param startDir Packed XZ start direction.
 * @param endDir Packed XZ end direction.
 */
void Ray_GridCellTraversal(s_func_8006E490* gridState, DVECTOR startDir, DVECTOR endDir);
```

### func_8006E490 → Ray_GridCellRecord
**Line:** 2732
**Original:**
```c
void func_8006E490(s_func_8006E490* arg0, u32 flags, q19_12 posX, q19_12 posZ);
```
**Proposed:**
```c
/** @brief Records a single grid cell visit during ray traversal with axis un-mirroring.
 *
 * @param gridState Grid traversal state.
 * @param flags Axis transformation flags (0=flip X, 1=flip Z, 2=swap XZ).
 * @param posX Cell X coordinate.
 * @param posZ Cell Z coordinate.
 */
void Ray_GridCellRecord(s_func_8006E490* gridState, u32 flags, q19_12 posX, q19_12 posZ);
```

---

## 7. Game State Machine

**File:** `src/bodyprog/events/game_sys_states.c`

### func_8003943C → Game_SoundCleanupForMenuTransition
**Line:** 349
**Original:**
```c
void func_8003943C(void);
```
**Proposed:**
```c
/** @brief Performs sound/ambient cleanup when transitioning to inventory or map menu.
 *
 * Stops ambient sounds, weapon audio, and per-map-overlay ambient SFX.
 */
void Game_SoundCleanupForMenuTransition(void);
```

### func_80039F90 → AreaLoad_GetTransitionFlags
**Line:** 688
**Original:**
```c
s8 func_80039F90(void);
```
**Proposed:**
```c
/** @brief Returns area transition visual effect flags if a transition is active.
 *
 * @return Transition effect flags, or 0 if no transition.
 */
s8 AreaLoad_GetTransitionFlags(void);
```

### func_8003A16C → Game_AutosaveUpdate
**Line:** 767
**Original:**
```c
void func_8003A16C(void);
```
**Proposed:**
```c
/** @brief Updates autosave slot with current position, rotation, and health.
 *
 * Guards behind demo/video mode flag to avoid overwriting during playback.
 */
void Game_AutosaveUpdate(void);
```

---

## 8. Loading Screen

**File:** `src/bodyprog/game_boot/load_screen.c`

### func_80035B98 → Screen_ItemInspectionDraw
**Line:** 34
**Original:**
```c
void func_80035B98(void);
```
**Proposed:**
```c
/** @brief Draws the item inspection background image to the screen. */
void Screen_ItemInspectionDraw(void);
```

---

## 9. View / Camera System

### File: `src/bodyprog/view/vw_main.c`

### func_80048E3C → Vw_LineSegmentIntersectionCheck
**Line:** 165
**Original:**
```c
s16 func_80048E3C(s16 arg0, s16 arg1, s16 arg2, s16 arg3, s16 arg4);
```
**Proposed:**
```c
/** @brief Computes parametric intersection ratio of a line segment against a bounded interval.
 *
 * Used in collision for computing how far a moving edge must be clipped.
 * Returns Q12(1.0) if no intersection, Q12(0.0) if at boundary.
 *
 * @param segmentLength Length/extent of the segment.
 * @param segmentDir Direction component of the line segment.
 * @param distToBound Signed distance to the boundary.
 * @param boundsMin Minimum bound of valid interval.
 * @param boundsMax Maximum bound of valid interval.
 * @return Q12 ratio.
 */
s16 Vw_LineSegmentIntersectionCheck(s16 segmentLength, s16 segmentDir, s16 distToBound, s16 boundsMin, s16 boundsMax);
```

### File: `src/bodyprog/view/vw_calc.c`

### func_800494B0 → Vw_ClampedSpeedToTarget
**Line:** 179
**Original:**
```c
s32 func_800494B0(s32 arg0, s32 arg1, s32 arg2);
```
**Proposed:**
```c
/** @brief Frame-rate-independent clamped interpolation speed calculator.
 *
 * @param currentVal Current value.
 * @param targetVal Target value.
 * @param maxRate Maximum rate of change per unit time.
 * @return Clamped speed.
 */
s32 Vw_ClampedSpeedToTarget(s32 currentVal, s32 targetVal, s32 maxRate);
```

### func_80049530 → Vw_TransformAndProjectPoint
**Line:** 196
**Original:**
```c
s32 func_80049530(VECTOR* arg0, DVECTOR* arg1);
```
**Proposed:**
```c
/** @brief Projects a 3D world position to 2D screen coordinates via GTE with translation accumulation.
 *
 * Contains inline MIPS GTE assembly manipulating translation registers directly.
 *
 * @param worldPos 3D position to transform and project.
 * @param screenPos Output 2D screen coordinates.
 * @return Projected depth / 4.
 */
s32 Vw_TransformAndProjectPoint(VECTOR* worldPos, DVECTOR* screenPos);
```

### func_80049AF8 → Vw_CoordToViewSpaceMatrix
**Line:** 400
**Original:**
```c
void func_80049AF8(GsCOORDINATE2* rootCoord, MATRIX* outMat);
```
**Proposed:**
```c
/** @brief Computes view-space matrix for a coordinate hierarchy node.
 *
 * Gets cumulative world matrix, subtracts camera position, multiplies by VbWvsMatrix.
 *
 * @param coord Coordinate hierarchy node.
 * @param outViewMat Output view-space matrix.
 */
void Vw_CoordToViewSpaceMatrix(GsCOORDINATE2* coord, MATRIX* outViewMat);
```

### func_80049B6C → Vw_CoordToWorldAndViewMatrices
**Line:** 411
**Original:**
```c
void func_80049B6C(GsCOORDINATE2* rootCoord, MATRIX* outMat0, MATRIX* outMat1);
```
**Proposed:**
```c
/** @brief Computes both world-space and view-space matrices for a coordinate node.
 *
 * @param coord Coordinate hierarchy node.
 * @param outWorldMat Output world-space matrix.
 * @param outViewMat Output view-space matrix.
 */
void Vw_CoordToWorldAndViewMatrices(GsCOORDINATE2* coord, MATRIX* outWorldMat, MATRIX* outViewMat);
```

### func_80049C2C → Vw_MakeWorldScreenMatrixAtPosition
**Line:** 424
**Original:**
```c
void func_80049C2C(MATRIX* outMat, q19_12 posX, q19_12 posY, q19_12 posZ);
```
**Proposed:**
```c
/** @brief Constructs a world-to-screen matrix at a given world position.
 *
 * @param outMat Output world-screen matrix.
 * @param posX World X position (Q19.12).
 * @param posY World Y position (Q19.12).
 * @param posZ World Z position (Q19.12).
 */
void Vw_MakeWorldScreenMatrixAtPosition(MATRIX* outMat, q19_12 posX, q19_12 posY, q19_12 posZ);
```

### func_8004A54C → Vw_ScreenRegionSpanCheck
**Line:** 794
**Original:**
```c
bool func_8004A54C(s_func_8004A54C* arg0);
```
**Proposed:**
```c
/** @brief Checks if screen-space region flags span across the screen center.
 *
 * Returns true if flagged regions span opposite screen edges, meaning the
 * projected geometry crosses through the visible area.
 *
 * @param regionFlags 3x3 screen region occupancy flags.
 * @return true if geometry spans the screen center.
 */
bool Vw_ScreenRegionSpanCheck(s_Vw_ScreenRegionFlags* regionFlags);
```
**Struct rename:** `s_func_8004A54C` → `s_Vw_ScreenRegionFlags` (in `include/bodyprog/view/structs.h` line 193)

### File: `src/bodyprog/view/vc_main.c`

### func_80080A10 → Vc_CurRoadField15Get
**Line:** 103
**Original:**
```c
s32 func_80080A10(void);
```
**Proposed:**
```c
/** @brief Returns field_15 from the current camera road data.
 *
 * Likely a per-road lighting zone index or environmental attribute flag.
 * If field_15 is later identified, rename accordingly (e.g., Vc_CurRoadLightZoneGet).
 */
s32 Vc_CurRoadField15Get(void);
```

### func_80080B58 → Vc_AttachCameraToCoord
**Line:** 144
**Original:**
```c
void func_80080B58(GsCOORDINATE2* arg0, SVECTOR* rot, VECTOR3* pos);
```
**Proposed:**
```c
/** @brief Attaches camera view to a coordinate hierarchy node with rotation and position offset.
 *
 * Called from map cutscene scripts with bone coordinates (typically HarryBone_Head).
 *
 * @param parentCoord Coordinate hierarchy node to attach to (e.g., bone).
 * @param rotation Additional local rotation offset (Q3.12).
 * @param position World position override (Q19.12).
 */
void Vc_AttachCameraToCoord(GsCOORDINATE2* parentCoord, SVECTOR* rotation, VECTOR3* position);
```

### func_80080D68 → Vc_HeadViewModeActivate
**Line:** 222
**Original:**
```c
void func_80080D68(void);
```
**Proposed:**
```c
/** @brief Sets flag for one-frame head-mounted camera view.
 *
 * When active, vcSetDataToVwSystem places camera on player's head bone
 * with hardcoded offsets (0, -0.05, 0.3) and 180-degree Y rotation.
 */
void Vc_HeadViewModeActivate(void);
```

### func_8008150C → Vc_IsInSelfViewExclusionZone
**Line:** 473
**Original:**
```c
bool func_8008150C(q19_12 posX, q19_12 posZ);
```
**Proposed:**
```c
/** @brief Checks if position falls within a hardcoded zone where self-view is forbidden.
 *
 * Map-specific rectangles (likely doorways/narrow passages).
 *
 * @param posX World X position (Q19.12).
 * @param posZ World Z position (Q19.12).
 * @return true if self-view should be blocked.
 */
bool Vc_IsInSelfViewExclusionZone(q19_12 posX, q19_12 posZ);
```

### File: `src/bodyprog/view/vc_util.c`

### func_800401CC → Vc_HeadBoneCameraRequest
**Line:** 76
**Original:**
```c
void func_800401CC(void);
```
**Proposed:**
```c
/** @brief Wrapper that triggers head-attached camera mode.
 *
 * Public API entry point for map scripts. Calls Vc_HeadViewModeActivate.
 */
void Vc_HeadBoneCameraRequest(void);
```

---

## 10. Event Scripting

**File:** `src/bodyprog/bodyprog_80085D78.c`

### func_80085D78 → Event_StepAdvance
**Line:** 37
**Original:**
```c
void func_80085D78(bool reset);
```
**Proposed:**
```c
/** @brief Advances the event system's primary or secondary state step counter.
 *
 * @param secondary If true, advances secondary counter; otherwise primary.
 */
void Event_StepAdvance(bool secondary);
```

### func_80085DC0 → Event_StepSet
**Line:** 49
**Original:**
```c
void func_80085DC0(bool arg0, s32 sysStateStep);
```
**Proposed:**
```c
/** @brief Sets an event step counter to a specific value.
 *
 * @param secondary If true, sets secondary counter; otherwise primary.
 * @param step Value to set.
 */
void Event_StepSet(bool secondary, s32 step);
```

### func_80085DF0 → Event_WaitForPlayerStopOrTimeout
**Line:** 61
**Original:**
```c
void func_80085DF0(void);
```
**Proposed:**
```c
/** @brief Waits for player to stop moving or 1-second timeout, then advances step. */
void Event_WaitForPlayerStopOrTimeout(void);
```

### func_80085EB8 → Event_CharaAnimControl
**Line:** 79
**Original:**
```c
void func_80085EB8(u32 arg0, s_SubCharacter* chara, s32 arg2, bool reset);
```
**Proposed:**
```c
/** @brief Controls character animation during events.
 *
 * @param action 0=set anim, 1=wait for keyframe, 2=lock, 3=unlock, 4=unlock+reset.
 * @param chara Character to animate.
 * @param animIdx Animation index.
 * @param secondary Whether to use secondary event state.
 */
void Event_CharaAnimControl(u32 action, s_SubCharacter* chara, s32 animIdx, bool secondary);
```

### func_8008605C → Event_BranchOnEventFlag
**Line:** 151
**Original:**
```c
void func_8008605C(e_EventFlag eventFlagIdx, s32 stepTrue, s32 stepFalse, bool stepSecondary);
```
**Proposed:**
```c
/** @brief Checks a savegame event flag and branches to stepTrue or stepFalse.
 *
 * @param flagIdx Event flag to check.
 * @param stepTrue Step if flag is set.
 * @param stepFalse Step if flag is clear.
 * @param secondary Whether to use secondary state.
 */
void Event_BranchOnEventFlag(e_EventFlag flagIdx, s32 stepTrue, s32 stepFalse, bool secondary);
```

### func_800862F8 → Event_TextureLoadAndDisplay
**Line:** 290
**Original:**
```c
void func_800862F8(s32 stateStep, e_FsFile fileIdx, bool reset);
```
**Proposed:**
```c
/** @brief Multi-step helper for loading a TIM texture from disc and displaying it.
 *
 * Steps: 0=load, 1=wait, 2=draw, 3=store framebuffer, 4=alt load, 5=alt draw, 6=restore.
 */
void Event_TextureLoadAndDisplay(s32 stateStep, e_FsFile fileIdx, bool secondary);
```

### func_80086470 → Event_ItemModelLoadAndAdd
**Line:** 364
**Original:**
```c
void func_80086470(u32 stateStep, e_InventoryItemId itemId, s32 itemCount, bool reset);
```
**Proposed:**
```c
/** @brief Loads a unique item 3D model for inspection. Multi-step: 0=load, 1=wait, 2=add to inventory.
 *
 * @param stateStep Current state step.
 * @param itemId Inventory item to load.
 * @param itemCount Quantity to add.
 * @param secondary Whether to use secondary state.
 */
void Event_ItemModelLoadAndAdd(u32 stateStep, e_InventoryItemId itemId, s32 itemCount, bool secondary);
```

### func_800865FC → Event_WaypointSet
**Line:** 430
**Original:**
```c
void func_800865FC(bool isPos, s32 idx0, s32 idx1, q3_12 angleY, q19_12 offsetOrPosX, q19_12 offsetOrPosZ);
```
**Proposed:**
```c
/** @brief Sets a position waypoint for event character movement.
 *
 * Can be relative to player (offset) or absolute.
 */
void Event_WaypointSet(bool isAbsolute, s32 waypointGroup, s32 waypointIdx, q3_12 angleY, q19_12 posOrOffsetX, q19_12 posOrOffsetZ);
```

### func_800866D4 → Event_PlayerMoveToWaypoint
**Line:** 450
**Original:**
```c
void func_800866D4(s32 arg0, s32 arg1, bool reset);
```
**Proposed:**
```c
/** @brief Moves player toward waypoint, advances event step when complete. */
void Event_PlayerMoveToWaypoint(s32 arg0, s32 arg1, bool secondary);
```

### func_80086728 → Event_NpcMoveToWaypoint
**Line:** 458
**Original:**
```c
void func_80086728(s_SubCharacter* chara, s32 arg1, s32 arg2, bool reset);
```
**Proposed:**
```c
/** @brief Moves an NPC toward waypoint, advances event step when complete. */
void Event_NpcMoveToWaypoint(s_SubCharacter* chara, s32 arg1, s32 arg2, bool secondary);
```

### func_800868DC → Event_TimerReset
**Line:** 500
**Original:**
```c
void func_800868DC(s32 idx);
```
**Proposed:**
```c
/** @brief Resets an interpolation timer to zero.
 *
 * @param timerIdx Timer index (0-5).
 */
void Event_TimerReset(s32 timerIdx);
```

### func_800868F4 → Event_TimerLerp
**Line:** 505
**Original:**
```c
s32 func_800868F4(s32 arg0, s32 arg1, s32 idx);
```
**Proposed:**
```c
/** @brief Linear interpolation over time: (target * elapsed) / duration.
 *
 * @param targetValue Target value.
 * @param duration Duration in frames.
 * @param timerIdx Timer index.
 * @return Interpolated value.
 */
s32 Event_TimerLerp(s32 targetValue, s32 duration, s32 timerIdx);
```

### func_8008694C → Event_TimerSinLerp
**Line:** 513
**Original:**
```c
s32 func_8008694C(s32 arg0, s16 arg1, s16 arg2, s32 arg3, s32 idx);
```
**Proposed:**
```c
/** @brief Sinusoidal interpolation: amplitude * sin(startAngle + sweepAngle * t).
 *
 * @param amplitude Output amplitude.
 * @param startAngle Starting angle.
 * @param sweepAngle Angular sweep.
 * @param duration Duration in frames.
 * @param timerIdx Timer index.
 * @return Interpolated value.
 */
s32 Event_TimerSinLerp(s32 amplitude, s16 startAngle, s16 sweepAngle, s32 duration, s32 timerIdx);
```

### func_8008716C → Event_FullTextureViewWithDismiss
**Line:** 803
**Original:**
```c
void func_8008716C(e_FsFile texFileIdx, q19_12 fadeTimestep0, q19_12 fadeTimestep1);
```
**Proposed:**
```c
/** @brief Full event sequence: freeze player, fade in, show texture, wait for button, fade out, unfreeze.
 *
 * Used for viewing notes/photos in-game.
 */
void Event_FullTextureViewWithDismiss(e_FsFile texFileIdx, q19_12 fadeTimestepIn, q19_12 fadeTimestepOut);
```

---

## 11. Vibration / Haptics

**File:** `src/bodyprog/bodyprog_80089090.c`

### func_80089090 → Vibration_SetPadMode
**Line:** 19
**Original:**
```c
void func_80089090(s32 arg0);
```
**Proposed:**
```c
void Vibration_SetPadMode(s32 padMode);
```

### func_800890B8 → Vibration_Init
**Line:** 24
**Original:**
```c
void func_800890B8(void);
```
**Proposed:**
```c
/** @brief Initializes the vibration subsystem, linked list, and buffers. */
void Vibration_Init(void);
```

### func_80089128 → Vibration_Update
**Line:** 36
**Original:**
```c
s32 func_80089128(void);
```
**Proposed:**
```c
/** @brief Main vibration update loop. Processes active effects, applies motor output.
 * @return Active effect count.
 */
s32 Vibration_Update(void);
```

### func_800892A4 → Vibration_Play
**Line:** 197
**Original:**
```c
void func_800892A4(s32 idx);
```
**Proposed:**
```c
/** @brief Plays a vibration effect by table index at default intensity (0x80).
 * @param vibIdx Vibration table index.
 */
void Vibration_Play(s32 vibIdx);
```

### func_800892DC → Vibration_PlayWithIntensity
**Line:** 202
**Original:**
```c
void func_800892DC(s32 idx, u8 arg1);
```
**Proposed:**
```c
void Vibration_PlayWithIntensity(s32 vibIdx, u8 intensity);
```

### func_80089314 → Vibration_RandomPulse
**Line:** 207
**Original:**
```c
void func_80089314(s32 arg0);
```
**Proposed:**
```c
/** @brief Generates random controller vibration pulses (faster in combat mode). */
void Vibration_RandomPulse(s32 isCombat);
```

### func_800893D0 → Vibration_ScaledByDistance
**Line:** 238
**Original:**
```c
void func_800893D0(q19_12 arg0);
```
**Proposed:**
```c
/** @brief Plays vibration scaled by distance (100 close, 200 far). */
void Vibration_ScaledByDistance(q19_12 distance);
```

### func_80089840 → Vibration_ClearAll
**Line:** 414
**Original:**
```c
void func_80089840(s_SysWork_2514* arg0);
```
**Proposed:**
```c
/** @brief Removes all active vibration effects. */
void Vibration_ClearAll(s_SysWork_2514* vibWork);
```

---

## 12. Combat System

**File:** `src/bodyprog/bodyprog_combat_8005BF38.c`

### func_8005C814 → Combat_RotateHitboxOffsets
**Line:** 47
**Original:**
```c
void func_8005C814(s_SubCharacter_D8* arg0, s_SubCharacter* chara);
```
**Proposed:**
```c
/** @brief Rotates a character's hitbox offsets by Y rotation into world-space. */
void Combat_RotateHitboxOffsets(s_SubCharacter_D8* hitboxDef, s_SubCharacter* chara);
```

### func_8005C944 → Combat_CharaMoveAndCollide
**Line:** 70
**Original:**
```c
s32 func_8005C944(s_SubCharacter* chara, s_800C4590* arg1);
```
**Proposed:**
```c
/** @brief Moves a character based on heading + speed, applying collision. Core movement function. */
s32 Combat_CharaMoveAndCollide(s_SubCharacter* chara, s_800C4590* collisionResult);
```

### func_8005D50C → Combat_AimTargetSelect
**Line:** 171
**Original:**
```c
bool func_8005D50C(s32* targetNpcIdx, q3_12* outAngle0, q3_12* outAngle1, VECTOR3* unkOffset, u32 npcIdx, q19_12 angleConstraint);
```
**Proposed:**
```c
/** @brief Selects best aiming target among NPCs with angle/distance constraints + LOS raycast.
 *
 * @param outTargetIdx Output: NPC index.
 * @param outPitchAngle Output: pitch to target.
 * @param outYawAngle Output: yaw to target.
 * @param originPos Aim origin position.
 * @param preferredNpcIdx Preferred NPC to lock on.
 * @param angleConstraint Maximum aiming angle.
 * @return true if valid target found.
 */
bool Combat_AimTargetSelect(s32* outTargetIdx, q3_12* outPitchAngle, q3_12* outYawAngle, VECTOR3* originPos, u32 preferredNpcIdx, q19_12 angleConstraint);
```

**File:** `src/bodyprog/bodyprog_combat_8008A058.c`

### func_8008A058 → Math_IntSqrt
**Line:** 30
**Original:**
```c
u32 func_8008A058(s32 arg0);
```
**Proposed:**
```c
/** @brief Integer square root using GTE leading-zero-count and lookup table. */
u32 Math_IntSqrt(s32 value);
```

### func_8008A0E4 → Combat_AttackInit
**Line:** 101
**Original:**
```c
s32 func_8008A0E4(s32 arg0, s32 weaponAttack, s_SubCharacter* chara, VECTOR3* pos, s_SubCharacter* chara2, q3_12 angle0, q3_12 angle1);
```
**Proposed:**
```c
/** @brief Initializes an attack: sets up animation, weapon, angle data, runs hit detection.
 * @return Hit count level (1-6 for player, NO_VALUE for miss).
 */
s32 Combat_AttackInit(s32 attackState, s32 weaponAttack, s_SubCharacter* chara, VECTOR3* attackPos, s_SubCharacter* target, q3_12 yawAngle, q3_12 pitchAngle);
```

### func_8008A3E0 → Combat_HitDetection
**Line:** 293
**Original:**
```c
s32 func_8008A3E0(s_SubCharacter* chara);
```
**Proposed:**
```c
/** @brief Main combat hit-detection. Processes weapon frames, casts rays/sweeps, applies damage.
 * @return Bitmask of hit NPCs.
 */
s32 Combat_HitDetection(s_SubCharacter* chara);
```

### func_8008B3E4 → Combat_RadioSoundSet
**Line:** 1024
**Original:**
```c
void func_8008B3E4(q23_8 vol);
```
**Proposed:**
```c
/** @brief Sets the radio static sound volume. */
void Combat_RadioSoundSet(q23_8 volume);
```

### func_8008B714 → Combat_ApplyDamage
**Line:** 1215
**Original:**
```c
s32 func_8008B714(s_SubCharacter* attacker, s_SubCharacter* target, VECTOR3* arg2, s32 arg3);
```
**Proposed:**
```c
/** @brief Core damage application: damage calc, knockback, hit sounds, blood effects.
 * @return Hit bitmask.
 */
s32 Combat_ApplyDamage(s_SubCharacter* attacker, s_SubCharacter* target, VECTOR3* hitPos, s32 hitType);
```

---

## 13. World Effects

**File:** `src/bodyprog/bodyprog_8005E0DC.c`

### func_8005E650 → WorldEffect_Init
**Line:** 201
**Original:**
```c
void func_8005E650(s32 mapId);
```
**Proposed:**
```c
/** @brief Initializes all world effects for a map. Clears effect table, blood splats, flags. */
void WorldEffect_Init(s32 mapId);
```

### func_8005E7E0 → WorldEffect_AllocSlot
**Line:** 262
**Original:**
```c
s32 func_8005E7E0(s32 arg0);
```
**Proposed:**
```c
/** @brief Allocates next free slot in the world effects table. Round-robin allocation.
 * @return Slot index or NO_VALUE.
 */
s32 WorldEffect_AllocSlot(s32 effectType);
```

### func_8005E89C → WorldEffect_UpdateAndDraw
**Line:** 293
**Original:**
```c
void func_8005E89C(void);
```
**Proposed:**
```c
/** @brief Main world effects update and draw loop (fire, water, snow, blood trails, etc.). */
void WorldEffect_UpdateAndDraw(void);
```

### func_8005F6B0 → Blood_SplatterCreate
**Line:** 690
**Original:**
```c
void func_8005F6B0(s_SubCharacter* chara, VECTOR* pos, s32 arg2, s32 arg3);
```
**Proposed:**
```c
/** @brief Creates blood splatter effects from a hit. Spawns particles and ground decals. */
void Blood_SplatterCreate(s_SubCharacter* chara, VECTOR* hitPos, s32 violenceLevel, s32 creatureType);
```

---

## 14. Visual Effects

**File:** `src/bodyprog/bodyprog_800652F4.c`

### func_800652F4 → Gfx_PlayerAuraDraw
**Line:** 17
**Original:**
```c
void func_800652F4(VECTOR3* arg0, s16 arg1, s16 arg2, s16 arg3);
```
**Proposed:**
```c
/** @brief Renders a cylindrical ring-shaped gouraud-shaded aura effect around a position.
 *
 * 32 segments of POLY_G4 quads in 3 concentric rings. Used for player status effects.
 *
 * @param center World-space center position.
 * @param yawAngle Yaw rotation of the ring.
 * @param expansion Ring expansion/size.
 * @param fadeIntensity Color fade amount.
 */
void Gfx_PlayerAuraDraw(VECTOR3* center, s16 yawAngle, s16 expansion, s16 fadeIntensity);
```

### func_80065B94 → Gfx_PlayerStarburstDraw
**Line:** 133
**Original:**
```c
void func_80065B94(VECTOR3* arg0, s16 arg1);
```
**Proposed:**
```c
/** @brief Renders an 8-pointed starburst/spike visual effect (hit flash, sparkle).
 *
 * @param position World-space center position.
 * @param elapsedTime Frame counter for the effect (0 = initialize random seeds).
 */
void Gfx_PlayerStarburstDraw(VECTOR3* position, s16 elapsedTime);
```

### func_80066184 → Gfx_PlayerBloodPoolDraw
**Line:** 223
**Original:**
```c
void func_80066184(void);
```
**Proposed:**
```c
/** @brief Renders a ground-plane blood pool effect underneath the player.
 *
 * Triggered by R3 button; draws layered semi-transparent quads with dissolve effect.
 */
void Gfx_PlayerBloodPoolDraw(void);
```

---

## 15. BGM / Music

**File:** `src/bodyprog/bodyprog_bgm_80087EA8.c`

### func_80087EA8 → Bgm_PlayIfDifferent
**Line:** 19
**Original:**
```c
void func_80087EA8(s32 cmd);
```
**Proposed:**
```c
/** @brief Plays a BGM track only if it differs from the current target. */
void Bgm_PlayIfDifferent(s32 bgmCmd);
```

### func_80087EDC → Bgm_CrossfadeToTrack
**Line:** 29
**Original:**
```c
void func_80087EDC(s32 cmd);
```
**Proposed:**
```c
/** @brief Multi-step BGM crossfade: wait, check, fade out, wait, set new track. */
void Bgm_CrossfadeToTrack(s32 bgmCmd);
```

### func_80088028 → Bgm_CrossfadeToSilence
**Line:** 73
**Original:**
```c
void func_80088028(void);
```
**Proposed:**
```c
/** @brief Crossfades BGM to silence (track 0). */
void Bgm_CrossfadeToSilence(void);
```

### func_80088048 → Bgm_FadeOutAndStop
**Line:** 78
**Original:**
```c
void func_80088048(void);
```
**Proposed:**
```c
/** @brief Mutes all BGM layers then fades out audio. Waits for completion. */
void Bgm_FadeOutAndStop(void);
```

### func_800880F0 → Bgm_FadeOutWithType
**Line:** 105
**Original:**
```c
void func_800880F0(s32 arg0);
```
**Proposed:**
```c
/** @brief Mutes all BGM layers then fades out using fast or slow fade.
 * @param fadeType 0=slow (SD_Call 22), nonzero=fast (SD_Call 23).
 */
void Bgm_FadeOutWithType(s32 fadeType);
```

---

## 16. Map Screen

**File:** `src/bodyprog/bodyprog_mapscreen_80066D90.c`

### func_80066D90 → MapScreen_DarkenBackground
**Line:** 17
**Original:**
```c
void func_80066D90(void);
```
**Proposed:**
```c
/** @brief Gradually darkens the screen by drawing 63 semi-transparent dark tiles. */
void MapScreen_DarkenBackground(void);
```

### func_80066E40 → MapScreen_FramebufferStore
**Line:** 44
**Original:**
```c
void func_80066E40(void);
```
**Proposed:**
```c
/** @brief Stores current framebuffer (320x240 at 320,256) into FS_BUFFER_3. */
void MapScreen_FramebufferStore(void);
```

### func_80066E7C → MapScreen_FramebufferRestore
**Line:** 51
**Original:**
```c
void func_80066E7C(void);
```
**Proposed:**
```c
/** @brief Restores previously stored framebuffer from FS_BUFFER_3. */
void MapScreen_FramebufferRestore(void);
```

**File:** `src/bodyprog/bodyprog_mapscreen_80066EB0.c`

### func_80067914 → MapScreen_PlayerMarkerDraw
**Line:** 272
**Original:**
```c
s32 func_80067914(s32 paperMapIdx, u16 arg1, u16 arg2, u16 arg3);
```
**Proposed:**
```c
/** @brief Draws the player position triangle marker on the paper map.
 *
 * Contains per-map coordinate mapping tables converting world position to map pixels.
 */
s32 MapScreen_PlayerMarkerDraw(s32 paperMapIdx, u16 scrollX, u16 scrollY, u16 zoomLevel);
```

### func_80068CC0 → MapScreen_PageArrowsDraw
**Line:** 775
**Original:**
```c
bool func_80068CC0(s32 arg0);
```
**Proposed:**
```c
/** @brief Draws up/down arrow indicators showing adjacent map pages are available. */
bool MapScreen_PageArrowsDraw(s32 mapIdx);
```

### func_80068E0C → MapScreen_MarkingsDraw
**Line:** 815
**Original:**
```c
bool func_80068E0C(s32 arg0, s32 idx, s32 arg2, s32 shade, u16 arg4, u16 arg5, u16 arg6);
```
**Proposed:**
```c
/** @brief Draws map markings/annotations (visited rooms, locked doors) on the paper map. */
bool MapScreen_MarkingsDraw(s32 mode, s32 mapIdx, s32 eventIdx, s32 shade, u16 scrollX, u16 scrollY, u16 zoomLevel);
```

---

## 17. Character Spawning

**File:** `src/bodyprog/chara_spawn.c`

### func_80088D34 → Chara_BoneAnimInit
**Line:** 29
**Original:**
```c
void func_80088D34(s32 idx);
```
**Proposed:**
```c
/** @brief Initializes bone animation data for a character type from g_CharaTypeAnimInfo. */
void Chara_BoneAnimInit(s32 charaTypeIdx);
```

### func_80088F94 → Chara_Despawn
**Line:** 122
**Original:**
```c
void func_80088F94(s_SubCharacter* chara, s32 unused1, s32 unused2);
```
**Proposed:**
```c
/** @brief Despawns/frees a character slot. Clears spawn flag, sets charaId to Chara_None. */
void Chara_Despawn(s_SubCharacter* chara, s32 unused1, s32 unused2);
```

### func_80088FF4 → Chara_SpawnFlagsSet
**Line:** 137
**Original:**
```c
void func_80088FF4(e_CharacterId charaId, s32 spawnIdx, s32 spawnFlags);
```
**Proposed:**
```c
/** @brief Sets spawn flags on a character spawn info entry.
 *
 * @param charaId Character ID.
 * @param spawnIdx Spawn point index.
 * @param spawnFlags Flags to set.
 */
void Chara_SpawnFlagsSet(e_CharacterId charaId, s32 spawnIdx, s32 spawnFlags);
```

### func_80089034 → Chara_SpawnPositionSet
**Line:** 145
**Original:**
```c
void func_80089034(e_CharacterId charaId, s32 spawnIdx, q19_12 posX, q19_12 posZ);
```
**Proposed:**
```c
/** @brief Sets the X/Z position of a character spawn point.
 *
 * @param charaId Character ID.
 * @param spawnIdx Spawn point index.
 * @param posX X position (Q19.12).
 * @param posZ Z position (Q19.12).
 */
void Chara_SpawnPositionSet(e_CharacterId charaId, s32 spawnIdx, q19_12 posX, q19_12 posZ);
```

---

## 18. Items / Inventory

**File:** `src/bodyprog/items/item_screens_1.c`

### func_8004C328 → Inventory_HasAmmoForAnyGun
**Line:** 141
**Original:**
```c
bool func_8004C328(bool unused);
```
**Proposed:**
```c
/** @brief Checks if the player has ammunition for any owned firearm.
 * @return true if any gun has ammo available.
 */
bool Inventory_HasAmmoForAnyGun(bool unused);
```

### func_8004C564 → Combat_WeaponEquipSoundUpdate
**Line:** 279
**Original:**
```c
void func_8004C564(u8 arg0, s8 weaponAttack);
```
**Proposed:**
```c
/** @brief Manages weapon equip/unequip sound effects (chainsaw/drill motor, volume ramping). */
void Combat_WeaponEquipSoundUpdate(u8 weaponType, s8 soundState);
```

**File:** `src/bodyprog/items/item_screens_cam.c`

### func_8004BD74 → ItemScreen_ItemModelDraw
**Line:** 137
**Original:**
```c
void func_8004BD74(s32 displayItemIdx, GsDOBJ2* arg1, s32 arg2);
```
**Proposed:**
```c
/** @brief Draws a 3D item model for inspection display with scale correction. */
void ItemScreen_ItemModelDraw(s32 displayItemIdx, GsDOBJ2* obj, s32 drawMode);
```

### func_8004BFE8 → ItemScreen_ProjectionSave
**Line:** 186
**Original:**
```c
void func_8004BFE8(void);
```
**Proposed:**
```c
/** @brief Saves current GTE projection state and sets item-screen projection (FOV=1000). */
void ItemScreen_ProjectionSave(void);
```

### func_8004C040 → ItemScreen_ProjectionRestore
**Line:** 215
**Original:**
```c
void func_8004C040(void);
```
**Proposed:**
```c
/** @brief Restores GTE projection state saved by ItemScreen_ProjectionSave. */
void ItemScreen_ProjectionRestore(void);
```
