# Same-context preview prototype

**Superseded:** the menu now uses `pc_baked_preview`, not this importer or its
framebuffer. The experimental modules remain for reference but are not called
by the options UI. See `BAKED-PREVIEWS-AUDIT.md` for actual example coverage.

This is an Android-tested rendering/import experiment, **not a finished options
preview**. It does not replace the Linux worker backend yet.

## Implemented

- `pc_preview_target`: owned 480x270 RGBA/depth framebuffer, with explicit
  begin/end restoration of draw/read framebuffer, viewport and scissor state.
- `pc_preview_mesh`: bounded raw v6 LM decoder for a self-contained static model;
  copies triangle positions and PSX material/UV/CLUT data into owned memory.
- `pc_preview_tim`: bounded 4/8-bit TIM palette decoder into owned RGBA pixels.
- `pc_preview_scene`: reads model 0 from school chunk SC0000 and its material
  sheets from the mounted disc, creates a private texture atlas and vertex buffer,
  renders once, then displays the cached texture in the mobile menu.
- Native unit tests cover malformed extents, unsupported shared vertex windows,
  triangle/quad strip order, ownership, UV metadata, palette selection and alpha.
  Synthetic mesh mutations are also exercised under ASan/UBSan on the host.

No map is loaded, no story callback runs, and no shared PSX VRAM is uploaded by
this importer. The diagnostic uses a fixed orthographic overview and simple face
lighting; it is visibly labelled as a test, including its missing effects.

## Not implemented or verified

- Full school composition, IPD instance transforms, global PLMs, Harry's skeleton,
  pipe attachment, flashlight, game-equivalent fog/postprocessing/PGXP.
- Applying any option to the diagnostic image. In particular the displayed
  fragment is **not evidence** that a changed graphics setting works in preview.
- Context-loss recovery, app suspension/resource-pressure tests, asynchronous
  scene loading and graceful resource-failure UI beyond the existing placeholder.
- Windows, macOS or iOS execution. The modules use OpenGL/ES-compatible concepts,
  but compiling on Android is not a cross-platform validation.

The current read/decode/upload runs once on the render thread when opening the
mobile PC options. It is small enough for this experiment, not the architecture
to use for an entire map. GPU resources are destroyed when that menu closes.
All GL calls must remain on the owning context's render thread.

Next steps are a prepared interior camera/composition, isolated skeletal geometry,
then effect implementations shared with the actual game rather than approximate
look-alikes. Lifecycle and game-state invariance tests are required before this
can be treated as a replacement for the Linux worker.
