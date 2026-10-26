# Responsive menus and baked graphics examples

This cross-platform validation branch contains the later mobile/baked revision,
not the earlier Linux live-worker design in PR #180. Existing PRs are unchanged.

This change replaces the English/Russian PC-options, ordinary-options and title
presentation without replacing the existing option adjustment/config-save APIs.
Auto mode keeps other languages on the legacy paths; an explicit EN/RU menu
override is independent of the disc's story language. This is not a rewrite of inventory,
map, save/load or all other original screens.

## Interface

- Five PC-options categories: Graphics, Lighting & System, Controls, Camera, HUD.
- Responsive label/value column and a separate, wrapped explanation column.
- English and Russian descriptions, including mobile-only options (53 help keys).
- Mobile: six category tabs, larger fixed-size typography, inertial vertical
  scrolling, category swipes, explicit Back controls and a larger example pane.
- Native game FONT16 letterforms: mounted Russian-disc mapping for Cyrillic,
  existing shipped PAL font for Latin. ViTCo uppercase Й is composed from its
  native И and breve; the disc and VRAM font are not modified.
- Green selected text, black background, separate ordinary-options volume
  sliders, smaller original mouse pointer.
- PC-options return transitions stay in the replacement compositor rather than
  flashing the legacy PC list.
- F10 in ordinary gameplay opens the same five categories and pauses the player's
  world. Tab switches to the retained extended quick menu (audio, bindings, mods,
  cheats). Original keyboard/controller bindings and mouse support remain.
- Num+ or Shift+= compares 2D/tank movement during normal gameplay only. Holds do
  not repeat; console, quick menu, demos, cinematics and modifier shortcuts are
  excluded. The existing `control_2d` setting is persisted.

The existing `menu_pillarbox` preference is respected, not rewritten. Turning it
off also lets native-resolution legacy menus and FMVs fill the window; those
legacy screens/video can stretch. The responsive title itself preserves image
aspect. Defaults are not changed by this PR.

## Title assets

`pc_port/assets/gamedata/menu-hd/` is copied by the existing runtime-asset build
step. Optional PNGs are loaded once, not once per frame:

| File | Purpose |
| --- | --- |
| `title-us.png` | Full US-style composition, normally 4:3 display aspect |
| `title-us-wide.png` | Wide composition, used when window aspect is >= 1.55 |
| `title-us-ultrawide.png` | User-supplied wider composition for aspect >= 1.95 |
| `title-pal.png` | PAL logo strip, not a full background |
| `title-jp.png` | Optional Japanese full composition |
| `side-left.png`, `side-right.png` | Dim edge fill outside contained US art |
| `label-00.png` ... `label-17.png` | Original-font EN/RU main/difficulty rows |

The wide image's **actual dimensions** determine its aspect; there is no forced
16:9 squeeze. 4:3/wide sources switch on resize. Missing images use disc VRAM art
with a modest cubic reconstruction; missing labels use native FONT16. The live
fog comes from the original title simulation, uploaded as a low-resolution colour
field and composited additively. It is not baked into the images. This compositor
approximates interpolation of the original fog, not exact PSX triangle rasterization.

Labels 0–8: LOAD, CONTINUE, START, OPTION, ACHIEVEMENTS, EXIT, EASY, NORMAL, HARD.
Labels 9–17: their Russian equivalents. The selected text is tinted green, without
a separate selection marker. Main-menu logic and game launching stay in title.c.
All six upstream title entries fit above the copyright band.

See `asset-provenance.json` for image origins, processing and SHA-256 hashes.
Underlying game artwork/letterforms remain their owners' resources; adding these
files does not grant a new license over them. AI-assisted processing is disclosed,
not represented as original hand-painted HD artwork. The background changes are
isolated in a separate commit so maintainers can drop them independently.

## Baked graphics examples

The normal menu uses `pc_baked_preview.c` on every platform. It does not launch a
second game process or alter the player's world to make an example. The earlier
Linux worker and diagnostic FBO/mesh/TIM modules remain as development references;
the normal menu does not call their live-preview entry points.

`gamedata/previews/manifest.tsv` contains 501 variants for 21 settings, referencing
619 JPEGs. This includes all integer FOV values 40–140 for FPS/TPS/OTS and separate
30/60 Hz precipitation loops. The images were captured offline from the real game
in independent temporary runtimes with an owned disc; other parameters are fixed.
Flashlight-shadow examples include an actual NPC caster because Harry and his
held weapon are excluded from the game's shadow-caster pass.

Values without an exact sample use the nearest variant, explicitly labelled as an
example rather than an exact live rendering of all current settings. Unsupported
effects retain descriptions and an honest unavailable-example message. See
`BAKED-PREVIEWS-AUDIT.md` for the full coverage/missing-effects list and provenance
JSON for file hashes and capture limitations. Weather loops reset after one second;
display refresh/render rate can limit the visible cadence.

PGXP uses a static off/on door comparison with the same camera. Examples are
compressed once with JPEG quality 80 and 4:2:0; title/font PNGs are unchanged.

