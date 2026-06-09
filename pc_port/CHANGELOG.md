# Silent Hill PC Port — Changelog

## v2026.06.09.2 -- 2026-06-09
- (no commits since last release)

## v2026.06.09.1 -- 2026-06-09
- Merge upstream Vatuu/master (Jun 2026) + merge resolution (squashed)
- pc-port: fix merge regressions ΓÇö grey-child crash, melee, map, transition flash
- pc-port: fix exterior/preload map regressions (intro environment)
- pc-port: revert merge player-state corruption in cutscene walk (player.c)
- Fix cutscene turn-in-place: restore dropped/renamed HAS_PlayerState defines
- Fix cutscene run-in-place: sharedData_800D32A0_0_s02 was u8 (truncated moveSpeed)
- Pause world while "I don't have a map" / "too dark" message is shown
- Fix Hor+ transition VRAM-atlas flash: clamp motion-blur tiles to framebuffer
- Add menu_pillarbox config option (default on)
- Bump PsyCross: menu pillarbox applies every frame (was lost after frame 1)
- Checkpoint: working menu pillarboxing + launcher controls-button stub
- Untrack launcher build artifacts (obj/ bin/), add to .gitignore
- Configurable keyboard/controller bindings + debug-control gate
- launcher: Controls window (keyboard + controller binding editor)
- launcher: refresh-rate slot -> Pillarboxing Yes/No; tooltips; preload default
- controls window: key-capture + layout fix; pillarbox/culling defaults+tips
- Fix gray fog-color flash when opening inventory/menus
- launcher controls: Turn Left/Right labels, bindable Shift, Reset button
- launcher controls: drop L3/R3 (stick click) rows
- diag: [ALLEY1] trace Cheryl run vs camera in the alley1 chase
- Expand [ALLEY1] diagnostic: log Cheryl controlState/anim/speeds
- Air Screamer: use real per-keyframe hitbox radius on PC, not hardcoded 1.5

## v2026.06.07.2 -- 2026-06-07
- Re-enable flashlight lens flare on PC (revert stub to clean decomp)
- Collision visualizer: show collState panel as raw fixed-point

## v2026.06.07.1 -- 2026-06-07
- Fix walk-through-walls: off-by-one in PC collision grid bounds check
- Collision visualizer stage 2: world-space wireframe overlay
- Collision visualizer: red hit-marking on contacted faces
- Collision visualizer: full-cell capture (stable, all geometry)
- Collision visualizer: cylinder colliders + near-plane clip
- Collision visualizer: collState inspector panel (func_8006A4A8)
- Collision visualizer: cache cell geometry + throttle floor probes
- Fix school progression crash: unreliable IPD fixup-skip check
- Fix school crash properly: isLoaded byte trusted before reformat ran
- Fix school crash part 2: skip fixup on stale/invalid IPD buffer

## v2026.06.06.3 -- 2026-06-06
- Add ' collision visualizer overlay for decomp debugging

## v2026.06.06.2 -- 2026-06-06
- (no commits since last release)

## v2026.06.06.1 -- 2026-06-06
- Fix alley3 lighter-hold: re-enable held-light arm pose on PC
- Lighter-hold: flame tracks the raised hand (invalidate arm-bone flg)
- Fix cutscene letterbox bars not rendering on PC
- Keep cinematic letterbox FOV locked during the zoom hold

## v2026.06.03.3 -- 2026-06-03
- Fix fogged-floor grid seams + Harry fog-flicker (per-vertex v0 fog)

## v2026.06.03.2 -- 2026-06-03
- Enhance [LIGHTERPOSE] trace with keyframe-settle detection
- Add [FMVEND] diagnostic for early FMV cutoff (Cheryl M2_01190)
- Fix Harry dropping the lighter-hold pose on gameplay resume (alley3)
- Revert lighter-hold idle guard (382a96139) ΓÇö no-op for the actual bug
- Capture demux-error detail at [FMVEND] (Cheryl M2 secCount mismatch)
- Fix FMV early cutoff: skip interleaved null/padding sectors in demux
- Clean up FMV cutoff debugging after null-sector fix
- Restore original PSX opening-BGM trigger; strip BGM debug scaffolding

## v2026.06.03.1 -- 2026-06-03
- Strip FIRE_DBG / FIRE_COMMIT investigation traces (combat fixes confirmed)
- Fix character models rendering black in flashlight/lighter darkness
- Add [LIGHTCMP] trace at Harry draw to compare lighting inputs vs PSX
- Add [EFXCALL] trace to Gfx_MapEffectsUpdate for alley3 mode debug
- Enrich [LMODE] trace with primType-transition state
- Fix Harry pitch-black in flashlight/lighter darkness (real root)
- Remove lighting-debug diagnostics after darkness fix confirmed
- Add [LIGHTERPOSE] trace for alley3 lighter-hold anim investigation

## v2026.06.02.2 -- 2026-06-02
- Make FIRE_DBG change-triggered; strip obsolete grey-child AI log spam
- Remove TPS branches from the combat aim/fire input path
- Revert "Remove TPS branches from the combat aim/fire input path"
- Add upperBodyState/lowerBodyState/weaponAttack to FIRE_DBG gun-gate trace
- Fix handgun fire-lockup: allow fire across the aim-HOLD window (FPS-proof)
- Change - / = cheat keys to give rifle / shotgun + ammo
- Add FIRE_COMMIT trace at the gun fire-commit point
- Fix auto-aim target-switch fire lockup (FPS-proof retarget transitions)

