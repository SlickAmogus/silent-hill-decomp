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
-(Optional) Use jpsxdec (https://github.com/m35/jpsxdec/releases/tag/v2.0) to extract the FMVs from the game image. The files will be listed as "HILL" but should be in order according to the table at the github link.<br>
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

  ### Extracting FMVs

  Silent Hill stores its FMV cutscenes as raw PSX STR (Streaming) data interleaved with XA audio on the disc image. These are **not** regular files in the filesystem — they are located at specific disc sectors referenced by the file table.

  To extract the FMVs, you can use tools like **jPSXdec** (https://github.com/m35/jpsxdec) which can scan a BIN/CUE and detect/extract all STR video streams and XA audio streams automatically.

  With jPSXdec:
  1. Open the BIN file in jPSXdec
  2. It will index all streams on the disc
  3. Select the video streams and extract them as AVI into gamedata\FMV. The files will be listed as "HILL" but should be in order according to the table below, starting with C1_20670.
  4. XA audio streams can be extracted as WAV files (not necessary for FMV playback)

  The XA streams (interleaved audio/video) in the file table are:

  | Index | File ID | Description |
  |-------|---------|-------------|
  | 2044 | XA/05_02152 | XA stream |
  | 2045 | XA/10_04432 | XA stream |
  | 2046 | XA/15_07496 | XA stream |
  | 2047 | XA/20_06552 | XA stream |
  | 2048 | XA/25_03904 | XA stream |
  | 2049 | XA/30_04056 | XA stream |
  | 2050 | XA/35_26008 | XA stream |
  | 2051 | XA/40_10384 | XA stream |
  | 2052 | XA/45_28784 | XA stream |
  | 2053 | XA/C1_20670 | Intro cinematic (part 1) |
  | 2054 | XA/C2_20670 | Intro cinematic (part 2) |
  | 2055 | XA/M1_03500 | In-game movie 1 |
  | 2056 | XA/M2_01190 | In-game movie 2 |
  | 2057 | XA/M3_02570 | In-game movie 3 |
  | 2058 | XA/M4_02490 | In-game movie 4 |
  | 2059 | XA/M5_03140 | In-game movie 5 |
  | 2060 | XA/M6_02112 | In-game movie 6 |
  | 2061 | XA/M7_01536 | In-game movie 7 |
  | 2062 | XA/M8_03039 | In-game movie 8 |
  | 2063 | XA/M9_01730 | In-game movie 9 |
  | 2064 | XA/MA_03590 | In-game movie A |
  | 2065 | XA/MB_04850 | In-game movie B |
  | 2066 | XA/MC_01930 | In-game movie C |
  | 2067 | XA/MD_03780 | In-game movie D |
  | 2068 | XA/ME_03300 | In-game movie E |
  | 2069 | XA/Z1_16180 | Ending movie 1 |
  | 2070 | XA/Z3_02340 | Ending movie 3 |
  | 2071 | XA/Z4_01590 | Ending movie 4 |
  | 2072 | XA/ZC_14392 | Ending cinematic |
  | 2073 | XA/ZZ_14239 | Final ending |

  The numbers after the underscore (e.g. `03500` in `M1_03500`) represent the sector count/size of the stream on disc.

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

  Press `*` (numpad) to toggle debug camera mode. 
  
  | Key | Action |
  |-----|--------|
  | Numpad 8 / 5 | Move forward / back |
  | Numpad 4 / 6 | Strafe left / right |
  | Numpad 7 / 9 | Turn left / right |
  | Numpad + / - | Move up / down |
  | Numpad / | Print current coordinates |
  | Numpad . | Toggle fog |
  | Numpad 0 | Toggle FPS cap |
  | Numpad 1 | Toggle wall collision |
  | Numpad 2 | Toggle third-person follow camera |
  | Numpad 3 | Teleport Harry to spawn point |

  ### Known Limitations

  - Memory card save/load stubbed
  - 11 of 42 maps cannot compile as DLLs (non-constant initializers / cross-map shared data)
  - Only tested on Windows with MSYS2/MinGW64


  ### Support
  Message me wherever and I should answer. I am in most of the discords related to these projects as KushAstronaut.
  
  I work with more than just AI. If you like what I do:\
  [![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/F1F3K8V3B)


<br/><br/>


