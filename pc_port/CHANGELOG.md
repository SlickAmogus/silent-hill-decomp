# Silent Hill PC Port — Changelog

## v2026.06.13.28 -- 2026-06-13
- Flashlight color: tint the real light-color matrix; console: flip tap/hold
- Split light-color console commands: fl = flashlight cast, wl = world ambient
- Tint chest lens-flare by the fl flashlight color
- Guard boss-projectile pool against wild entries during the ending (CutsceneGlitch.log)

## v2026.06.13.27 -- 2026-06-13
- Air Screamer bite reach 3u->4u + flashlight color console command
- Guard credits text drawers against NULL str (results.log crash)
- Fix credits.c link error: include sh_log.h for SH_DBG

## v2026.06.13.26 -- 2026-06-13
- Add adsr console command + bump PsyCross (envelope on audio thread, default OFF)
- Fix ending/credits crash: s32* iterator over char*[] read half-pointers (64-bit)
- Add [AS] attack-timing diagnostic for Air Screamer hit delay

## v2026.06.13.25 -- 2026-06-13
- Revert PsyCross ADSR: caused hard freezes + save-load hangs (deadlock)

## v2026.06.13.24 -- 2026-06-13
- Fix final-boss crash ROOT: D_800CAE30 zero-stub gave projectiles NULL ptr_0

## v2026.06.13.23 -- 2026-06-13
- (no commits since last release)

## v2026.06.13.22 -- 2026-06-13
- Bump PsyCross: SPU ADSR envelope for looping voices (clock bell ring-out)
- Fix final-boss crash (incubus/incubator variants): seed ptr_0 in twin pool init

## v2026.06.13.21 -- 2026-06-13
- Fix PuppetNurse/Doctor NULL field_124 crash in map7_s01 (astro.log)
- Restore chest-flashlight lens flare strength (was dimmed by facing knee)

## v2026.06.13.20 -- 2026-06-13
- Header-driven auto-extraction: end the zero-stub whack-a-mole

## v2026.06.13.19 -- 2026-06-13
- Fix carousel horses stacked at center: extract horse offset/angle tables
- Add proactive latent-stub finder (header-driven, pre-empts the bug class)

## v2026.06.13.18 -- 2026-06-13
- (no commits since last release)

## v2026.06.13.17 -- 2026-06-13
- Fix lit-character backface culling (face through head in cutscenes)
- Fix final-boss attack crash: seed projectile-pool ptr_0 (NULL-deref in func_800D88E8)

## v2026.06.13.16 -- 2026-06-13
- Reduce log hitching: remove hot-path audio debug logs

## v2026.06.13.15 -- 2026-06-13
- Fix final-boss crash (both variants): port projectile motion-script tables

## v2026.06.13.14 -- 2026-06-13
- Proactively extract 2 more zero-stub ROM tables + improve audit write-detection
- Fix MonsterCybil AI freeze (blocks Good/Bad endings): keyframe constants zero-stub

## v2026.06.13.13 -- 2026-06-13
- Fix final-boss rifle div0 crash + repeating grunt SFX: D_800EC770 zero-stub

## v2026.06.13.12 -- 2026-06-13
- Add zero-stub classification sweep (latent read-before-write bug finder)
- Fix final-boss cutscene crash (post-aglaophotis): D_800F2448 stub too small
- Extract map7_s02 keypad puzzle solution D_800E9E1C (was zero-stub)
- audit_zero_stubs: detect compound assignments (+=,++) -> fewer false positives

## v2026.06.13.11 -- 2026-06-13
- PsyCross: PGXP coverage diagnostics
- Fix map7_s01 astrology puzzle + JP-warning-screen (two zero-stubs)

## v2026.06.13.10 -- 2026-06-13
- PsyCross: bump to PGXP Z-fight fix v3 (per-vertex continuous depth)
- PsyCross: PGXP v4 ΓÇö texture-only shader + un-quantised flat depth
- PsyCross: revert PGXP to texture-only known-good
- PGXP phase 1: store-macro capture + world-emit hooks (game side)
- Fix Alessa-scene div-by-zero crash after Cybil boss (map6_s04)

## v2026.06.13.9 -- 2026-06-13
- Bump PsyCross: revert PGXP per-vertex depth (restore texture-only)

## v2026.06.13.8 -- 2026-06-13
- Bump PsyCross: PGXP continuous-depth Z-fighting fix
- Bump PsyCross: PGXP depth-warp fix (preserve b.w)

## v2026.06.13.7 -- 2026-06-13
- (no commits since last release)

## v2026.06.13.6 -- 2026-06-13
- PGXP: console `pgxp 0/1` + F1 hot-toggle; bump PsyCross
- Bump PsyCross: PGXP hint-based vertex lookup fix
- launcher: PGXP tooltip reflects working state + F1 toggle; banner click shows About box
- Bump PsyCross: PGXP coverage probe

