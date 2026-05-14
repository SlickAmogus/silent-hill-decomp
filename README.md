## About this fork

  This is an experimental PC port built on top of the PSX decompilation using PsyCross as a PSX hardware abstraction layer (SDL2 + OpenGL + OpenAL), made with Claude and with help from REDRIVER2's source code.<br/>
  <br/>PsyCross: https://github.com/OpenDriver2/PsyCross<br/>
  REDRIVER2: https://github.com/OpenDriver2/REDRIVER2

  I'm going to get it playable as possible, but I'm not sure yet how fleshed out it will be and there are no guarantees. I'm hoping this will be of help to other efforts like Silent Engine (https://github.com/Sezzary/SilentEngine), which is a non-AI multiplatform port in the works.

  ### Status

  - **Main menu**: fully working — logos, FMV intro, options, save/load screens all display correctly
  - **New Game**: loading screen plays (Harry running animation), transitions into gameplay
  - **Opening cutscene**: fully working — DMS camera, text, Harry visible with animations, ambient audio. Camera just needs adjustment.
  - **In-game 3D world**: rendering working — textured environment, fog, snow particles, trees, buildings, lamp posts, ground geometry
  - **Player movement**: working — collision-based walk/run, wall collision mostly solid, floor height working, stairs working.
  - **Camera**: PSX fixed-camera system functional but still needs adjustment; controllable with debug mode
  - **Audio**: SFX working via OpenAL; BGM loads; some sound effects working
  - **Map overlays**: 31 of 42 maps compile and load as DLLs; map1_s00 (Midwich Elementary) confirmed working
  - **NPC AI**: enabled, grey children now spawn
  - **Memory card**: save/load stubbed. Interacting with save points will cause odd behavior.
  - **Graphics**: Supports high resolutions, 16:9, high refresh rates, and uncapped FPS. Press numpad 0 to toggle FPS cap.


  <!-- Main menu screenshot -->
  <img width="636" height="503" alt="image" src="https://github.com/user-attachments/assets/48f597c2-e629-463d-a516-998ed646dc88" />


  <!-- In-game world screenshot -->
  <img width="631" height="475" alt="image" src="https://github.com/user-attachments/assets/45c7d367-bb16-4e61-b220-94d115aaefe6" />


 ### Short Instructions
-Extract somewhere on your PC<br>
-Put your game data inside the gamedata folder and name it Silent Hill (USA).bin<br>
-Run SilentHillPC_Launcher to configure the game, or just run SIlentHillPC. You may need to run the game once before the launcher will work because of smartscreen.<br>


  ### Controls (PsyCross defaults)

  | PSX Button | Keyboard |
  |------------|----------|
  | Cross | C |
  | Circle | V |
  | Triangle | F |
  | Square | D |
  | Start | Enter |
  | Select | Space |
  | D-Pad | Arrow keys |
  | L1 | A |
  | R1 | S |
  | L2 | Q |
  | R2 | W |


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


