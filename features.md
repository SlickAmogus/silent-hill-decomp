# Silent Hill PC feature catalog

> **Status — Current guide.** Source and build configuration are authoritative. Use the [operational reference](pc_port/docs/Console_And_Debug_Reference.md) for controls, console commands, and debug tools, or the [documentation index](pc_port/docs/README.md) for design and audit records.

| Code | Meaning |
|---|---|
| **I** | Implemented and expected to work in the current build. |
| **E** | Experimental, partial, restricted, platform-specific, or awaiting coverage. |
| **D** | Disabled, superseded, planned, or absent. |

## Launch and platform

| Status | Area | Current behavior |
|---|---|---|
| I | Windows CLI | `SilentHillPC.exe -data <directory>` changes only the directory searched for disc images. A direct image path does not work; config, mods, logs, and saves remain relative to the working directory. `-h` / `--help` prints usage. Unknown options and a trailing `-data` are ignored. |
| I | Windows | Native C11/C++17 game using PsyCross, SDL2, OpenGL, OpenAL, and JPEG; separate .NET Framework 4.7.2 launcher/updater. |
| E | Linux/macOS | Current CMake, texture-pack code, and map loader are POSIX-safe; `build.sh` covers Linux and macOS, and overlays use `.so`/`.dylib`. The Windows launcher is not available on POSIX. |
| I | Disc selection | USA, PAL, and NTSC-J probing; `region = auto` prefers USA, then PAL, then NTSC-J. `disc_image =` is empty/automatic by default; a value selects an exact `.bin` filename in `gamedata/`. |
| I | Launcher/updater | Disc/display probing; SHA-256 loose/ZIP updates that preserve config and saves; launcher self-update only to a newer version. |

The game has no RAR reader. Texture packs are directories or ZIP files; the Windows launcher may extract a RAR into a loose `.rar.extracted` folder before launch.

## Current capabilities

| Status | Area | Current capability |
|---|---|---|
| I | Rendering | Arbitrary resolution; windowed/fullscreen/borderless; 4:3, Hor+, or stretch 3D presentation; optional 2D pillarboxing; VSync/FPS limits; nearest/dither/bilinear filtering; MSAA. |
| I | Effects and lighting | Nine post-process looks, four tone maps, four flashlight modes, adjustable beam/effect mix, and real-time flashlight shadows. |
| E | PGXP | Runtime-effective precise projection, perspective-correct interpolation, edge handling, and near clipping; default off. `USE_PGXP=0` is vestigial because current paths are built unconditionally and runtime-gated. |
| I | World rendering | Culling override, chunk preload, expanded resident texture pool, and texture overrides/packs. |
| E | Whole-map exteriors | Opt-in scenic draw/preload for outdoor cells; default off, resource-heavy, and awaiting coverage after the v2 redesign. |
| I | Classic controls | Fixed-camera tank controls through the original PSX input path; primary/secondary keyboard-or-mouse and controller action binds. |
| E | Alternate controls | TPS, OTS, and FPS styles share alternate bindings; mouse/pad look, OTS shoulder swap, aim assist/crosshair, optional 2D movement, and optional FPS head tracking. |
| I | Mouse cursor | `mouse_cursor = 1` by default. Mouse hover/click/wheel works in the title menus, save/load, options, and cursor puzzles only while the OS pointer is free; captured TPS/OTS/FPS look remains unaffected. |
| I | Save/load | Original MCD format, checksum, endings, ranking, and New Game+ state. F6/F8 open the original save/load screens during settled gameplay; they are not snapshots. |
| I | Audio | Original SEQ/VAB/SMF BGM through SPU/OpenAL; positional/ambient SFX; ADSR and reverb mapping; separate BGM, SE, XA voice, and FMV movie volume. |
| I | Disc media | Thirty table streams: nine XA-only voice entries plus 21 STR/MDEC movies with interleaved XA. Original BIN playback uses 4-bit XA and MDEC; 8-bit XA is unsupported. |
| I | AVI overrides | `gamedata/fmv/*.avi`; 64-bit/OpenDML and 4K+ files; MJPEG aliases, RGB DIB, and raw YUV video; PCM 8/16/24/32-bit integer or float32. Compressed audio plays silent; unsupported video falls back to the BIN movie. |
| E | Maps | 43 registry names: built-in `map0_s00` plus 42 optional map modules. Missing modules use metadata-only stubs. `chara_global` is an additional pseudo-map module for portable AI, not a story map. |
| I | Global character pool | `global_chara_pool = 1` by default. PC-side assets and portable AI backfill allow cross-map console spawns while native registrations and authored natural spawns keep priority. |
| E | Pool limitations | Map-bound actors/boss phases and Chicken can be `[no-ai]` statues; foreign monsters may use wrong or silent map-local SFX. See [Global character pool](pc_port/docs/Global_Chara_Pool.md). |
| E | Localization | PAL disc languages and NTSC-J Rev1/2 paths are implemented but partial; PAL still needs coverage, long localized messages can clip, and JAP0 proceeds after a warning with incompatible Rev1/2 tables. |
| I | Modified discs | Exact-image selection and disc-authoritative USA text support in-place fan translations; unchanged retail discs remain a no-op. |
| E | Loose modding | `gamedata/load/...` TIM/PNG/byte overrides require `allow_loose_files = 1`; texture replacement has documented palette/page restrictions. |
| E | Developer tools | Gated overlay console, cheats, collision visualization, free camera, FPS eye tuner, animation inspector, spawning, and render/audio tuning. |
| D | Beta monsters | No beta monster restoration is shipped. Chicken has assets but no AI; other proposals require invented behavior. |
| D | Randomizer | Not implemented; its document is scoping only. |