The player validates manifest paths, entry/frame counts, image dimensions, file size
and a 32 MiB decoded-frame budget. Frames are decoded only when the variant changes;
animation changes the texture on its playback tick without repeated image decoding.
Closing the menu releases the current decoded frames and GL texture. A real SDL/GL
test exercises texture upload, all 303 FOV values, both weather loops, nearest/missing
lookup, unpack-state restoration and close/reopen without needing a game disc.

## Build and review

### Module boundaries

- `pc_menu_font.c` owns CPU FONT16 loading/validation, glyph mapping, measurement
  and rasterization. It has no GL or quick-menu state dependency. Confirmation
  and binding panels consume `pc_menu_font.h` directly, not the quick-menu API.
- `pc_title_screen.c` owns title image/label textures, art layout and live-fog
  composition. Its small painter interface borrows image/rectangle/text drawing;
  it does not access the compositor's atlas, shader, VBO or quick-menu state.
  Asset loading, background, fog and entry composition have separate lifecycles.
- `pc_quick_options.c` retains settings input, shared GL-state save/restore and
  atlas submission. Title composition is one delegated draw operation.
- `pc_baked_preview.c` owns manifest lookup, bounded image decoding, animation
  playback and the current example texture. It has no game-world callbacks.

These boundaries reduce cross-feature dependencies; they do not make a large
rendering/UI change automatically safe or guarantee zero coupling warnings.

This includes the responsive-renderer work related to [PsyCross PR #22](https://github.com/SlickAmogus/PsyCross/pull/22),
merged with upstream renderer revision `d7ae5d4`: full-viewport presentation and
pointer coordinates for responsive overlays, neutral UI post-processing, hidden
worker windows and an optional pre-swap capture callback. The parent repository's
submodule pointer must include that commit. A standalone PsyCross build does not
need a game callback symbol.

```sh
cmake -S pc_port -B pc_port/build -G Ninja \
  -DSH_BUILD_MAP_DLLS=ON -DBUILD_TESTING=ON -DSH_FMV_FFMPEG=OFF
cmake --build pc_port/build --target SilentHillPC pc_options_help_test
ctest --test-dir pc_port/build -R '^pc_options_help_test$' --output-on-failure
```

`pc_options_help_test` checks all EN/RU help entries, scene selection and layouts
from 640x480 through ultrawide/portrait/4K. This unit test does not prove rendering,
input, worker lifecycle or translations on every disc. Runtime verification and
known limits are recorded in the PR rather than claimed by this test.

For disc-free validation of the actual PNG/JPEG player, add `-DSH_UI_GL_TEST=ON`,
build `pc_baked_preview_gl_test`, then run it from its build directory (under
Xvfb when Linux has no display). That target stages its own example fixtures;
building the game first is not required. The test creates a real SDL/GL context,
checks all 303 FOV samples, both weather animations, nearest/missing lookup,
pixel-unpack state preservation and close/reopen, then destroys the cache,
context and window normally. CTest bounds it to 120 seconds. Failed checks
print to stderr and exit nonzero rather than opening a CRT assertion dialog.
`SH_PREVIEW_TRACE=1` optionally logs decoded frames for diagnosis.

Software-driver CI is not vendor-driver certification. In particular, a passing
upload sequence followed by a hang in `SDL_DestroyWindow` is still a failed
lifecycle test, not a pass or a reason to skip teardown.

The aim-transition, OuterProduct12, sewer-water and mall-TV PGXP fixes (#176–#179)
are inherited from current upstream, not reapplied as duplicate patches. The local
Bluetooth cache repair is a machine-specific operation, not part of the game PR.

### Manual regression checklist

Use a separate runtime directory with **copied**, not linked, memory cards/config
and an image you own. Do not use a live playthrough for destructive reset tests.

1. Test both English and Russian: title, difficulty, ordinary options (including
   volume sliders), all five PC categories, reset confirmation and cancel.
2. Try 640x480, 1280x720 and an ultrawide viewport; resize between 4:3 and wide.
   Check paragraph wrapping, label/value separation and title aspect. With all
   six title rows visible, verify mouse hits still match the denser row pitch.
3. Test keyboard, mouse hover/click/wheel and the configured controller. Enter
   PC options and return repeatedly: no legacy list should appear in transition.
4. Select PGXP, lighting, fog, FOV and precipitation examples. Each must match its
   labelled sample value; changing unrelated settings must not pretend to change
   the baked image. Missing effects must show their description/fallback.
5. Close/reopen repeatedly and switch between static images and both weather loops.
   Confirm no child process is started and example memory/texture are released.
6. From normal gameplay, F10 must preserve player position, health, timer and
   event flags. Close and resume movement; reopen and use Tab to reach the old
   extended quick menu. Num+/Shift+= should act once per press, not during
   cinematics, demos, console or open settings.
7. Check a non-EN/RU language still uses its legacy menu/font path, and test
   missing optional title PNGs/native-font fallback. Keep Windows/macOS, GLES,
   actual physical-controller input and other disc families as explicit review
   items unless they have been tested separately.