## v2026.06.13.5 -- 2026-06-13
- Fix D_800CC424 zero-stub: Harry's map6_s04 Cybil-boss anim overrides

## v2026.06.13.4 -- 2026-06-13
- Fix Cybil boss-fight crash: variableFunc pointer was raw PSX address

## v2026.06.13.3 -- 2026-06-13
- Extract more INCLUDE_RODATA zero-stubs: lighthouse-effect VRAM + boss positions
- Extract remaining INCLUDE_RODATA zero-stubs: SFX positions + rotations + data

## v2026.06.13.2 -- 2026-06-13
- Fix Cybil boss progression lock: DMS node-name strings were zero-stubs

## v2026.06.13.1 -- 2026-06-13
- Fix otherworld garbage textures: gate far world objects on texture residency
- Add [TEXVRAM] probe for otherworld lighthouse rainbow (map6_s02 chunk textures)

## v2026.06.12.14 -- 2026-06-12
- Lost-poke census: fix D_800A9938 alias (Cybil boss anim buffer size)

## v2026.06.12.13 -- 2026-06-12
- Fix cutscene letterbox black corner squares in borderless widescreen
- Fix Cybil carousel boss never spawning: set NoEnemySpawn on map6_s04 entry

## v2026.06.12.12 -- 2026-06-12
- Fix mall TV cult-symbol animation: extract full D_800DB874 pattern table

## v2026.06.12.11 -- 2026-06-12
- Fix instant otherworld transition: extract D_800F0084 threshold table (map6_s00)

## v2026.06.12.10 -- 2026-06-12
- Fix motel dresser snap-back + BGM layer diagnostics for bar scene
- Fix lighthouse-stair crash: collision offset-alpha div-by-zero

## v2026.06.12.9 -- 2026-06-12
- ROOT FIX sewer/save-load crash family + Romper attack crash

## v2026.06.12.8 -- 2026-06-12
- walls: extend [WALL-HIT] with vertical-span data (speed-dependence)
- walls: un-gate [WALL-HIT] from the visualizer
- sewer crash guard + 4:3 flash hysteresis (user report batch 1/2)
- Fix mall TV-bank static/sigil screens: extract zero-stubbed effect tables

## v2026.06.12.7 -- 2026-06-12
- floatstinger: [MOTH] wing/anim state probe
- fix Floatstinger idle wing flap: lost duration poke through alias
- walls: [WALL-HIT] face-naming probe at cylinder contact

## v2026.06.12.6 -- 2026-06-12
- fix Floatstinger boss: dead AI dispatch table + zero-stubbed rodata
- retire exe-side D_800D7A04 stub (DLL now defines the real table)

## v2026.06.12.5 -- 2026-06-12
- blood: fix effect-descriptor leak + make [BLOOD-CFG] actually fire
- walls: [COLL-MISS] diagnostic in Ipd_CollisionDataGet NULL path

## v2026.06.12.4 -- 2026-06-12
- remove visibility force-set bypasses + sanitize blood color on load
- fix invisible walls: IPD header clobber zeroed collision surfaces

## v2026.06.12.3 -- 2026-06-12
- speed probe: add pos/zone-cap/dtR to [SPEED] log line
- FMV: controller skip (Cross/Start) + [MESHCULL] backface diagnostic
- strip stale debug logging (60-96% of log volume)
- blue-blood triage: log extraBloodColor once per map load

## v2026.06.12.2 -- 2026-06-12
- Fix Cybil basement voice desync: cmd table truncated to half its real size

## v2026.06.12.1 -- 2026-06-12
- Pointer-truncation audit: fix live sites found via full warning harvest
- Cybil-scene voice desync: consumption trace + table-overrun guard
- fixup: include sh_log.h for the [VOICE] trace (link error)

## v2026.06.11.21 -- 2026-06-11
- Fix larva-boss intro crash: VECTOR* truncated through s32 param
- Fix larva crash follow-up: widen func_800D185C in header + twinfeeler.c copy

## v2026.06.11.20 -- 2026-06-11
- Fix black rooms from pinned texture pages: nearer chunks steal from farthest

## v2026.06.11.19 -- 2026-06-11
- Fix room void after teleport doors: same-frame eviction of fresh chunks

## v2026.06.11.18 -- 2026-06-11
- Log git build hash at startup + widen void diagnostic to player-cell misses
- build-info: drop dirty marker (CMake git autocrlf false positives)

## v2026.06.11.17 -- 2026-06-11
- Fix character hand/held-item visibility: merge mis-mapped variant macro

