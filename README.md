## Silent Hill AI-Assisted PC Port

  This is an experimental PC port built on top of the PSX decompilation using PsyCross (PsyQ SDK Compatibility Layer originally for REDRIVER2), made with heavy AI-Assistance from Claude Opus 4.6 and 4.7.<br/>
  <br/>PsyCross: https://github.com/OpenDriver2/PsyCross<br/>


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
- Certain effects and textures may still be missing or glitchy in specific areas
- Audio may loop incorrectly occasionally
- Ending cutscenes are glitchy
- Certain item placements seem angled or odd in the environment
- PGXP barely works and is glitchy
- Combat isn't perfect and may have slight issues.

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
-Put your game data inside the gamedata folder and name it Silent Hill (USA).bin<br>
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


  ### Building Prerequisites

  - **MSYS2/MinGW64** (Windows) with the following packages:
    ```
    pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 mingw-w64-x86_64-openal
    ```
  - **PsyCross** — Clone as a sibling directory:
    ```
    git clone https://github.com/OpenDriver2/PsyCross.git
    ```
  - Your directory layout should look like:
    ```
    parent/
      silent-hill-decomp/
      PsyCross/
    ```
  - **Game disc image** — A BIN/CUE dump of *Silent Hill* (USA) (SLUS-00707).

  ### Building

  From an **MSYS2 MinGW64** shell (first time):

  ```bash
  cd silent-hill-decomp/pc_port
  mkdir build && cd build
  cmake .. -G Ninja -DSH_BUILD_MAP_DLLS=ON
  cmake --build .
  ```

  Subsequent builds (incremental):
  ```bash
  cmake --build .
  ```

  From PowerShell (adjust path as needed):
  ```powershell
  & "C:\msys64\usr\bin\bash.exe" -lc "export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH && cd /c/path/to/silent-hill-decomp/pc_port/build && cmake --build ."
  ```

  `-DSH_BUILD_MAP_DLLS=ON` builds 31 maps as DLLs loaded at runtime. Without it only the starting area is available.

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
  
  ### Debug Controls

  Most debug events get echoed to **`SilentHill.log`**. To also see them live in-game, set `show_console` in `config.cfg`:
  - `0` = off (default)
  - `1` = external console window (stdout)
  - `2` = in-game overlay (renders inside the game window)
  - `3` = both

  **Top-row number keys** (gameplay cheats / loggers):

  | Key | Action |
  |-----|--------|
  | `0` | Toggle wall collision (noclip) |
  | `1` | Kill Harry (force death animation) |
  | `4` | Log current camera state as **BAD CAMERA POSITION** |
  | `5` | Log current camera state as **GOOD CAMERA POSITION** (paste `posDelta` / `yawDelta` / `pitchDelta` into a `s_camCorrections[]` entry) |
  | `6` | Log Harry's position |
  | `7` | Toggle invincibility (locks HP to max) |
  | `8` | Give 15 handgun bullets |
  | `9` | Toggle no-target (enemies ignore Harry via `CharaFlag_Unk4`) |
  | `-` | Give Chainsaw (+ 1 Gasoline Tank if you don't already have one) |
  | `=` | Give Rock Drill (+ 1 Gasoline Tank if you don't already have one) |
  | `` ` `` (backtick) | Open the debug command console (type `HELP` for commands) |

  **Numpad — mode toggles and utilities:**

  | Key | Action |
  |-----|--------|
  | Numpad `*` | Toggle free-fly debug camera |
  | Numpad `0` | Toggle "raw cam mode" (bypass camera corrections + zero nudges — use to capture a clean BAD baseline) |
  | Numpad `2` | Toggle third-person follow camera (mouse aims, RMB aim, LMB fire) |
  | Numpad `3` | In gameplay: rescue-Y teleport (vy back to last safe Y + 2.5u push back). In normal-cam mode: clear nudge accumulator. |
  | Numpad `.` | Log Harry's position with the `HARRY POSITION LOGGED` tag. Also toggles fog when debug cam is on. |

  **Numpad — camera nudges** (move the currently active camera, debug-cam or normal):

  | Key | Action |
  |-----|--------|
  | `8` / `5` | Move forward / back (cam-relative) |
  | `4` / `6` | Strafe left / right |
  | `7` / `9` | Turn left / right (yaw) |
  | `+` / `-` | Tilt up / down (pitch) |
  | PgUp / PgDn | Move up / down (vertical) |
  | `/` | Print current camera coordinates |

  Hold any nudge key for continuous adjustment. After tuning a cam, press top-row `5` (GOOD) to dump a paste-ready `s_camCorrections[]` delta.



  ### Support
  Message me wherever and I should answer. I am in most of the discords related to these projects as KushAstronaut.
  
  I work with more than just AI. If you like what I do:\
  [![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/F1F3K8V3B)


<br/><br/>

Silent Hill is © Konami and this does not contain any game assets. You must provide a legally obtained dump of Silent Hill for PSX to use.

