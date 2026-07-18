# TASK: Real flashlight shadow mapping (monster shadows cast by the beam)

Prepared 2026-07-03 for a focused session. Goal: monsters (and world geometry)
cast **real dynamic shadows** from the flashlight beam, integrated with the
existing per-pixel flashlight cone. User picked this over cheap blob shadows.

This is a large PsyCross renderer change and WILL need iterative visual tuning
(shadow bias, map resolution, coordinate-space correctness). Budget several
run-and-tune loops with the user.

## Pipeline facts (already investigated — all in pc_port/PsyCross/src/render/PsyX_render.cpp unless noted)
- **Geometry flush:** the whole scene is drawn at frame present by `DrawAllSplits()`
  (gpu/PsyX_GPU.cpp ~1315), which loops "splits" (batched primitive groups) and calls
  `GR_DrawTriangles(split.startVertex, split.numVerts/3)` (PsyX_render.cpp:3015 →
  `glDrawArrays(GL_TRIANGLES, start, tris*3)`) over the shared vertex buffer
  `g_glVertexBuffer[g_curVertexBuffer]` (bound at ~2970). Each split carries
  textureId/blendMode/clip/startVertex/numVerts.
- **Per-vertex view-space position:** `GrVertex.vsx/vsy/vsz` (PsyX_render.h:147) = CAMERA
  view-space pos, captured via the GTE shadow FIFO (same as PGXP). Bound as `a_viewpos`
  (offset `vsx`, PsyX_render.cpp:2987), varying `v_viewpos`. Valid only when vsz>0
  (untracked verts gate off). This is the space the cone shader already works in.
- **Existing FBO pattern to copy:** post-process already creates `g_postFBO`/`g_postTex`
  (single-sample) + `GR_DrawFullscreenTexture` (VAO `g_postVAO`, gl_VertexID triangle).
  Model the shadow depth FBO + depth-only draw on these (GR state save/restore idiom at
  the end of GR_DrawFullscreenTexture: reset g_PreviousShader=-1, g_lastBoundTexture=-1,
  blend=BM_NONE, depth=0, scissor=0, re-enable GL_STENCIL_TEST).
- **Light data (world space):** `g_WorldEnvWork.field_60` = light POS (Q12), `field_58` =
  light DIR (turns with Harry). The flashlight push (bodyprog_80055028.c ~181) transforms
  these through `GsWSMATRIX` (world→camera-view) into `g_PsyX_FlashlightPos`/`Dir` (view
  space) each frame, gated on `isFlashlightOn_15 && !cutscene`. Cone params:
  `g_PsyX_FlashlightRange`(4000)/`InnerCos`/`OuterCos`(~0.82). Uniform `u_flashlightOn` =
  `g_PsyX_UsePerPixelFlashlight && g_PsyX_FlashlightActive` (PsyX_render.cpp ~1727).

## Approach: light-POV depth pre-pass + compare in the cone shader
1. **Shadow FBO.** Create a depth-only FBO `g_shadowFBO` + depth texture `g_shadowTex`
   (GL_DEPTH_COMPONENT24, e.g. 1024×1024, GL_LINEAR + GL_COMPARE_REF_TO_TEXTURE for
   hardware PCF, or NEAREST + manual compare). Clamp-to-border, border depth 1.0.
2. **Light matrix.** Each frame (when the flashlight is active) build on the CPU:
   `u_lightFromView = lightProj * lightView * inverse(cameraView)`
   - `cameraView` = `GsWSMATRIX` (world→view) captured at the flashlight-push point; invert it
     (rotation transpose + translated origin) to get view→world.
   - `lightView` = look-at from world light pos (`field_60`) along world light dir (`field_58`).
   - `lightProj` = perspective with FOV ≈ acos(OuterCos)*2 (the cone angle), near/far bracketing
     `g_PsyX_FlashlightRange`. Pass `u_lightFromView` (mat4) to BOTH the depth shader and the cone shader.
3. **Depth pre-pass.** Before `DrawAllSplits` draws color (or as a first traversal): bind
   `g_shadowFBO`, glClear depth, set a depth-only program whose vertex shader is
   `gl_Position = u_lightFromView * vec4(a_viewpos, 1.0)` (no fragment output). Replay the
   OPAQUE splits' vertex ranges (`glDrawArrays(GL_TRIANGLES, split.startVertex, split.numVerts)`)
   from the SAME `g_glVertexBuffer`. Skip additive/translucent + 2D/UI splits (they shouldn't
   cast). Front-face cull or a depth bias to fight shadow acne. Restore FBO/state after.
   - EASIEST integration: give `DrawAllSplits` a `shadowPass` bool (or a sibling
     `DrawAllSplitsShadow()`), called once with the shadow FBO bound before the normal color loop.
4. **Sample in the cone shader.** In GPU_FRAGMENT_SAMPLE_SHADER's `u_flashlightOn` branch, after
   computing the cone term: `vec4 lp = u_lightFromView * vec4(v_viewpos,1.0); vec3 luv =
   lp.xyz/lp.w*0.5+0.5;` then `float lit = texture(u_shadowTex, luv.xy).r >= luv.z - bias ? 1 : 0`
   (or `textureProj` with a sampler2DShadow for PCF). Multiply the cone contribution by `lit`.
   Add a slope-scaled `bias` uniform to tune. Keep OFF path byte-identical (only inside
   `u_flashlightOn>0`).

## Gotchas / tuning knobs
- **Coordinate space:** confirm `GsWSMATRIX` at the flashlight-push frame is the CAMERA world→view
  (not a stale per-object matrix). If shadows swim/detach, the inverse-view or the view-space
  assumption is wrong — verify by projecting a known point.
- **Shadow acne / peter-panning:** front-face culling in the depth pass + a small constant+slope
  bias. Start bias ~0.0015, iterate.
- **What casts:** only opaque world + characters. Exclude sky/fog/2D/additive splits (blendMode /
  drawenv.dfe). The vsz>0 gate already excludes untracked (2D) verts.
- **Perf:** one extra geometry pass at shadow-map res. Fine on PC. 1024² is a good start.
- **MSAA:** the shadow FBO is its own single-sample depth target — unaffected by the main FB's MSAA.
- **Gate:** only run the pre-pass when `g_PsyX_UsePerPixelFlashlight && g_PsyX_FlashlightActive`
  (i.e. flashlight on, not in a cutscene). Feature default follows per_pixel_flashlight, or add a
  separate `flashlight_shadows` config toggle.

## Files to touch
- PsyCross: PsyX_render.cpp (FBO create/destroy, depth shader, shadow uniforms, sample in cone
  shader, light-matrix build), PsyX_GPU.cpp (DrawAllSplits shadow pass), PsyX_render.h (GrVertex
  already fine; add g_shadow* externs), maybe PsyX_public.h for a config toggle.
- Parent: bodyprog_80055028.c (already pushes light pos/dir — reuse; optionally publish the raw
  world light pos/dir + camera view for the CPU light-matrix build). pc_config.c + config.cfg +
  launcher if a `flashlight_shadows` toggle is added.

## Open question for the user
Confirm: shadows only while the per-pixel flashlight is ON (simplest, ties to that feature), or a
separate on/off toggle? And is 1024² shadow resolution acceptable to start (bump later if edges
look chunky)?
