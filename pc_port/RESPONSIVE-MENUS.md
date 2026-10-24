# Responsive menus and isolated graphics previews

This change replaces the English/Russian PC-options, ordinary-options and title
presentation without replacing the existing option adjustment/config-save APIs.
Other languages retain the legacy menu paths. This is not a rewrite of inventory,
map, save/load or all other original screens.

## Interface

- Five PC-options categories: Graphics, Lighting & System, Controls, Camera, HUD.
- Responsive label/value column and a separate, wrapped explanation column.
- English and Russian descriptions for all 40 current PC settings.
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

## Linux graphics showcase

The preview is real game rendering in a **disposable second process**, not a
simulation of the player's save or a filter applied to a static screenshot:

- School (`map1_s00`): geometry/filter/lighting/camera examples, Harry with a pipe
  and flashlight.
- Street (`map2_s00`): fog; the same cached worker animates actual precipitation
  for the weather setting.
- Nonvisual or unsupported settings have no misleading example. Resolution,
  MSAA, HUD, input and some camera actions are not demonstrated.

Rendering is 640x360. At most two workers are retained while the menu is open.
Static scenes settle over four frames and sleep until settings change. Weather
animates at up to 60 rendered frames/s. The school scene preloads asynchronously
on PC-options/F10 entry, not at game launch or in ordinary options. First load is
still nonzero and hardware-dependent; reopening after close loads again.

Each worker gets a private 0700 directory, a 0600 shared-memory file, the existing
map-module directory and a read-only-use link to the mounted image. It does not
share player memory cards, config, plugins or account files. It starts a fresh
showcase game, disables achievements/Discord/plugins/randomizer, uses dummy audio,
hides its window and excludes physical controllers. Inherited nonstandard file
descriptors are closed at spawn. This is **process separation, not an OS sandbox**:
map libraries still execute native code and have the user's normal permissions.

The renderer freezes showcase events/AI after warmup and advances only weather
when needed. GPU readback happens outside the shared lock. Heartbeat publication
is independent of that lock; a worker exits after 60 seconds without its owner.
Closing the menu signals owned workers without blocking on GPU shutdown; child
reaping is nonblocking. Normal exit removes only exact private files, never linked
assets. Abrupt process termination may leave private temporary files behind.

Set `SH_MENU_PREVIEW_DISABLE=1` to disable workers. The settings UI stays usable
and reports preview unavailable. Windows/macOS builds currently have no worker
backend and show that fallback. The UI/asset loading is otherwise portable C/GL;
those platforms still need build/runtime review.

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
- `pc_menu_preview.c` owns the separate showcase processes and IPC; it does not
  own the title assets or font buffers.

These boundaries reduce cross-feature dependencies; they do not make a large
rendering/UI change automatically safe or guarantee zero coupling warnings.

This depends on [PsyCross PR #22](https://github.com/SlickAmogus/PsyCross/pull/22): full-viewport presentation and
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

Keep the independent aim-transition, OuterProduct12, sewer-water and mall-TV PGXP
fixes in their existing PRs (#176–#179). They are not bundled here. The local
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
4. On an initial nonvisual row, wait for hidden school preload. It must not take
   keyboard focus. Select lighting/PGXP, then fog, weather, and school again.
   Static scenes should stop rendering once settled; weather should animate.
5. Toggle PGXP repeatedly without destroying/restarting the worker. Close/reopen
   immediately and after both scenes have loaded; verify no visible child window
   and no surviving worker after closing. Test `SH_MENU_PREVIEW_DISABLE=1`.
6. From normal gameplay, F10 must preserve player position, health, timer and
   event flags. Close and resume movement; reopen and use Tab to reach the old
   extended quick menu. Num+/Shift+= should act once per press, not during
   cinematics, demos, console or open settings.
7. Check a non-EN/RU language still uses its legacy menu/font path, and test
   missing optional title PNGs/native-font fallback. Keep Windows/macOS, GLES,
   actual physical-controller input and other disc families as explicit review
   items unless they have been tested separately.