## v2026.06.02.1 -- 2026-06-02
- pc: `~` toggles in-game console; raise game window on launch
- pc: GUI-subsystem app (no console window) + console slide animation
- Fix death/grab map-anim freeze in non-map0 maps
- Fix grey-child melee, grab break-free, and 64-bit combat pointer bugs
- Fix Larval Stalker melee: real collision data (same zero-stub bug as grey children)
- Fix Creeper + Hanged Scratcher melee: real collision data (zero-stub bug)

## v2026.06.02.1 -- 2026-06-02
- pc: `~` toggles in-game console; raise game window on launch
- pc: GUI-subsystem app (no console window) + console slide animation
- Fix death/grab map-anim freeze in non-map0 maps

## v2026.06.01.4 -- 2026-06-01
- Bump PsyCross: fix inventory HUD gradient-bar flicker (zero G3/G4 fog pads)

## v2026.06.01.3 -- 2026-06-01
- camera: remove obsolete s_camCorrections band-aid system + debug traces
- camera: re-enable fixed-angle XZ limit clamp (was disabled on PC)
- camera: revert map0_s01 fix_ang band-aids + drop disabled override table

## v2026.06.01.2 -- 2026-06-01
- camera: fix in-place TransposeMatrix corrupting SETTLE-mode cameras
- docs: document in-place TransposeMatrix camera fix

## v2026.06.01.1 -- 2026-06-01
- camera: add facing-direction gate to road cam corrections
- camera: ease scene corrections in instead of snapping
- camera: let a correction span a whole rail-cam shot
- camera: match span-shot corrections by fixed box, not cur_near_road
- camera: fix 3D projection vertical center (112->120) to match PSX
- Revert camera projection vertical-center change (unvalidated)
- camera: trace watch-target Y pipeline ([CAMPITCH]) to pin the aim-too-low bug
- camera: restore PSX road cam-height clamp (root cause of mis-framing)
- camera: trace final render angle (cam_mat_ang) to isolate pose->matrix bug
- camera: fix Math_RotMatrixZxyNeg pitch inversion (root cause, verified vs PSX)
- camera: default to original (corrections off); document road-cam fix

## v2026.05.31.1 -- 2026-05-31
- combat: fix continuous handgun fire on locked targets
- debug: route key-press events to console overlay; document controls
- launcher: give the game window focus after Play
- combat: stop locked handgun fire from latching on after button release

## v2026.05.30.4 -- 2026-05-30
- warning_screen: fix OT order + add fade-out
- boot: warning screen final timing; fix snow leaking indoors in map0_s02

## v2026.05.30.3 -- 2026-05-30
- debug: top-row -/= give Chainsaw / Rock Drill (+ Gasoline if missing)

## v2026.05.30.2 -- 2026-05-30
- combat: fix melee attack in TPS mode; bypass PSX shift-register for mouse
- combat: knife now behaves like the real PSX game
- combat: smooth handgun continuous fire + fix knife double-swing
- combat: continuous knife hold; clean reload; R/M/I PC hotkeys
- combat: selective melee release-latch; drop I/M open hotkeys

## v2026.05.30.1 -- 2026-05-30
- gfx: wire OT bucket count into PsyCross depth tracking
- gfx: bump PsyCross ΓÇö fix OT depth direction
- gfx: bump PsyCross ΓÇö fix a_zw attrib binding in non-PGXP path
- PsyCross: advance submodule to 99417e8
- PsyCross: bucket-accurate OT depth assignment
- pc_port: per-vertex GTE SZ depth + clear table in GsDrawOt
- pc_port: bump PsyCross to b22793b (global SZ depth scale)
- gfx: quantise mesh depth to 64-unit SZ buckets; map0_s02 camera fixes
- combat: fix weapon-fire and melee-attack gate stuck on PC

## v2026.05.29.2 -- 2026-05-29
- gfx: revert backface cull disable in Gfx_MeshDraw

## v2026.05.29.1 -- 2026-05-29
- gfx: bypass preloadChunks for interior maps; disable backface cull on PC

## v2026.05.27.1 -- 2026-05-27
- pc-port: replace dead debug console with dbg_overlay marker system
- dbg_overlay: fix marker logging; strip per-frame log spam
- logging: strip per-frame SH_DBG spam; fix dbg_overlay key detection
- logging: remove remaining [2D_FX] spam; add one-shot overlay diagnostics
- dbg_overlay: fix rendering ΓÇö correct UV orientation, LSB font bit order, GL init timing
- Add ingame debug overlay with 4-mode show_console config
- dbg_overlay: increase LINE_LEN/MAX_CONSOLE, fix line render order
- sh_log: route SH_LOG/SH_WARN to ingame overlay; fix MapRegistry fprintf
- Remove stale diagnostic logging (GameBoot steps, DMS, RADIO_SPU)
- main_pc: fix stale show_console comment for mode 2
- map2_s00: fix event cap, dead-end crosses, gas station, floor fall-through

## v2026.05.22.2 -- 2026-05-22
- pc-port: fix inventory screen flicker
- pc-port: fix jump-back delta-time movement, sidestep smoothing, gun-attack anim ownership
- pc-port: fix screen fade DR_MODE routing; whitelist LINE_F2/G2 in OT sanitizer
- pc-port: fix main menu background left-edge white line

## v2026.05.22.1 -- 2026-05-22
- (no commits since last release)

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