## v2026.06.11.16 -- 2026-06-11
- Fix map4_s01 pickup crash (data stubbed as functions) + item-pickup softlock

## v2026.06.11.15 -- 2026-06-11
- Fix school black void: texture-page pool starved by interior window

## v2026.06.11.14 -- 2026-06-11
- Pause shows the true frozen frame + Harry receives fog in gameplay
- docs: index character-fog negative-index fix
- Fix all-gray/all-black interiors: stale shared-buffer pointers in chunk slots

## v2026.06.11.13 -- 2026-06-11
- launcher/config: canonical map names from upstream README + config regeneration

## v2026.06.11.12 -- 2026-06-11
- Fix shrunk map pickups (merge-lost PC blocks) + interior chunk streaming rework
- Fix overlapping/cut-off cutscene voices + Levin St house indoor snow

## v2026.06.11.11 -- 2026-06-11
- Fix Split Head boss crash: PSX stack-frame aliasing + boss div-zero audit
- docs: index Split Head stack-aliasing fix + boss audit additions

## v2026.06.11.10 -- 2026-06-11
- Systematic div-by-zero sweep: guard all x86 idiv/rem fault sites
- docs: index drain-valve, school-key/cam-warp, and div-by-zero sweep fixes

## v2026.06.11.9 -- 2026-06-11
- Fix fog-color flash during puzzle key insertion
- Fix two user-reported div-by-zero crashes (school key + camera warp)

## v2026.06.11.8 -- 2026-06-11
- [SPEED] probe: gate on logging instead of debug controls
- Guard SdUtKeyOnV against garbage VAB images (unused map6_s05 crash)
- Fix Cybil boss not spawning: remove NoEnemySpawn force-clear band-aid
- Guard Lm_MaterialRefCountDec against unloaded LM headers (map6_s05)
- Fix vanishing world objects (doghouse papers/GOLD_HID) + spawn/groaner probes
- Fix radio static stuck after door transitions + anim-rate probe
- Throttle [WOBJ] find-fail to once per name per session

## v2026.06.11.7 -- 2026-06-11
- Fix missing cutscene voices game-wide: 32 zero-stubbed voice tables
- Adjusted positioning of launcher dropdown.
- Fix cursor-click puzzles: extract keypad rects/codes (4 maps)

## v2026.06.11.6 -- 2026-06-11
- Console: help + debug command references; block debug keys while typing
- Game-over screen: black background, not fog color
- XA voice deep-dive: harden stuck-state paths + disc audit tool
- Fix item TMD previews vanishing in foggy/dark maps
- Borderless display mode + launcher Fullscreen/Windowed/Borderless dropdown
- Add [SPEED] probe: 1s wall-clock ground speed log (debug-gated)

## v2026.06.11.5 -- 2026-06-11
- release-nightly: ship runtime DLLs (MinGW/SDL2/OpenAL/libjpeg)

## v2026.06.11.4 -- 2026-06-11
- Update to test launcher self-update functionality.

## v2026.06.11.3 -- 2026-06-11
- SH1Updater: create gamedata/ on first run + disc image prompt
- Disc image presence check in updater + launcher
- Launcher: strip inline update flow ΓÇö updater is the only update path
- Retire SH1Updater ΓÇö launcher self-updates via the rename swap

## v2026.06.11.2 -- 2026-06-11
- SH1Updater.exe: standalone game+launcher updater

## v2026.06.11.1 -- 2026-06-11
- Interactive console: hold ~ for Half-Life-style command input
- Console commands: help, map, give, noclip, fmv
- Log + flush FS queue WaitForEmpty timeout (was a silent escape)
- Fix console Enter leaking into the game as Start
- Fix drain-valve cutscene div-by-zero in map1_s03 drip draw
- Console input: suppress controls after pad parse; remove menu half-boot
- Quick Save/Load hotkeys (F6/F8) + console noclip fix + launcher tweaks
- Fix console Enter leaking to main menu + FMV instant-skip from console
- Console fmv: hide XA voice banks, list only real movies
- Console fmv: fade transition, numeric indices, intro/end aliases

## v2026.06.10.10 -- 2026-06-10
- Fix plates-door crash: raw PSX pointer as FS read destination
- Crash telemetry, eclipse-door black background, world-object resolve trace
- Fix item-door corruption: g_ItemTriggerEvents was a ONE-element array

## v2026.06.10.9 -- 2026-06-10
- Fix rumble launch crash: PC-sized effect node pool
- Fix second rumble launch crash: field_2510 pointer truncation

## v2026.06.10.8 -- 2026-06-10
- NPC whitelist retired, flare knee, DualShock rumble, launcher dedup

## v2026.06.10.7 -- 2026-06-10
- Stub port round 2: per-map types, world-object class resolved, +keyframe data

