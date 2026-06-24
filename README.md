## Silent Hill AI-Assisted PC Port

  This is an experimental PC port built on top of the PSX decompilation using PsyCross (PsyQ SDK Compatibility Layer originally for REDRIVER2), made with heavy AI-Assistance from Claude Opus 4.6 and 4.7.<br/>
  <br/>PsyCross: https://github.com/OpenDriver2/PsyCross<br/>

  Project Website: https://sh1pc.com/

### Status

The port is 100% playable start-to-finish — the full game can be completed. Expect visual/audio glitches that are actively being fixed; none are progression-blocking.
- **Main menu**: fully working — logos, FMV intro, options, save/load screens all display correctly.
- **In-game 3D world**: rendering working — textured environments, fog, snow/particle effects, trees, buildings, lamp posts, ground geometry. 
- **Player movement**: working — collision-based walk/run, wall collision mostly solid, floor height and stairs working.
- **Combat**: working — handgun, hunting rifle, and shotgun fire reliably; melee weapons hit; break-free, death, and grab animations work. Chainsaw and Rock Drill have issues, others not mentioned untested.
- **Bosses**: functional — Twinfeeler (worm), Split Head, Floatstinger, and the Cybil fight all run; some boss visual effects still being polished.
- **Cutscenes**: mostly working — DMS-driven scenes, animations, and letterboxing play; isolated scenes still have effect/timing glitches.
- **Camera**: PSX fixed-camera system 1:1 with the original game for the most part.
- **Audio**: SFX, BGM, and voices all working; XA streaming plays cutscene voices directly from the disc image.
- **Map overlays**: all 42 maps compile as DLLs and load; room/door transitions working.
- **NPC AI**: full AI enabled — Grey Children, Air Screamers, Groaners, and others confirmed working.
- **Memory card**: save/load fully working; Quick save and quick load menus enabled with default keys F6 and F8.
- **Graphics**: high resolutions, 16**:9 (Hor+), high refresh rates, and uncapped FPS; optional runtime PGXP (buggy); settings adjustable in the launcher.
- **PAL/EUR support**: boots into English gameplay from a PAL disc and loads the censored "Mumbler" Grey Children; localized menus are still garbled (font-layout work pending) and an in-game language selector is planned.
- **Data accuracy**: the port replaces hundreds of missing PSX ROM data tables ("zero-stubs") that cause invisible/black/silent/mis-positioned effects; automated audits (audit_zero_stubs.py, audit_stub_layout.py) now surface remaining ones proactively, with the endings (map7_s03) as the next target.
- **Updates**: the latest launcher can check for and install updates. Nightly builds posted here**: https**://github.com/SlickAmogus/silent-hill-pc-nightly<br>

### Known Issues / Bugs

Major items currently being worked on (priority but in no particular order):

- Crash entering the clock tower in Midwich
- PGXP: seams at character joints and distant faces dropping out; warped geometry at screen edges
- Harry stops abruptly on invisible walls while sprinting
- Placeholder aiming shim — real aiming/camera-follow not yet wired in
- Combat runs too fast at high FPS
- Enemy AI needs a rework (some jerk through the floor during combat)
- Cutscene corruption, texture issues, and stray stretched vertices
- Missing/corrupted graphics in the options menu
- Linux / Steam Deck visuals broken (red tint)
- PAL text and languages not working

There's also a long tail of ending/Nowhere, per-cutscene, and miscellaneous issues.

**Full tracked bug list:** https://github.com/SlickAmogus/silent-hill-decomp/issues/13

<br>

  <!-- Main menu screenshot -->
  <img width="636" height="503" alt="image" src="https://github.com/user-attachments/assets/48f597c2-e629-463d-a516-998ed646dc88" />

<img width="1920" height="1080" alt="Build Screenshot 2026 06 15 - 21 52 22 18" src="https://github.com/user-attachments/assets/38f94098-fc11-41a2-b1bd-6109703e9603" />

<img width="1920" height="1080" alt="Build Screenshot 2026 06 15 - 21 52 59 83" src="https://github.com/user-attachments/assets/9e92ed6e-b7ca-4736-bdbd-aa4527d6063e" />

<img width="1920" height="1080" alt="Build Screenshot 2026 06 15 - 22 25 08 86" src="https://github.com/user-attachments/assets/13c6b711-4384-48c0-8821-7c4f8c2efde9" />

