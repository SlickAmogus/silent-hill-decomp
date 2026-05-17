# Silent Hill PC Port — Changelog

Auto-updated by `tools/release-nightly.ps1` on each release. The script
prepends new sections directly above the `<!-- next-release-here -->`
marker, so the newest release is always at the top.

The base PSX decompilation comes from
[Vatuu/silent-hill-decomp](https://github.com/Vatuu/silent-hill-decomp).
Everything below describes only the PC port work on top.

<!-- next-release-here -->

## [v2026.05.16.1] — Initial nightly release

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
- PGXP (subpixel precision + perspective-correct interpolation) is wired
  through the GTE + prim-emit pipeline; runtime-gated via `use_pgxp`
  config flag (default off).
- High-resolution loose TIM override system: drop textures into
  `gamedata/load/` to replace originals.

### Player
- Harry's full body renders with all 23 bones and gouraud shading.
- Movement: walk + run via collision-based path. Wall collision and
  floor height fully working in most areas. Camera-relative WASD.
- TPS (third-person) follow-cam toggle on numpad `2`. Spherical orbit
  with far-look-at to eliminate Q12→Q8 jitter.
- Aim + fire system: handgun and knife work; muzzle flash and blood
  splat re-enabled safely.

### Combat & enemies
- Air Screamer (bird boss): AI, animation, swoop attack, hit-take,
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
- Radio static near enemies works (silent up until this release because
  a MIPS-compiler reciprocal-multiply for the NPC array index was baked
  for PSX `sizeof(s_SubCharacter)`).

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
- 31/42 maps build as runtime-loaded DLLs; 11 skipped (non-constant
  initializers / cross-map shared data).
- PSX-address sanitizer scrubs raw `0x80XXXXXX` function pointers from
  DLL map headers.

### Debug / launcher
- Top-row `4`/`5` keys log BAD/GOOD camera positions with full delta
  capture (post-rotation translation + raw yaw/pitch nudges).
- Numpad `.` logs Harry's detailed position + camera state for tracking
  fall-through-floor spots (`HARRY POSITION LOGGED`).
- Numpad `3` rescue-teleport logs the landing spot (`HARRY FALL POSITION`)
  + probes collision (slope angles + void-cell flag).
- Launcher with display config, debug logging toggle, hi-res loose
  texture toggle, map override, fullscreen / vsync / culling / preload
  / intro / PGXP settings, dropdown for UI scaling.

### Known issues at v2026.05.16.1
- Some areas show garbage / chunky-pixel textures on walls.
- Map item screens have rendering issues (item TMD invisible during
  some pickups).
- White center cursor occasionally visible.
- Falling-through-floor in certain spots (under active diagnosis).
- Handgun bullets pickup model invisible (under active diagnosis).
- "Continue" save crash (under active diagnosis).
- Sluggish intro FMV playback.