## v2026.06.10.6 -- 2026-06-10
- Diagnostics for school BGM, invisible cat, muzzle-flash blob
- Fix muzzle-flash blob, invisible cat + missing enemies, flare intensity, locker cadence

## v2026.06.10.5 -- 2026-06-10
- (no commits since last release)

## v2026.06.10.4 -- 2026-06-10
- Fix NPC anim-completion poll: NULL animInfo crash + exact-kf freeze
- Binary-extract the 12 remaining enemy/NPC anim tables + re-extract cat
- Fix stuck-aim on empty clip: auto-reload entry never initialised
- logging: [NURSE] state trace for the frozen-nurse diagnosis
- Fix frozen nurses: binary-extract the 8 stubbed puppet-nurse data tables
- Stub sweep: extract all ROM-constant zero-stubs from map binaries
- Fix g_MainImg0 zero stub: real s_FsImageDesc from main.c
- Gate auto-extraction on 64-bit-safe types; fixes map3_s03 nurse crash
- Pad auto-extracted arrays to exe stub capacity; fixes hospital-entry crash
- Remove [NURSE] diagnostic trace; nurse behavior verified in-game

## v2026.06.10.3 -- 2026-06-10
- Fix flashlight lighting seams + restore chest lens flare
- Flare occlusion on PC: facing test instead of framebuffer readback

## v2026.06.10.2 -- 2026-06-10
- Fix silent layered BGM map-wide: extract real layer-limit/room-flag tables
- Fix invisible school cat: unify duplicated chara anim data array
- Route PsyCross logging via PsyX_Log_SetStream before init
- docs: index the BGM layer-table extraction + duplicated chara-anim array fixes
- logging: BGM room-index-on-change + per-layer volume-on-change
- Bump PsyCross: shutdown terminate diagnostics + log-tail flush
- logging: XA play/stop/reject trace for the ambience audit
- Fix melee phantom swing on release + add ammo/auto-reload diagnostics
- Add movement_original config: opt-in PSX lower-body movement machine
- docs: index melee phantom-swing + anim-stuck detector fixes
- logging: [MOVE-ORIG] lower-body state trace for movement_original diagnosis
- Fix walk/sidestep moving in place under movement_original (double dt-scaling)
- Fix walk/sidestep speed + wall smack: unfuse moveSpeed from runDistance
- Make movement_original the default; remove the [MOVE-ORIG] trace
- docs: index the moveSpeed/runDistance unfusion + movement_original default

## v2026.06.10.1 -- 2026-06-10
- debug: repurpose keys 4/5 to cycle the map config (prev/next)

## v2026.06.09.6 -- 2026-06-09
- debug: Esc warm-reboots to title (PC dev key)
- diag: load cat at modelIdx 0 (tpage 28) to test tpage-29 invisibility

## v2026.06.09.5 -- 2026-06-09
- Fix school black-void: force isLoaded=false on IPD reformat-fail
- Bump PsyCross submodule: repair dead POLY_FT4 clut guard
- Fix interior chunk-buffer overrun thrash (school void/exploded geometry)
- Fix cat locker cutscene freeze: real CAT_ANIM_INFOS table (was zero-stub)
- Fix cat locker scene-end crash: NULL-guard Anim_BoneInit (WinDbg-confirmed)
- docs: add Port_Fixes_Index ΓÇö curated game-code PC-port fixes
- logging: remove ~345 stale troubleshooting traces (keep infra)
- logging: trim [SH] boot/chunk spam + gate per-frame state logs
- logging: strip dead scaffolding left by the trace removal
- docs: add combat/animation/cutscene band-aids to Port_Fixes_Index
- Fix chemical-on-hand cutscene crash: guard div-by-zero in smoke particle
- docs: ┬º1 now covers div-by-zero (hand cutscene crash) alongside NULL derefs

## v2026.06.09.4 -- 2026-06-09
- log: remove stale per-frame [MCRD2] spam + the [ALLEY1] Cheryl diagnostic
- debug: key 6 spawns a Grey Child; add [CHMOVE] Cheryl movement trace
- math: restore overflow-safe Math_Vector2/3MagCalc on PC (merge regression)
- math/cheryl: target the overflow fix to the chase gates, not the global macro
- debug: grey-child spawn ΓÇö bypass per-area NPC cap + guard model load
- cheryl: remove [CHMOVE] diagnostic trace (Cheryl run-through fix confirmed)
- diag: log failing object name + item-LM magic in [WOBJ] find-fail (map1_s00 banding)
- cat: guard NULL playbackFunc ΓÇö fixes school crash (merge regression)

## v2026.06.09.3 -- 2026-06-09
- pc_port: bump PsyCross ΓÇö pillarbox bars stay black on item-examine screen

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