## Settings

All PC Options rows persist. Window state, VSync, filtering, PGXP, effects, flashlight, culling, FPS, volumes, controls, and map selection apply as shown; only MSAA and chunk preload require restart.

### PC Options

| Page | Status | Setting | Values; default | Apply |
|---|---|---|---|---|
| Graphics | I | Resolution | `640×480`, `1280×720`, `1366×768`, `1600×900`, `1920×1080`, `2560×1440`, `3840×2160`; **640×480** | Live |
| Graphics | I | Window Mode | Windowed, Fullscreen, Borderless; **Windowed** | Live |
| Graphics | I | VSync | Off, On; **Off** | Live |
| Graphics | I | Texture Filter | Off, Dither, Bilinear; **Dither** | Live |
| Graphics | E | PGXP | Off, On; **Off** | Live |
| Graphics | I | Antialiasing | Off, 2×, 4×, 8×; **Off** | Restart |
| Graphics | I | Post Process | Off, CRT, Scanlines, Vignette, Color Grade, Film Grain, Sharpen, PSX Retro, Cinematic; **Off** | Live |
| Graphics | I | Tone Mapping | Off, Reinhard, ACES, Filmic; **Off** | Live |
| System | I | Flashlight | Classic, Classic+Shadows, Modern, Modern+Shadows; **Classic** | Live |
| System | I | Beam Intensity / Size | `0..3`, step `0.1`; **1.20 / 3.00** | Live |
| System | I | Disable Culling | Off/On; **On** | Live |
| System | I | Preload Chunks | Off/On; **On** | Restart |
| System | I | FPS Limit | Off, 30, 60, 120, 240; **30** | Live |
| System | I | FMV Movie Volume | `0..1`, step `0.05`; **1.0** | Live |
| Controls | E | 2D Controls | Off/On; **Off** | Live |
| Controls | I | Mouse / Pad Sensitivity | `0.1..4.0`, step `0.1`; **1.0 / 1.0** | Live |
| Controls | I | First Person FOV | `55..110°`, step `1`; **67.4°** | Live |
| Controls | I | Invert Mouse Y / Pad Y | Off/On; **Off / Off** | Live |
| Controls | I | Aim Assist / Crosshair | Off/On; **On / Off** | Live |
| Controls | E | Map | 43 registry names; **`map0_s00`** | Next New Game |

