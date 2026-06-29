# Silent Hill PC Port — Changelog

## beta-2026.06.29.1 -- 2026-06-29
- Fixed awkward TPS/OTS aiming, should be much better!
- Rainbow corruption should be fixed                                   
- Fixed pipe melee swing ending too soon
- New tonemapping and per pixel lighting options, also added to launcher   
 

Commit summaries:                                                                                         
- Aim hitbox: full-body coverage + blood at the shot spot
- Graphics: tone mapping (F3) + per-pixel flashlight toggle (F4) + launcher
- Melee: play the full swing (fix pipe stopping at waist)
- Fix rotated Nowhere elevator door (g_WorldObject0 stub overrun)
- Fix interior "rainbow" texture corruption (stale stolen VRAM page)
- Per-pixel flashlight: view-space shadow propagation + per-frame light push
- map7_s03: hide boss fire/lightning FX before the fight
- Per-pixel flashlight: gate on flashlight flag, replace PSX glow
- Flashlight cone: gate on Harry's flashlight flag, not field_2
- Flashlight cone: suppress per-vertex directional light when cone is active

## beta-2026.06.28.2 -- 2026-06-28
- OTS/TPS free-aim: Fix for free aim so that it's not as hard to hit enemies and controllers have auto aim again (both may need a little more tweaking)
- Also removed hop backwards animation from OTS/TPS for smoother controls
Coming soon: There are bunch of half-fixed updates that will be fully fixed and pushed soon, including more optional graphical enhancements, cutscene fixes, etc.

## beta-2026.06.28.1 -- 2026-06-28
- Graphics options (MSAA + post-process) wired to config + launcher
- Removed outdated launcher options, added help/feedback options
- Fix Chainsaw / Rock Drill stuck-in-AimStart at uncapped FPS

## beta-2026.06.27.1 -- 2026-06-27
- Fixed the gasoline tank inventory model throwing stretched "spike" triangles and crashing the game when viewed in the inventory

## beta-2026.06.26.3 -- 2026-06-26
- Small fix for bullets only hitting right in front of the player in OTS/TPS modes

## beta-2026.06.26.2 -- 2026-06-26
- Reworked TPS/OTS upper body animation and aiming system and it is much better than before. Not perfect, but will be tuned in future updates.
- Backspace now toggles free aiming crosshair while ingame

Commit summaries:
- TPS/OTS aim: per-weapon ready keyframe + Backspace crosshair toggle
- Custom clean upper-body fire/reload FSM for free-aim guns
- Fix free-aim FSM: aim-release stuck + reload reliability

## beta-2026.06.26.1 -- 2026-06-26
- Fixed major issue with ranged weapons causing bosses like split head and some regular enemies to take way too many shots to kill
- Fixed stretch item pickups
- Added Linux and MacOS build support and they should be in nightly builds now, haven't tested yet since this is the first one
- More air screamer fixes
- More ending cutscene fixes, still need to work on fire texture issues and lightning/fire under and around map
- Lighting on water partially fixed
- Fixed TPS/OTS camera being active during cutscenes
- Big updates to OTS/TPS modes, added proper animations for strafing, increased speed, made walking/running controller sensitive, changed aiming animation to a more fitting one, near instant shooting. It's still buggy, still working on updating it.
- Added keyframe viewer debug tool. In debug mode, press K to activate it. It will freeze Harry (you can still move) and you can cycle through his keyframes with , and . and change animation types with / - switching to other loaded NPCs and cycling their animations is planned
- Other misc fixes, below will have more details, but some of the things mentioned are not actually fixed. However it will at least give a good idea of what is being worked on.
Coming soon: Fixing remaining graphical/audio issues, further tweak alt. camera modes, clean up launcher and add more GFX options, fix extra weapons and see thru inventory

