# Integrated cross-platform menus

This branch replaces the older #180 / #185 implementations. Android upstream
`d5cc03b47` has been merged and the menu/config/input/renderer conflicts resolved.
It retains the upstream native achievements and controls screens. It also includes
upstream pc-port commits `6eeeaa1f8` and `e54c5c222` (replacement-model weapon
placement and launcher QoL metadata), already present in the locally tested game.
PsyCross is pinned to a merge with upstream `d7ae5d4`; no older input or lighting
implementation is intentionally restored. Physical-device coverage remains limited.

## Shared implementation and resources

- One `pc_title_screen.c`, `pc_menu_font.c`, `pc_quick_options.c`, config parser,
  PNG/JPEG decoder and example manifest are compiled on all five platforms.
- `ui_language = auto|en|ru` controls the new menu, option values, descriptions
  and dialogs independently of the mounted disc's story language. The title
  button supports mouse, touchscreen, L, and controller right. It persists only
  `ui_language`; it does not rewrite the disc language or message tables.
- Desktop menus have five clickable tabs; touch layouts retain six categories,
  larger targets, inertial scrolling and explicit Back buttons. These are
  deliberate platform layouts, not divergent copies of the source.
- 501 parameter variants reference 619 JPEG images, including 303 integer FOV
  samples and two 60-frame precipitation loops. The PGXP example is now a
  genuinely rendered static off/on door pair, not a scarcely visible animation.
- Example JPEGs use quality 80, 4:2:0, encoded once from the captured PNGs.
  They are lossy comparisons, not exact captures of the current game scene.
  Native menu glyphs and title artwork remain PNG. Original asset rights apply;
  provenance records the capture and conversion methods, not a new license.
- The normal menu uses bounded decoded-frame caching and no secondary game
  process. Historical renderer prototypes remain development references only.

The three additional lighting settings include eight real-render captures: a
separate glTF test character, the supported fog-enabled world-lighting path,
and four stock-Harry fill-light levels. The test GLB is not installed or bundled;
CesiumMan attribution and adaptations are recorded in `lighting-credits.txt`.
The world example explicitly enables the engine fog path in both captures and
does not claim to demonstrate every room or draw path.

Only 21 settings have verified examples. Unsupported visual effects show their
description rather than an unrelated image; see `BAKED-PREVIEWS-AUDIT.md`.
The showcase holds other parameters fixed and uses a nearest-value caption when
appropriate. Precipitation loops reset at their one-second boundary.

## Validation

The workflow builds Linux, Windows, macOS, Android (ARM64 and ARMv7), and an
unsigned iOS device IPA from the **same revision**. A standard-library validator
checks every packaged example's digest, dimensions, frame count and memory bound.
The desktop real-GL test decodes actual shipped JPEGs with the same enabled image
formats, exercises all FOV values, both loops, the static PGXP pair, GL unpack
state preservation and complete cache/context/window cleanup.

Windows CI uses Mesa softpipe explicitly. The hosted runner's llvmpipe completed
the preview checks but hung in SDL window destruction, so neither a hidden skip
nor a claimed vendor-GPU test is used. iOS compilation/bundle checks do not prove
touch behaviour on a real iPhone. Existing iOS installs may retain older staged
assets; updating user-customized assets needs a separate migration policy.

Android CI APKs have ephemeral debug signing keys and are **not** updates for an
existing locally signed installation. Do not uninstall an existing app merely
to install a CI artifact: that can discard app data. Release signing belongs to
the maintainer; no keys or game discs are included here.

The 2D aim, OuterProduct12, sewer-water timing and mall-TV fixes are now inherited
from current upstream; their earlier PRs were merged, not reapplied as duplicates.