<img width="1920" height="1080" alt="Build Screenshot 2026 06 15 - 19 08 25 80" src="https://github.com/user-attachments/assets/6ce976f7-934d-4989-ad5d-22e0753dacf2" />


  <!-- In-game world screenshot -->

 ### Short Instructions
-Extract somewhere on your PC<br>
-Put your game data inside the gamedata folder and name it Silent Hill (USA).bin (or any name, it will find it automatically as of recent updates)<br>
-Run SilentHillPC_Launcher to configure the game, or just run SIlentHillPC. You may need to run the game once before the launcher will work because of smartscreen.<br>


  ### Controls (PsyCross defaults)

  | PSX Button | Keyboard |
  |------------|----------|
  | Cross | C |
  | Circle | V |
  | Triangle | Z |
  | Square | X |
  | Start | Enter |
  | Select | Space |
  | D-Pad | Arrow keys |
  | L1 | A |
  | R1 | D |
  | L2 | RSHIFT |
  | R2 | LSHIFT |

  PC-only keys (not on the PSX pad):

  | Action | Key |
  |--------|-----|
  | Reload weapon | R |
  | Quick Save | F6 |
  | Quick Load | F8 |

  All bindings are editable in the launcher or `config.cfg`.


  ### Building Prerequisites

  - **MSYS2/MinGW64** (Windows). Install MSYS2 from https://www.msys2.org/, then from an **MSYS2 MinGW x64** shell install the toolchain:
    ```
    pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2 mingw-w64-x86_64-openal
    ```
  - **PsyCross** — pulled in as a git **submodule** (the SH-specific fork). From the repo root:
    ```
    git submodule update --init --recursive
    ```
    This populates `pc_port/PsyCross`. Do **not** clone PsyCross separately — the build uses the submodule.
  - **Game disc image** — A BIN dump of *Silent Hill* (USA) (SLUS-00707). Place it at:
    ```
    pc_port/build/gamedata/Silent Hill (USA).bin
    ```

  ### Building

  **Easiest (Windows): one command.**
  ```
  pc_port\build.bat
  ```
  Double-click `build.bat` for a menu (incremental / clean rebuild / build+run / nuke), or pass a mode:
  `build.bat` (incremental), `build.bat rebuild`, `build.bat configure`, `build.bat run`, `build.bat nuke`.
  It auto-configures (Ninja + map DLLs) on the first run and rebuilds incrementally afterward.
  If MSYS2 isn't at `C:\msys64`, set `MSYS2_ROOT` first (e.g. `set MSYS2_ROOT=D:\msys64 && pc_port\build.bat`).

  **Manual** — from an **MSYS2 MinGW64** shell (first time):
  ```bash
  cd silent-hill-decomp/pc_port
  cmake -S . -B build -G Ninja -DSH_BUILD_MAP_DLLS=ON
  cmake --build build
  ```
  Subsequent incremental builds: `cmake --build build`.

  From PowerShell (adjust the path):
  ```powershell
  & "C:\msys64\usr\bin\bash.exe" -lc "export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH && cmake --build /c/path/to/silent-hill-decomp/pc_port/build"
  ```

  `-DSH_BUILD_MAP_DLLS=ON` builds the maps as DLLs loaded at runtime. Without it only the starting area is available.

  > **Note:** close the game before rebuilding — the linker cannot overwrite a running `SilentHillPC.exe` (the error is `cannot open output file SilentHillPC.exe: Permission denied`). `build.bat` detects this and tells you.

  ### Setting Up Game Data

  Place the disc BIN file at:
  ```
  silent-hill-decomp/pc_port/build/gamedata/Silent Hill (USA).bin
  ```

  ### Game File Table

  The game's assets are packed into two data archives (`SILENT.` and `HILL.`) on the disc. The file table contains 2074 entries. File names are encoded as 6-bit characters. The full file enum with paths is in [`include/main/fileenum.h.USA.inc`](include/main/fileenum.h.USA.inc).

  Key file types:
  | Extension | Type ID | Description |
  |-----------|---------|-------------|
  | .TIM | 0 | PSX texture image |
  | .VAB | 1 | Sound bank |
  | .BIN | 2 | Binary overlay / generic data |
  | .DMS | 3 | Cutscene animation data |
  | .ANM | 4 | Character animation |
  | .PLM | 5 | Player model data |
  | .IPD | 6 | Map/world geometry chunks |
  | .ILM | 7 | Illumination/lighting model |
  | .TMD | 8 | 3D model |
  | .DAT | 9 | Generic data |
  | .KDT | 10 | Collision data |
  | .CMP | 11 | Compressed data |

  ### Running

  ```bash
  cd silent-hill-decomp/pc_port/build
  ./SilentHillPC.exe
  ```

  The game expects the disc image at `./gamedata/Silent Hill (USA).bin` relative to the working directory.
  
  ### Console Commands

  An in-game command console for cheats, warps, and live tuning. Enable it in `config.cfg`:
  - `allow_debug_controls = 1`
  - `show_console = 2` (in-game overlay) or `3` (overlay + external window) - Note that even if console is set to off, holding ~ ingame will still toggle it. Really this setting is useless and will be updated to just external on or off.

  In-game, **hold `` ` ``** (the backtick / tilde key) to open the console, then ** tap** to bring up a `>` input prompt. Type a command and press **Enter** — commands are case-insensitive, Backspace edits. Type **`HELP`**, or **`DEBUG`** / **`DEBUG 2`**, to list everything live.

  | Command | What it does |
  |---------|--------------|
  | `give <item>` | Give a weapon / ammo / health — e.g. `give all`, `give health`, `give ammo`. `help give` lists items. |
  | `kill` / `killall` | Kill nearby / all enemies |
  | `map <name>` | Warp to a map (e.g. `map map0_s00`) |
  | `noclip` | Toggle wall collision (walk through walls) |
  | `obst` | Toggle round-obstacle collision (sprint-through) |
  | `pgxp [0\|1]` | Toggle PGXP perspective-correct rendering |
  | `fl` / `wl [color]` | Toggle flashlight / world light (optional color: red, green, blue, yellow, cyan, purple, orange, pink, white, default) |
  | `fmv [name]` | Play an intro / ending FMV (bare `fmv` lists them) |
  | `setending bad\|bad+\|good\|good+` | Force the ending path (set before the ending) |
  | `setflag <n> 0\|1` · `getflags` · `clearflags` | Inspect / set story event flags |
  | `quit` | Exit the game |

  **Live tuning knobs** (run with no argument to read the current value): `vfov`, `vshift` (vertical FOV / framing), `pgxpedge`, `weld`, `weldw` (PGXP), `alpha` (slope invisible-wall fix), `adsr` (looping-SFX envelope), and `invaspect` / `invscale` / `invcary` / `inveqy` / `invdim` (inventory item display). `DEBUG 2` documents them.

  ### Debug Controls

  Also requires `allow_debug_controls = 1`. Debug events echo to the per-run **`SilentHill_<timestamp>.log`**; set `show_console` (above) to watch them live in-game.

  **Cheats & tools (top-row keys):**

  | Key | Action |
  |-----|--------|
  | `Esc` | Warm reset to the title screen |
  | `0` | Noclip toggle (walk through walls) |
  | `4` / `5` | Map config prev / next (loads on New Game) |
  | `6` | Kill nearby enemies |
  | `7` | Invincibility toggle |
  | `8` | +15 handgun bullets |
  | `9` | No-target toggle (enemies ignore Harry) |
  | `-` | Give Hunting Rifle + 30 shells |
  | `=` | Give Shotgun + 30 shells |
  | `'` | Collision visualizer panel |
  | `[` / `]` | Drop A / B position markers into the log |
  | `` ` `` | Open the console (see above) |

  **Camera (numpad):**

  | Key | Action |
  |-----|--------|
  | Num `*` | Free debug camera on/off |
  | Num `2` | Third-person chase cam (mouse look) |
  | Num `8` / `5` / `4` / `6` | Fly forward / back / strafe left / right |
  | Num `7` / `9` | Turn left / right |
  | Num `+` / `-` | Tilt up / down |
  | PgUp / PgDn | Move up / down |
  | Num `/` | Print camera coordinates to the log |
  | Num `3` | Reset cam nudge / in-game rescue teleport |
  | Num `0` | Raw cam mode (zero all nudges) |
  | Num `.` | Log Harry's position (+ fog toggle in debug cam) |

  With the debug camera **off**, the same numpad keys nudge the normal game camera — a live camera-tuning aid.



  ### Support
  Message me wherever and I should answer. I am in most of the discords related to these projects as KushAstronaut, and I have a thread dedicated to this project in the Silent Hill channel of the PSX decompilation discord..
  
  I work with more than just AI. If you like what I do:\
  [![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/F1F3K8V3B)


<br/>

Silent Hill is © Konami and this does not contain any game assets. You must provide a legally obtained dump of Silent Hill for PSX to use.