Commit summaries:
- Revert dt-carry global timing change (edfe66887) — disturbed other cutscenes
- Stub sweep: extract 5 confirmed read-before-write ROM tables (audit HIGH/MED-HIGH)
- Linux build support (integrate SlickAmogus SH PR#22 + PsyCross PR#3), Windows-safe
- Cutscene timing probes: [XATIME] (xa_player) + [MSGSYNC] (map_msg_display)
- Point PsyCross submodule at master tip (2e36ecd) after merging Linux support
- macOS (arm64) build support (integrate SlickAmogus SH PR#20 + PsyCross PR#1)
- ci: add Linux + macOS build workflows + release-nightly -AttachCrossPlatform
- linux: -Wl,-Bsymbolic on map .so — fix cafe (map0_s01) reload loop
- ci: add missing libjpeg (FMV MJPG decode) to Linux + macOS builds
- build.sh: fix macOS gcc detection aborting under set -e
- cmake: make <SDL2/SDL.h> resolve with Homebrew SDL2 (macOS)
- map7_s03: fix Good+ ending falling-fire ghost textures (64-bit ptr truncation)
- map7_s03: fix Good+ ending bottle-smash ~10s freeze (high-fps one-shot miss)
- map7_s03: fix boss flame/lightning rendering under/around the arena (zero-stubs)
- map7_s03: bottle breaks on impact (was hovering intact through the scream)
- release-nightly: include Linux+macOS builds by default (was opt-in)
- map7_s03: stop boss flame/lightning rendering after the Incubus fight (stale gate)
- OTS/TPS free-aim: move+aim+shoot, instant aim/fire, camera-ray reticle (first cut)
- OTS/TPS free-aim tuning: walk/sprint model, run-then-aim, crosshair center
- OTS/TPS free-aim: tilt Harry's torso + arms toward the aim pitch
- OTS/TPS free-aim: pitch from camera look, not hand->point (fix arms-over-head)
- PC-disable audit: re-enable 4 band-aid'd effects + strip dead probes
- OTS/TPS free-aim: park instant-aim at keyframe 588 (gun-forward), not field_4
- Add in-game keyframe inspector (K / , / .) for finding Harry poses
- Add anim-info panel to the keyframe inspector (amber box, K)
- Keyframe inspector: kf console command + accelerating , . hold
- Fix stretched pickup/take-item: skip H-correction in Hor+ mode
- map7_s03: clear force-field mesh gate (D_800F4830) on Incubus exit + ending
- Revert D_800F4830 gate change (58226d6be) — not the cause
- OTS/TPS: side-run strafe anims (HarryAnim_RunLeft/RunRight) when sprinting
- Keyframe inspector: `/` jumps to next anim start + show anim range
- Combat: shotgun deals full pellet damage (fix Split Head tankiness)
- Keyframe inspector: `/` cycles the equipped weapon's upper-body anims
- OTS/TPS: full-body strafe + turn-run adapts to directional run anim
- Keyframe inspector: reach weapon anims past base keyframeCount (567->658)
- OTS/TPS aim hold at kf591 + full-body movement (gun-equipped) + dir anims
- OTS/TPS aim: stop arm-overwrite (hands-behind-head); faster run + match strafe
- OTS/TPS: force the default cinematic camera during cutscenes
- OTS/TPS free-aim: bullet pitch from hand->target so shots hit the reticle
- map7_s03: [BOSSFX2] diagnostic for pre-spawn fire/lightning
- map7_s03 diag: `add` console command to isolate the additive fire/lightning layer
- OTS/TPS: bullets hit screen center + crosshair centered
- OTS/TPS: fix Harry stuck in the aim pose when not aiming

## beta-2026.06.25.5 -- 2026-06-25
- Air screamer fixes
- Harry extra voice lines in Lisa cutscene fixed.
- Fixed over 70 zero-stubs that could've all been causing misc bugs, not sure what has all been fixed yet.

Commit summaries:
- Fix map7_s00 Lisa-cutscene playing elevator voices early (audioCmds overrun)
- Fix map1_s02 BGM (shadowed stub) + 5 cutscene audioCmds overruns (audit findings)
- Unshadow 77 map-DLL data tables: remove exe zero-stubs that shadowed real data
- Air screamer: extract the 2 missing scale VECTORs (were zero stubs -> model collapsed)

## beta-2026.06.25.4 -- 2026-06-25
- Reverted fix that caused issued with other cutscenes, still working on the late game Alessa cutscene at the theme park.
- Fixed knife double swing not hitting both times.
- Added contributing.md to the project to clarify project ownership and ways to contribute.
- Fixed stubs for maps which could've caused miscellaneous bugs, mainly in the school.

Commit summaries:
- Fix cutscene audio/visual desync: carry per-frame delta-time truncation at high fps
- Fix knife double-swing: first slash dealt 0 damage (blade scaler used partial window)
- Add CONTRIBUTING.md (ownership, official channels, contribution policy)
- Revert dt-carry global timing change (edfe66887) — disturbed other cutscenes
- Stub sweep: extract 5 confirmed read-before-write ROM tables (audit HIGH/MED-HIGH)

## beta-2026.06.25.3 -- 2026-06-25
- Fixed issue of gameplay frames being scene in the sky after opening map. (Reverted to brief black sky when opening map which will be fixed soon.(
- Massive cutscene fixes that should hopefully correct a lot of issues (still testing, see details below)

Commit summaries:
- Fix map6_s04 Alessa/Dahlia cutscene crash (D_800ED848 undersized stub)
- map6_s04 cutscene: ADSR scope for portal SFX loop + crash guard/tracer
- Fix fps-dependent subtitle drift in cutscenes (typewriter speed)
- Fix cutscene visuals running slow vs real-time audio (g_DeltaTime cap)
- Fix g_CommonWorldObjects 256-byte stub overrun (64-bit struct growth)
- Fix map1_s02 silent monologue + map3_s02 degenerate cutscene clip (zero-stubs)
- Fix 7 more undersized u8[256] stub overruns (64-bit struct growth)
- Revert map-open frame-hold (ghost regression); keep brief black flash

## beta-2026.06.25.2 -- 2026-06-25
- Fix permanent pillarbox after examining 2D screens

## beta-2026.06.25.1 -- 2026-06-25
- Puzzle/examine screens no longer stretched by widescreen ortho — render 4:3.
- Map-open black flash gone — holds the frozen foggy frame across the load gap like pause does.
- Menus always use classic controls — TPS/OTS binds no longer leak into menu navigation.
- Blade weapons deal proper, FPS-independent damage.

Commit summaries:
- Fix widescreen 2D-screen stretch + map-open black flash
- Force classic control scheme in menus
- Add [BLADESWEEP] diagnostic for blade-weapon melee damage
- Add Doorway Randomizer mode design/effort doc (scoping only)
- Fix blade-weapon melee damage at high fps (fps-independent peak scaler)

## beta-2026.06.24.3 -- 2026-06-24
- Update pushed so that latest update does not downgrade users to past beta version. If you see this, you are on the correct version.

## v2026.06.24.2 -- 2026-06-24
- Launcher is fixed to detect releases from new branches automatically now. 
- Enemies audited and a lot of issues are fixed. Enemies falling through floor should be fixed.
- Final boss improvements
- Air screamer improvements
Relatively small update for now. Still working on a lot of bug fixes and improvements.

Commit summaries:
- Launcher: detect newest build across branches by parsed version; betas as real releases
- Launcher: on a version tie, prefer the beta release (leading-edge stream)
- Fix repo build scripts + README to match the real MSYS2/Ninja build
- air_screamer: deal damage on all 3 cone attacks; shrink PC shove radius
- incubus/unknown23: extract real boss ROM tables; fix data_stubs type mismatches
- collision: hold NPCs at current height when no IPD chunk is loaded (PC)
- air_screamer: restore real PSX cone attack + hull hitbox; remove combat band-aids
- player: allow aim-then-walk when aiming at nothing (open space)


## v2026.06.24.1 -- 2026-06-24
- Update to make launcher support automatic migration to beta branch

## beta-2026.06.24.1 -- 2026-06-24
- Controls: Revamped control system so that the default camera and control style have their own, separated control scheme from the alternate control styles. Now in the launcher, under controls, you can check a checkbox to switch the control scheme you are customizing the controls for. By default, they control similarly to modern action games.
- Pulled in more fixes from sergiomanzur to syncronize cutscene voices, fix cutscene stretching, improve fog, make intro screens skippable, misc cleanup, and combat improvements (apply enemy melee damage once per swing, before you could take more damage at high fps)
- Fog level slightly increased to match real SH, but can be tuned with fogstr console command
- Fixed console ghosting and black sky when game paused in most cases
- Fixed air screamer first appearance so it actually flaps its wings
- Fixed Hyper Blaster so it can be used from cheats
- Miscellaneous qol fixes detailed below
Coming soon: Fixing the black bar on top of certain fixed camera shots, issues with puzzle overlays in some cases, leftover random crashes, issue with katana damage + continuing to work on other known issues. Please report any crashes to me and it is certainly possible I forget things so feel free to remind me (especially if it's game breaking). Want to try fully implementing PAL + other language support soon as that has been requested a lot.

Commit summaries: 
- Port in PR#17 self-contained fixes: combat / msg-timer / voice-sync / cleanup
- Widescreen: flag the OT2 (2D-UI) draw for full vertical ortho; drop msg-shift band-aid
- Fog: add `fogstr` console command (world fog density), default neutral
- QOL: optional skip the boot logos (Disclaimer / Konami / KCET)
- hfov: add `hfov` console command (3D-world horizontal scale), default neutral
- Air Screamer flyby: actually flap the wings (advance the anim, don't just set it)
- Console: don't zero dt while a map-message is displayed (e.g. "I don't have a map")
- Air Screamer flyby wings: flap in the AS update, not the event (AI was clobbering it)
- 2D-background clear: black bars for map-pickup; keep fog sky on the death screen
- 2D-bg clear: GAME OVER stays black; "no map"/"too dark" messages keep the foggy sky
- Fix console ghosting on pause + warning fading back in
- Revert warning g_PcMenuPillarbox=0 — warning should be pillarboxed, not fullscreen
- Warning screen: stop the SECOND flash — skip the boot-state warning re-draw on PC
- Blood: fade additive layers with world fog so distant blood isn't vivid
- Map DLLs: apply the exe's -Wno-* warning suppressions (GCC 14 build fix, PR#19)
- docs: graphics effects feasibility study (32-bit color, AA, lighting, filters, RT)
- Controls: per-camera control schemes (classic vs alternate-camera) + controller alternates
- Launcher: controller alternate column + per-camera control schemes
- Launcher: controls-window layout polish + lock main window size
- Launcher: refresh icon/branding assets + fix CHANGELOG em-dash encoding
- Launcher: nudge Alt. Cam Controls checkbox/help right + trim help wording
- release-nightly: first release of a stream now generates a real changelog
- release-nightly: sort releases by parsed version, not createdAt
- Controls: make the dev-console toggle key rebindable (key_console)
- Letterbox: draw fixed-cam cinematic bars in OT2 (full vertical ortho) — fixes ghost bar
- Revert cutscene-border OT2 move — wrong target for the fixed-cam top-bar report
- Controls: bind turn-left/right to arrow keys in altcam scheme (fix TPS/OTS menu nav)
- Console give: unlock HyperBlaster fire gate; add [MELEEDMG] probe for katana


## v2026.06.23.1 -- 2026-06-23
- BIG UPDATE! Sorry for the delay on this one, but wanted to get this new launcher out so that I wouldn't be so hesitant to push updates in the future. I will make a video going over this update as well, but I will list the big change below, and all the Claude commit summaries are below that.
- PGXP: Seams/Missing faces FIXED, warping almost fixed and is barely noticeable now. PGXP is near perfect!
- Collision into Invisible Walls: FIXED!! None so far after extensive testing!
- Launcher: Added build settings to choose a custom build from any point (will not overwrite the launcher). After choosing a custom build, click apply and then download build! This will be useful if I ever break things in an update and you need to revert to an old Version
- Launcher: Also added ability to use a custom repo. Meaning if anyone forks and mods my port, you can load their version directly through the launcher! They just have to post releases in their fork in a way the launcher will recognize (more on that soon)
- Launcher: Controls section, added mouse bindings and EXPERIMENTAL Thirdperson\Over the shoulder camera options! Be sure to bind the keys appropriately for these modes, and with it enabled you can press F9 or the right stick to cycle camera modes.
- Thirdperson/Over the Shoulder: Use right stick or mouse to look, left stick\WASD (optional) to move. Strafing is enabled in this mode as well.
- OTS Camera: By default press middle mouse to swap shoulders!
- TPS/OTS: You can turn on a crosshair, or invert the mouse/right stick
- Steam Deck: Merged fix from PR from sergiomanzur that may fix Steam Deck graphics. If not there is also more logging available to help. Please post in the github issue if you're still seeing issues!
- Crashing\Visual glitches: Lots should be fixed, let me know what remains! Please comment on the comprehensive issue list in GitHub
- Console: Now has history via up/down and will not glitch while the game is paused. Should be doc available for setflags console command usage for triggering cutscenes manually, if not will make it soon.

Coming soon: Inventory item backface visibility issue fix. Other visual bug and crash fixes.


Commit summaries:
- PsyCross: Mesa VRAM color fix (Steam Deck / Proton) — cherry-pick of #14
- fix: ending/Nowhere crashes — map7_s03 endings + map6_s04 Cybil boss
- data: extract ending/park zero-stub tables (force-field, Dahlia lightning, sand, disc, sprites)
- fix: invisible-wall random sprint-smack — preserve forward-input debounce
- feat: switch PC aiming from the combat_target.c shim to the real decompiled targeting
- tools: extract_map_data.py requires an explicit map name (or 'all')
- fix: boss camera framing — read the live swing angle, not the dead D_800EBB5A alias
- targeting: remove the PC Air Screamer melee carve-out — func_8005CD38 is now the unmodified original
- Restore Cybil-approach ambient enemies (larval stalkers / grey children)
- Fix map4_s04 (Lisa/Dahlia) cutscene rainbow corruption -- extract 3 draw-rect stubs
- Collision: re-enable round-obstacle collision + scope preload collision to local cells
- Launcher + nightly: zip releases on a beta branch, configurable repo/branch/build, safe launcher self-update
- Launcher: harden custom-repo updates + link nightly releases to source
- Launcher: a pinned build now downloads/plays that build ("Download Build")
- Pause: stop console/game double-pause fight + pause world while examining
- Revert the examine/message world-pause — PSX doesn't pause plain examines
- Launcher: paginate all releases, alpha/beta wording, one-time old-build warning
- Launcher: soften old-build warning wording (break vs corrupt)
- Launcher: separate Download Build from Check for Updates; track highest-ever build per branch
- Launcher: "Redownload Build" when the selected build is installed + button tooltips
- Launcher: nudge update status label + progress bar position
- Launcher: preview changelog before installing + always re-promptable updates
- Control styles: promote TPS camera out of debug into a real camera-mode system
- Control styles Stage 2: de-isolate TPS input + secondary/mouse binds + sticks
- Launcher Stage 3: Experimental controls — Control Style, Change Camera, mouse/secondary
- TPS refinements: aim zoom, diagonal strafe, always-on alternate binds
- Launcher: simplify controls — always-on alternate binds, Inventory, aim zoom, overwrite prompt
- Launcher: auto-migrate alpha -> beta when the beta stream goes live
- Add Over-the-Shoulder camera mode + aim crosshair
- Launcher: Over-the-Shoulder + crosshair + Swap Shoulder bind
- Launcher: rename "TPS/OTS Aim Zoom" + add Experimental tooltips
- Blood white-edge fix + PGXP pgxpdepth console + crosshair tweak
- Launcher: OTS always in Control Style dropdown + Allow-debug moved off Reset button
- Blood: lower additive-color cap 0xA0 -> 0x80 to further reduce white edges
- Characters: precise backface cull so faces stop dropping at distance (PGXP)
- Melee: stop phantom swings after release (flush tap-queue on button release)
- Inventory: precise float backface cull so rotating items stop being see-through
- Launcher: single-instance — warn and exit if already running
- Inventory: sort item faces by centroid depth (fix rotation see-through)
- TPS/OTS: fix shooting dying after mode-cycling + config-bound controller run
- TPS/OTS: reset orbit camera on entry + control-default cleanup + F4/Backspace
- TPS/OTS: drain mouse delta on capture transition (no camera jerk on entry)
- Inventory: resolve see-through via per-prim depth + forced depth test (item pass)
- Inventory depth fix: scope force-depth to the world-frozen item screen only
- Inventory depth fix: gate force-depth on the PAUSE flag, not sysState
- Inventory depth fix: force-depth when sysState == Gameplay (status-menu view)
- Revert inventory see-through work (backface cull / centroid otz / per-prim depth)


## v2026.06.22.1 -- 2026-06-22
- Revise website link and console command details
- PsyCross: Mesa VRAM color fix (Steam Deck / Proton) — cherry-pick of #14
- fix: ending/Nowhere crashes — map7_s03 endings + map6_s04 Cybil boss
- data: extract ending/park zero-stub tables (force-field, Dahlia lightning, sand, disc, sprites)
- fix: invisible-wall random sprint-smack — preserve forward-input debounce
- feat: switch PC aiming from the combat_target.c shim to the real decompiled targeting
- tools: extract_map_data.py requires an explicit map name (or 'all')
- fix: boss camera framing — read the live swing angle, not the dead D_800EBB5A alias
- targeting: remove the PC Air Screamer melee carve-out — func_8005CD38 is now the unmodified original
- fix: Cybil approach area — restore ambient larval stalkers / grey children (free the NPC cap at the boss cutscene instead of blocking the whole map)
- fix: map4_s04 (Lisa/Dahlia) cutscene rainbow corruption — extract the 3 zero-stub draw-area/offset rects
- fix: walk-through poles / hydrants / streetlights — re-enable round-obstacle collision (console `OBST 0/1`)
- fix: preload-only phantom invisible walls — scope collision to the player's local chunk window (console `COLLSCOPE 0/1`)
- launcher: new Build Settings (pick repo / branch / build), zip-release support, and safer self-update — the launcher now only updates itself when the build is actually newer, and asks first

## v2026.06.21.2 -- 2026-06-21
- Fixed text going off screen and cutscene letterbox issues that started after the last patch, let me know if you see any issues anywhere.
- PGXP: Improved warping on edges of screen,  gone at 4:3 and barely noticeable at 16:9. Working on more tweaks and fixing the seams/invisible spots at a distance.
- Timestamped per-run logs
- Changed map console command to edit map in config and not instantly to go the map (wasn't working)
Please check the github to see all the known issues that are being worked on. Also if you experience a random crash, posting your log file somewhere like github is really helpful.                                         

Claude list:                 
- docs: add Controls (R/F6/F8) + Console Commands section; fix Debug keys
- console: Up/Down arrows recall recent commands (8-entry history)
- fixes: cutscene vfov-crop skip, subtitle msgshift, map=config, console history
- fix: cutscene letterbox bars — border-state gate + no fixed-cam vshift

## v2026.06.21.1 -- 2026-06-21
- ASPECT RATIO FIX!! Game now has proper aspect ratio and FOV in 4:3 and 16:9. Fixed camera framing should be corrected as well. Please inform me of anything that is still wrong with FOV or camera framing.
- Running into walls: Hopefully fixed now even at high FPS, let me know if not.
- Invisible health drinks fixed courtesy of sergiomanzur on GitHub!

Claude list:
- Player: make run-into-wall smack gate self-consistent + [WALLANIM] probe (#42)
- Player: throttle forward-input history to 30 Hz — fixes random run-into-wall smack (#42)
- Aspect: fix Harry-too-wide in Hor+ — 320x224 framebuffer needs 15/14 PAR, not square
- config: correct pixel_aspect comment (default is now 15/14 PSX-faithful, not square)
- Fix invisible cafe health drink placement
- Aspect/vertical: render the 224-line field, not the full 448 buffer (squish + over-tall FOV)
- Aspect/vertical: gate the interlace-field fix to the 3D world only (un-break title)
- Aspect/vertical: console `vfov` to tune the 3D-world vertical FOV crop (PsyCross)
- PsyCross: bump to 212093d (vertical-FOV fix + strip aspect debug logging)
- config: drop the pixel_aspect knob — bake the correct 15/14 (PSX 320x224 -> 4:3)
- console: fix VFOV command (was lowercase; console uppercases all input)
- camera: shift fixed-angle-camera shots' vertical framing up to match PSX
- Merge pull request #12 from sergiomanzur/pc-port

## v2026.06.20.1 -- 2026-06-20
- Fixed walking while aiming, can now walk and aim always.
- Invisible wall collisions while running should be greatly reduced, let me know if they still happen frequently.
Next: Still working on aspect ratio fixes and fine tuning PGXP. Everything else mentioned coming after those.
There are a lot of bugs that I am aware of and still working on, will post a github issue for consolidation.

Claude list:
- collision: [WALLSTOP] probe — capture the ACTUAL invisible-wall block
- collision: [WALLSTOP] v2 — same-frame wrap/chunk/world capture
- collision: [WALLSTOP] kind=4 — capture chara-vs-chara (NPC) blocks
- collision: [WALLSTOP] kind=5 — static obstacle block; complete path coverage
- collvis: draw the actual blocking obstacle as a RED box (any chunk)
- collision: disable ptr_18 round-obstacle solid collision (invisible-wall fix)
- collvis: draw near ptr_18 obstacles red even when collision is OFF
- collvis panel: show Harry's animation-driven collision cylinder offset
- collision: [WALLEDGE] latched diagnostic for the wall-edge bump reaction
- collvis panel: move WALLEDGE readout to the top (was cut off bottom)
- collision: ROOT FIX invisible walls at full speed — cap spurious slope factor (#42)
- Player: fix can't-start-walking-while-aiming (aim-state sites the prior fix missed)
- collision: raise slope-alpha cutoff 0.5->0.8 (walk-speed floor-as-wall spots)
- collision: reject phantom floor above Harry's head (indoor invisible-wall)
- Bump PsyCross: re-apply 4:3 display-aspect fix (Harry-too-wide)
- Player: stop spurious "run into wall" hands-up on open ground (root cause)
- Revert aspect 4:3 re-apply — was applied without approval

## v2026.06.20.1 -- 2026-06-20
- collision: [WALLSTOP] probe — capture the ACTUAL invisible-wall block
- collision: [WALLSTOP] v2 — same-frame wrap/chunk/world capture
- collision: [WALLSTOP] kind=4 — capture chara-vs-chara (NPC) blocks
- collision: [WALLSTOP] kind=5 — static obstacle block; complete path coverage
- collvis: draw the actual blocking obstacle as a RED box (any chunk)
- collision: disable ptr_18 round-obstacle solid collision (invisible-wall fix)
- collvis: draw near ptr_18 obstacles red even when collision is OFF
- collvis panel: show Harry's animation-driven collision cylinder offset
- collision: [WALLEDGE] latched diagnostic for the wall-edge bump reaction
- collvis panel: move WALLEDGE readout to the top (was cut off bottom)
- collision: ROOT FIX invisible walls at full speed — cap spurious slope factor (#42)
- Player: fix can't-start-walking-while-aiming (aim-state sites the prior fix missed)
- collision: raise slope-alpha cutoff 0.5->0.8 (walk-speed floor-as-wall spots)
- collision: reject phantom floor above Harry's head (indoor invisible-wall)
- Bump PsyCross: re-apply 4:3 display-aspect fix (Harry-too-wide)
- Player: stop spurious "run into wall" hands-up on open ground (root cause)

## v2026.06.19.2 -- 2026-06-19
- PGXP: FIXED!! From testing, PGXP has been greatly improved. There are still ocassional seams or missing faces, but much better than before.
Coming soon: Aspect ratio fixes (no more wide harry) + things already mentioned 

- docs: PGXP complete shadow-memory rewrite plan (DuckStation-faithful)
- Bump PsyCross: PGXP shadow-memory rewrite Steps 0-2 (safe floor)
- PGXP rewrite Steps 3-4: shadow copy propagation in the world+char drawers
- PGXP rewrite Step 6: remove dead bridge call sites + weld console cmds
- PGXP: shadow-store lit-character verts (Harry was affine)
- PGXP: WELD/WELDW console tunables + bump PsyCross (seam weld)
- Bump PsyCross: default PGXP seam weld OFF

## v2026.06.19.1 -- 2026-06-19
- PGXP: I was a little hasty, seams were only gone because it's mostly affine. Actively working on a fix but left it as is for this release to get crash fixes out.
- Crash Fixes: The Church cutscene crash and similar crash points should be fixed. Late game boat door crash *might* be fixed but needs testing. If not it will be tonight.
- Console: You can now type ., -/_, and =/+ (Shift-aware) — so commands like weld 2.5 and inveqy -50 work.
- Console: tays open after a command now (run several in a row); press Enter on an empty line, or ~, to close it.
- Controls: Esc now always works (no debug mode needed): warm-resets to the title in-game, and quits the game at the title screen.
- Controls: F1 (PGXP toggle) now always works without debug controls.
- Debug: Crashes now write a full call-stack; crash reports are self-diagnosing.

## v2026.06.18.6 -- 2026-06-18
- PGXP Improved! No more seams or messed up tree billboards. Still working on making characters look better. 

- Bump PsyCross: PGXP slot-index vertex matching (tree-warp fix)
- PGXP: park verts in the second mesh-render path (fix tree-foliage smear)
- PGXP: park verts in the model GT3/GT4 drawer (func_8005AC50) — tree foliage
- PGXP: force billboards affine (Gfx_BillboardDraw) — fix tree-foliage spikes
- PGXP: snap-XY around the character bone-draw loop (fix joint seams)

## v2026.06.18.5 -- 2026-06-18
- This update reduces the issues with the inventory screen to match the original PSX by fixing vertical scaling, positioning, and dimming. Backface issue still present, will be fixed soon. 
Coming soon: More aspect fixes, monster reworks to fix animations, real PSX style targeting, sound fixes, and corruption/crash fixes.

- walls/worm: [WORM] vulnerability-window probe in the LIVE twinfeeler code
- worm: port stranded sz==0 div-by-zero guard into the LIVE twinfeeler copy
- inventory: [INV-ASPECT] one-shot probe for the squished-item report
- inventory: add `invaspect` toggle for squished item-preview models
- inventory: default square aspect, scale size-only (fix equipped pos), tunable
- inventory: default scale 125, Y nudges, off-center dimming
- inventory: per-slot dimming + defaults invcary 50 / inveqy -50

## v2026.06.18.4 -- 2026-06-18
- Debug: replace key 6 non-working grey-child spawn with kill-nearby-enemies + killall cmd

## v2026.06.18.3 -- 2026-06-18
- Restored screen fade in between rooms (any FPS)
- Bump PsyCross: [WORLDSPLIT] world-draw-path diagnostic
- walls: log swept-collision geometry in [WALL-HIT] (#42)
- Fix invisible walls at full run: cap gameplay timestep at 30fps (#42)
- Fix missing room-transition / level-load fade: clamp fade dt, drop *4

## v2026.06.18.2 -- 2026-06-18
- Reverted an update that I didn't mean to include yet that wasn't working and hasn't been tested enough (aspect correction).

- Bump PsyCross: 4:3 display-aspect fix for Harry-too-wide
- Bump PsyCross: revert 4:3 aspect fix (no visual effect; investigating)

## v2026.06.18.1 -- 2026-06-18
- Restore PAL/NTSC-J Gillespie house-fire newspaper (missing from NTSC-U). It is located in Nowhere and you have to read the earlier newspaper to find it. You can manually unlock it by typing "setflag 393 1" in the console
- TMD cache: ring eviction instead of memmove (fix inventory-scroll GTE crash)
- Inventory preview: skip malformed TMD object instead of crashing + log it
- Inventory: clear carousel model slots up front (fix stale-model flung verts + crash)
- Diagnostic: [ITEM-DRAW] flushed dump of each item preview model before draw
- Bump PsyCross: [ASPECT] diagnostic
- Remove session diagnostics ahead of release
- Bump PsyCross: route [ASPECT] probe to log + dump GTE FOV/viewport


## v2026.06.17.1 -- 2026-06-17
- Blood should now be RED everywhere except where it's not intentionally! (still minor issues but will work on it)
- BGM speed may be fixed but haven't tested everywhere
- Good and Good+ endings both now play without SFX looping and you can watch the whole cutscene before the fight
- Fixed other random crashes like some cases when using cheats to give all weapons

Coming soon: Aspect fixes, real game targeting system code brought in to replace shim, PGXP clean up, Updated launcher with custom build support


## v2026.06.16.4 -- 2026-06-16
NOTES: This should fix a couple of ending cutscene and post ending crashes.
Blood color is being worked on. Also working on fixing a bug that prevents you from kicking air screamers.

- Fix off-color blood on certain maps: re-apply trusted blood color per map (#41)  --NOT FIXED YET
- blood-cfg: log extraBloodColor byte address for root-cause watchpoint (#41)
- console: give gasoline (chainsaw/drill fuel) + include it in allweapons
- Fix good+ ending crash: clamp Gfx_MeshDraw OT depth (split-pointer stomp)
- Fix bad-ending crash: guard divide-by-zero in func_800DA4EC / func_800DA4B4

## v2026.06.16.3 -- 2026-06-16

---New debug console commands

Open the console with ~ (tap to toggle, hold to type a command, debug controls must be enabled in launcher).

- give <item> — grant weapons, ammo, recovery, and story/ending items. Run help give and help give 2 for the full list. Firearms come with ammo; give allweapons grants everything. Examples: give shotgun, give flauros, give aglaophotis.
- getflags — show the event flags that determine the ending and their current state.
- setending bad | bad+ | good | good+ — set the two flags that select the SH1 ending (Cybil saved + the Kaufmann/good path) in one go. Set it before reaching the ending.
- setflag <n> 0|1 — set any event flag (0–1663) directly.
- clearflags — forget console-set flags so they stop carrying into the next New Game / map load.

Flags set via the console persist through a New Game boot, so you can choose an ending in the menu, set it, then start a New Game into the ending map to test it without a save.

- PGXP: bump PsyCross — persist shared-vertex parks, strip diagnostics
- console: expand give (story items), add getflags/setflag for ending testing
- console: add setending bad|bad+|good|good+
- console: persist console-set flags across map warp
- console: add clearflags to forget pending console-set flags
- console: re-apply pending flags on New Game boot (config-map ending tests)
- release-nightly: pause after writing CHANGELOG.md for manual edit

## v2026.06.16.2 -- 2026-06-16
- PGXP: record effect-quad vertex addresses for deterministic matching
- PGXP: bump PsyCross — reject mismatched precise coords (geometry-warp fix)
- PGXP: bump PsyCross — deterministic environment rendering working

## v2026.06.16.1 -- 2026-06-16
- Revise README with project status and known issues
- Revise gameplay status and fix wording in README
- Merge branch 'pc-port' of https://github.com/SlickAmogus/silent-hill-decomp into pc-port merging readme
- Enhance support section with additional contact info
- map7_s03: size g_NpcBoneCoords + D_800F4B40 correctly (stretch/crash fix)
- Tooling: audit_stub_layout catches oversized struct work-buffer stubs
- map7_s03: clamp otz-derived OT bucket indices in ceremony effect drawers

## v2026.06.15.5 -- 2026-06-15
- Tooling: per-area scoping + alias-write detection + map attribution for audit_zero_stubs
- Twinfeeler: extract remaining ROM tables found by zero-stub audit
- Twinfeeler: restore D_800E08F0 == D_800E0698.field_258 alias (burrow dust pos)
- Tooling: audit_stub_layout.py — find truncated stubs, NULL-ptr stubs, lost aliases
- Twinfeeler: stop the acid-attack loop SFX (1561) - constant squish fix
- Player: fix can't-start-walking-while-aiming at high FPS
- map7_s03: reformat ending-cutscene pointer tables (bad-ending crash fix)
- map7_s03: extract ending-image color/palette tables (D_800EB010/410/814/C18)
- map7_s03: extract D_800EB810 position-spread scale (Q12 1.0)

## v2026.06.15.4 -- 2026-06-15
- Twinfeeler: extract real dust/dirt color ramp (D_800DAA58)
- Twinfeeler: extract emerge tables (Y-pos sink + sfx loop fixes)
- Debug: move kill-Harry from number-1 key to `kill` console command

## v2026.06.15.3 -- 2026-06-15
- Remove [FBFEED] probe; boss flicker addressed by SetDrawStp/SetDrawOffset OT fix
- PAL/EUR: generate filetable.c.EUR.inc + fileenum.h.EUR.inc (2310 files)
- PAL/EUR: runtime region support (single exe, auto-detect disc)
- PAL: autodetect fallback prefers US over PAL among unnamed .bins
- Bump PsyCross (VRAM-bounds clamp) + log boot-TIM file index for PAL
- PAL: fix no BGM/SFX — region-remap the g_AudioData sound table
- PAL: render Grey Children as Mumblers (the PAL censorship)
- Twinfeeler: clamp worm-segment OT bucket index (black flicker fix)
- Twinfeeler: fix worm-particle OT corruption (black flicker root)

## v2026.06.15.2 -- 2026-06-15
- Bump PsyCross: SetDrawStp/SetDrawOffset/SetPolyG3 (no-op prim-stub OT fix)
- Fix map2_s00 street spawns: extract D_800F1CAC progression variants

## v2026.06.15.1 -- 2026-06-15
- Diag: [FBFEED] probe — is motion-blur sampling a black framebuffer?
- Fix cult-TV cutscene crash: SetDrawStp was a no-op stub
- Fix SetDrawOffset no-op stub corrupting the OT (boss-fight [OT-SCAN])
- Fix SetPolyG3 no-op stub (same garbage-OT-prim class)

## v2026.06.14.6 -- 2026-06-14
- Diag: log TV screen data ([TVSCR]) to confirm D_800DB874 at runtime
- Version resource: update per-commit (not per-build) to avoid needless relinks
- Diag: read off-screen TV texel+CLUT from VRAM ([TVSCR2])
- Fix mall TV screens empty when off: restore D_800DB91C clutX=448

## v2026.06.14.5 -- 2026-06-14
- Stop distant rooms showing in single-cell boss arenas (exact-cell draw)
- Fix exe zero-stubs shadowing maps' extracted data (TVs, OT crash, +others)
- Add Win32 version metadata to the exe (auto-updating File Version)

## v2026.06.14.4 -- 2026-06-14
- Guard coord-hierarchy walk against non-canonical (truncated) links

## v2026.06.14.3 -- 2026-06-14
- Fix ending cutscene freeze/desync: per-phase DMS header selection

## v2026.06.14.2 -- 2026-06-14
- Diag: dump distinct map-geometry texture-page/CLUT combos ([MAPTEX])
- Fix ending-arena magenta poles: restore (0,0)-page textures each frame
- Revert no-op (0,0) re-upload in ending handler
- Diag: log world-lighting setup ([LIGHT]) to find ending over-brightness
- Diag: one-shot VRAM dump during ending to inspect arena textures
- Diag: per-object texture-page probe ([MT]) with screen-Y
- Fix ending texture corruption: real cutscene chara texture descriptors

## v2026.06.14.1 -- 2026-06-14
- map7_s03 ending: stop crash + fix mid-scene CLUT corruption
- Add application icon (Cheryl) to SilentHillPC.exe
- Update app icon from revised source + track .ico as RC dependency

## v2026.06.13.30 -- 2026-06-13
- Inventory: skip 3D item-preview stretch-correction when pillarboxed
- Diag: log puppet-nurse hurt SFX to settle wrong-damage-sound report
- Melee: apply damage once per swing per target (fixes hurt-SFX machine-gun)

## v2026.06.13.29 -- 2026-06-13
- Log boss-pool stomp (probe ending crash/corruption timing + cause)
- Ending cutscene glitches: suppress framebuffer->VRAM store during map7_s03 ending

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
- PsyCross: PGXP v4 — texture-only shader + un-quantised flat depth
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
- Launcher: strip inline update flow — updater is the only update path
- Retire SH1Updater — launcher self-updates via the rename swap

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
- docs: add Port_Fixes_Index — curated game-code PC-port fixes
- logging: remove ~345 stale troubleshooting traces (keep infra)
- logging: trim [SH] boot/chunk spam + gate per-frame state logs
- logging: strip dead scaffolding left by the trace removal
- docs: add combat/animation/cutscene band-aids to Port_Fixes_Index
- Fix chemical-on-hand cutscene crash: guard div-by-zero in smoke particle
- docs: §1 now covers div-by-zero (hand cutscene crash) alongside NULL derefs

## v2026.06.09.4 -- 2026-06-09
- log: remove stale per-frame [MCRD2] spam + the [ALLEY1] Cheryl diagnostic
- debug: key 6 spawns a Grey Child; add [CHMOVE] Cheryl movement trace
- math: restore overflow-safe Math_Vector2/3MagCalc on PC (merge regression)
- math/cheryl: target the overflow fix to the chase gates, not the global macro
- debug: grey-child spawn — bypass per-area NPC cap + guard model load
- cheryl: remove [CHMOVE] diagnostic trace (Cheryl run-through fix confirmed)
- diag: log failing object name + item-LM magic in [WOBJ] find-fail (map1_s00 banding)
- cat: guard NULL playbackFunc — fixes school crash (merge regression)

## v2026.06.09.3 -- 2026-06-09
- pc_port: bump PsyCross — pillarbox bars stay black on item-examine screen

## v2026.06.09.2 -- 2026-06-09
- (no commits since last release)

## v2026.06.09.1 -- 2026-06-09
- Merge upstream Vatuu/master (Jun 2026) + merge resolution (squashed)
- pc-port: fix merge regressions — grey-child crash, melee, map, transition flash
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
- Revert lighter-hold idle guard (382a96139) — no-op for the actual bug
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
- gfx: bump PsyCross — fix OT depth direction
- gfx: bump PsyCross — fix a_zw attrib binding in non-PGXP path
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
- dbg_overlay: fix rendering — correct UV orientation, LSB font bit order, GL init timing
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
