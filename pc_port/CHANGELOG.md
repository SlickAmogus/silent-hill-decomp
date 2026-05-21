# Silent Hill PC Port — Changelog

## v2026.05.21.1 -- 2026-05-21
- Revise README with project details and instructions
- pc-port: fix melee sprint-cancel arm-swing + handgun fire-completion race
- Fix wording in project description
- fix gun fire/reload regressions; correct trigger_zones.md Y-axis

## v2026.05.19.3 -- 2026-05-19
- pc-port: fix leg animation when aiming + walking
- pc-port: fix weapon-ready state during movement + sprint-overrides-aim

## v2026.05.19.2 -- 2026-05-19
- tools: filter changelog meta-commits from nightly release notes
- pc-port: combat fixes + launcher update dialog

## v2026.05.19.1 -- 2026-05-19
- pc-port: fix map gray bars, radio static after kill, menu bilinear

## v2026.05.18.2 -- 2026-05-18
- tools: update CHANGELOG.md before upload so released copy has current notes
- launcher: show "You're up to date!" dialog when no updates found (launcher does not patch itself, will be uploaded separately at some point)
- pc-port: clarify PsyX_EndScene forward decl comment
- tools: fix release script broken by embedded quotes in commit messages

## v2026.05.18.1 -- 2026-05-18
- pc-port: fix content warning screen 176px black bar on left

## v2026.05.17.5 -- 2026-05-17
- changelog: restore initial release snapshot section
- changelog: update v2026.05.17.2 entry with additional fixes

## v2026.05.17.4 -- 2026-05-17
- tools/changelog: simplify format to date + commits, newest first
- tools: simplify GitHub release notes to commits only

## v2026.05.17.3 — 2026-05-17
- launcher: fix update apply failing silently + reset button after update
- pc-port: add cheat keys 7/8/9/0 + menu SFX feedback

## v2026.05.17.2 — 2026-05-17
- tools: remove --prerelease from nightly releases
- tools/launcher: fix UTF-8 BOM causing manifest deserialization failure
- tools: exclude config.cfg from nightly manifest
- pc-port: fix inventory TMD rendering, handgun ammo pickups
- pc-port: positional audio fixes, fixed radio sounds for monsters
- pc-port: added a lot of post-cafe camera fixes

## v2026.05.17.1 — 2026-05-17
Initial nightly release.

---

## v2026.05.16.1 — Initial release snapshot

First public nightly. Snapshot of the PC port's state as of the launcher's
auto-update rollout.

### Boot & menus
- Konami / KCET logos, intro FMV, main menu, options screen, save/load
  screens all navigable.
- Loading screen plays correctly between map transitions.
- Pre-Konami "graphic content" warning screen wired up.
- 15-second startup delay (audio task pool drain) eliminated.

### World rendering
- Full 3D world + textured environments with fog.
- Per-vertex shader fog replacing the original PSX overlay system; fixes
  the seam line on top of the screen.
- 16:9 hor+ widescreen with per-shot pixel-aspect culling correction.

### Player
- Harry's full body renders with all 23 bones and gouraud shading.
- Movement: walk + run via collision-based path. Wall collision and
  floor height fully working in most areas.
- TPS (third-person) follow-cam toggle on numpad `2`. WIP
- Aim + fire system: handgun and knife work; muzzle flash and blood
  splat re-enabled safely.

### Combat & enemies
- Air Screamer (bird enemy): AI, animation, swoop attack, hit-take,
  death-and-fade all working. Cafe-window break cutscene plays.
- Groaner (dog): full AI from disc-extracted rodata.
- Bloodsucker, Romper, SplitHead, Creeper, HangedScratcher, LarvalStalker,
  PuppetNurse: AI re-enabled via per-enemy `*_anim_infos.c` + per-DLL
  dispatch tables extracted from disc.
- Cybil, Alessa (and her ghost-child variant), BloodyLisa, Lisa,
  Kaufmann, Dahlia: NPC AI enabled (anim infos extracted).
- Enemy spawn density restored to vanilla PSX: fixed a 16-byte
  `s_SpawnInfo` struct mismatch on x86-64 that was making every distance
  check use a bogus Z coordinate, and reduced the per-slot spawn
  cooldown from 10s to 1s.

### Audio
- SFX via PsyCross SPU → OpenAL.
- Ambient SFX VAB loads properly.
- BGM loads correctly.
- XA voice streaming from the original disc image.
- 3D audio: distance-based volume falloff restored (was previously
  full-volume regardless of distance — Air Screamer wing flap could be
  heard across the entire map).

### Cameras
- WYSIWYG `s_camCorrections[]` system with road-region matching and
  per-anchor XZ-radius override.
- Hand-tuned corrections across map0_s00 intro (3 starting-area shots,
  Cheryl-chase alley, alley2 first/second/third/fourth fixed cams,
  alley3 shots through final post-spawn), map0_s01 cafe entry, and
  map2_s00 post-cafe dog-head area.
- Rotation deltas (yaw/pitch) now stored as rotation rather than baked
  into translation — survives baseline drift on tracking cams.
- Camera correction system skipped during cutscenes (when
  `VC_USER_CAM_F` is set) so DMS-driven cinematic cams aren't perturbed.

### Cutscenes & FMVs
- DMS-driven in-game cutscenes work (opening, cafe, etc.).
- FMV playback via ported DuckStation MDEC + custom STR demuxer +
  MPEG-1 VLC decoder. XA audio mixed in.
- Enter-key input bleed from FMV skip into the next state fixed.

### Map system
- 42/42 maps compile successfully.
- PSX-address sanitizer scrubs raw `0x80XXXXXX` function pointers from
  DLL map headers.

### Debug / launcher
- Top-row `4`/`5` keys log BAD/GOOD camera positions with full delta
  capture (post-rotation translation + raw yaw/pitch nudges).
- Numpad `.` logs Harry's detailed position + camera state for tracking
  fall-through-floor spots.
- Numpad `3` rescue-teleport + collision probe.
- Launcher with display config, debug logging toggle, hi-res loose
  texture toggle, map override, fullscreen / vsync / culling / preload
  / intro / PGXP settings, dropdown for UI scaling.

### Known issues at v2026.05.16.1
- Some areas show garbage / chunky-pixel textures on walls.
- Map item screens have rendering issues (item TMD invisible during pickups).
- Falling-through-floor in certain spots (under active diagnosis).
- Handgun bullets pickup model invisible (under active diagnosis).