The original Options and Extra Options screens retain brightness, controller configuration, vibration, auto-load/language, mono/stereo, BGM/SE/XA volume, weapon/view/movement controls, blood color, auto-aim, and unlockable view/bullet settings.

### Config and launcher

| Status | Key; default | Effect |
|---|---|---|
| I | `widescreen_mode = 1`, `menu_pillarbox = 1` | Hor+ 3D with 4:3 2D menus; alternatives are 4:3 pillarbox or stretch. |
| I | `refresh_rate = 0`, `vsync = 0` | Display-default fullscreen Hz; config also accepts adaptive VSync `-1`. |
| I | `skip_intros = 0` | Skip logos and opening movie when enabled. |
| I | `show_console = 0` | External console only: `1` or `3` shows it; `0` or legacy `2` does not. It no longer controls the overlay. |
| E | `allow_loose_files = 0` | Enable disc-layout overrides under `gamedata/load/`. |
| I | `resident_textures = 1`, `texture_packs = 1` | Expanded persistent textures and directory/ZIP packs under `gamedata/texturemods/`. |
| I | `global_chara_pool = 1` | Keep supported character assets resident and backfill portable AI. |
| I | `mouse_cursor = 1` | Enable free-pointer menus and cursor puzzles. |
| E | `bullet_decals = 0` | Load `gamedata/decal.png`; keep up to 64 impact decals. |
| E | `whole_map_exteriors = 0` | Scenic exterior draw/preload; requires resident textures and chunk preload. |
| I | `flashlight_intensity_fps = 2.10`, `flashlight_size_fps = 1.30` | Separate first-person beam values. |
| I | `post_process_intensity = 1.0`, `tonemap_intensity = 1.0` | Effect mix, each `0..1`. |
| I | `controller_movement = both`, `movement_original = 1` | Analog/D-pad source and original PSX lower-body movement. |
| I | `control_style = classic` | Classic, TPS, OTS, or FPS; the game publishes the style list. |
| I | `tps_aim_zoom = 1`, `aim_assist = 1`, `crosshair = 0` | Alternate-camera aiming defaults. |
| E | `immersive_fps_head_tracking = 0`, `control_2d = 0` | Experimental opt-in camera/movement behavior. |
| I | `altcam_button_sprint = 0` | Full stick can sprint; `1` requires the Run action. |
| I | `adsr = 1`, `reverb_scale = 0` | Sequenced-BGM envelopes; `0` uses the engine reverb mapping. |
| E | `unlimited_enemies = 0` | Raise natural room spawns to the 32-slot PC cap. |
| I | `xa_volume = 1.0`, `fmv_volume = 1.0` | Separate XA voice and movie volume. |
| I | `language = en`, `region = auto` | PAL text language and preferred detected disc region. |
| I | `disc_image =` | Empty means automatic; otherwise exact filename in `gamedata/`. |
| I | `map = map0_s00` | Start map for the next New Game; never a live warp. |
| I | `enable_debug_log = 0` | Opt-in `SilentHill.log`. |
| E | `allow_debug_controls = 0` | Gate console, cheat keys, free camera, and inspectors. |

The Windows launcher exposes the common display, graphics, flashlight, map/disc/region, control-style, binding, and developer settings. Binding names and defaults are in [controls.md](pc_port/docs/controls.md).

## Documentation

| Topic | Reference |
|---|---|
| Controls, 62 accepted console names, debug tools | [Console, controls, and debug reference](pc_port/docs/Console_And_Debug_Reference.md) |
| Binding configuration and mouse input | [Controls](pc_port/docs/controls.md) |
| Disc streams and AVI contracts | [FMV and XA stream files](pc_port/docs/fmv_files.md) |
| Cross-map assets and AI limits | [Global character pool](pc_port/docs/Global_Chara_Pool.md) |
| All current, audit, and historical records | [PC port documentation index](pc_port/docs/README.md) |
