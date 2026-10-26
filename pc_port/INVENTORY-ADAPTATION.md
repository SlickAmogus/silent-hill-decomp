# Inventory adaptation (depends on menu PR #187)

This preserves the original inventory, not a replacement item system. It uses
the native FONT16 painter and full-width overlay integration from #187. Until
that dependency is merged, this branch includes its history; the inventory-only
delta starts after `0550e98d01222f8accaee72cc7b24c567c510f9c`.

## Changes

- Recompose the existing health/equipment/command/carousel regions at their
  authored display proportions. Original 3D item models, health scan/glitch,
  item quantities, descriptions and command logic remain in use.
- Draw centered native EN/RU headings, navigation, quantities and complete item
  descriptions. Description text appears immediately in the adapted inventory;
  story dialogue is not changed. Unsupported/SJIS menus retain the legacy path.
- Draw one selector after recomposition, avoiding split/trailing legacy frames.
- Mouse wheel rotation and horizontal finger swipes advance the original
  one-slot model loader, preserving its seven resident carousel models. Touch
  taps on side slots/arrows use that same queue rather than a mouse-only target.
  Command submenus remain modal. Original pad/stick navigation is retained.
- Cache a full-Y-turn model bound at the authored tilt, rather than fitting the
  instantaneous silhouette. Center weapon pivots and use one stable uniform
  scale; item rotation therefore does not repeatedly resize the model. This
  bound currently measures original TMD vertices, not replacement glTF meshes.
- Optional `gamedata/menu-hd/inventory-portrait.png`: user-provided portrait,
  aspect-preserving atlas adaptation. Original artwork rights remain with their
  owners; the provenance file records processing, not a new license. Existing
  user high-resolution portrait overrides take precedence.

## OpenGL / GLES

The compositor copies the current GPU framebuffer into reusable panel textures;
there is no per-frame CPU readback. It restores read framebuffer/read buffer,
texture binding, unpack PBO and pixel-store state. A multisampled window is
resolved at identical dimensions before copying. GLES requires matching source
and destination color formats: RGB windows use RGB8, not RGBA8. The incorrect
RGBA assumption was reproduced as black panels on a real Android phone and fixed.

Textures are released when leaving the adapted screen. Models and menu actions
remain owned by the original game. The helper is inactive for item-examination
screens and unsupported menu scripts. This is not a general GL-context-loss
recovery implementation.

## Validation and limitations

- Pure layout tests cover six aspect ratios, inverse hit testing, navigation
  targets and command rows. The desktop workflow builds these alongside the
  existing ten PC tests; Windows uses real Mesa softpipe, not a vendor GPU.
- Linux full-game checks used independent copies of memory cards: wheel bursts
  and reversal, input during animation, gesture-state emulation, modal commands,
  close/reopen, stable full-turn weapon bounds and unchanged inventory contents,
  health/equipment. Ultrawide/MSAA and native-resolution captures were checked.
- Android ARM64 and ARMv7 compile. A physical Android 16/Adreno phone showed the
  actual portrait/models after the RGB fix; the user confirmed swipe operation.
  Side-arrow taps were corrected subsequently. No physical controller or iPhone
  verification is claimed. CI compilation alone is not touchscreen validation.
- Test grants, user configurations, memory cards, disc images and debug bindings
  are not included. The portrait is separate from the code commit for review.

Implementation and diagnostics were prepared with AI assistance and manual user
feedback. Please review the model-fit cost and renderer integration before merging.
