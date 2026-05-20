## Silent Hill AI-Assisted PC Port

  This is an experimental PC port built on top of the PSX decompilation using PsyCross (PsyQ SDK Compatibility Layer originally for REDRIVER2), made with heavy AI-Assistance with Claude Opus 4.6 and 4.7.<br/>
  <br/>PsyCross: https://github.com/OpenDriver2/PsyCross<br/>

  I'm going to get it playable as possible, and I'm aiming to get the full game playable from start to finish. Aside from that, there will be as many optional PC enhancements as possible and potentially support for custom assets. 
  
  Beyond that, the port that will be more fleshed out in the long run will be Silent Engine (https://github.com/Sezzary/SilentEngine), which is a non-AI multiplatform port in the works. However, I'm hoping this one can help hold people over in the meantime.

  ### Status

  - **Main menu**: fully working — logos, FMV intro, options, save/load screens all display correctly
  - **In-game 3D world**: rendering working — textured environment, fog, snow particles, trees, buildings, lamp posts, ground geometry
  - **Player movement**: working — collision-based walk/run, wall collision mostly solid, floor height working, stairs working.
  - **Camera**: PSX fixed-camera system functional but isn't perfect; adjustable with numpad keys
  - **Audio**: SFX and BGM all working. Voices working.
  - **Map overlays**: All maps compile, most can be loaded.
  - **NPC AI**: Enabled, grey children, air screamers, and groaners confirmed working.
  - **Memory card**: save/load fully working, may still have progression bugs that will be fixed as they are found.
  - **Graphics**: Supports high resolutions, 16:9, high refresh rates, and uncapped FPS. Graphic settings adjustable in launcher.
  - **Updates**: Latest launcher can check for and install updates. Nightly builds posted at repo here: https://github.com/SlickAmogus/silent-hill-pc-nightly

  <!-- Main menu screenshot -->
  <img width="636" height="503" alt="image" src="https://github.com/user-attachments/assets/48f597c2-e629-463d-a516-998ed646dc88" />

<img width="1920" height="1080" alt="Build Screenshot 2026 05 19 - 01 31 25 75" src="https://github.com/user-attachments/assets/3cb30c50-cbc2-4026-ab2d-c5717239f398" />

<img width="1920" height="1080" alt="Build Screenshot 2026 05 17 - 20 00 56 26" src="https://github.com/user-attachments/assets/96d6d279-3d52-446b-9837-96e3934e021d" />

<img width="1920" height="1080" alt="Build 2026 04 06 - 01 09 37 03_1 mp4_snapshot_01 30 483" src="https://github.com/user-attachments/assets/0aeeacaa-99b4-41ef-8b6f-65b6e7d1d14f" />

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

  **Mode toggles** (work in any cam mode):

  | Key | Action |
  |-----|--------|
  | Numpad `*` | Toggle free-fly debug camera |
  | Numpad `0` | Toggle FPS cap |
  | Numpad `1` | Toggle wall collision (noclip) |
  | Numpad `2` | Toggle third-person follow camera (uses mouse to look) |
  | Numpad `3` | Reset (gameplay: teleport Harry to safe spawn / normal-cam: clear nudge accumulator) |
  | Numpad `.` | Toggle fog |
  | `` ` `` (backtick) | Open debug console (type `HELP` for commands) |

  **Top-row number keys** (gameplay logging / cheats):

  | Key | Action |
  |-----|--------|
  | `1` | Kill Harry (force death animation) |
  | `4` | Log current camera state as **BAD CAMERA POSITION** |
  | `5` | Log current camera state as **GOOD CAMERA POSITION** |
  | `6` | Log Harry's position |
  | `7` / `8` / `9` / `0` | TPS preset-pose loggers |

  **Numpad keys — camera nudges** (same bindings whether debug cam is on or off — moves the active camera):

  | Key | Action |
  |-----|--------|
  | `8` / `5` | Move forward / back (cam-relative) |
  | `4` / `6` | Strafe left / right |
  | `7` / `9` | Turn left / right (yaw) |
  | `+` / `-` | Tilt up / down (pitch) |
  | PgUp / PgDn | Move up / down (vertical) |
  | `/` | Print current camera coordinates |

  Numpad `3` clears all accumulated nudges (or in gameplay: teleports Harry to spawn). Hold a key for continuous adjustment. After tuning a cam, press top-row `5` (GOOD) to log the camera state for use in a `CamCorrection` entry.



  ### Support
  Message me wherever and I should answer. I am in most of the discords related to these projects as KushAstronaut.
  
  I work with more than just AI. If you like what I do:\
  [![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/F1F3K8V3B)


<br/><br/>

Silent Hill is © Konami and this does not contain any game assets. You must provide a legally obtained dump of Silent Hill for PSX to use.

