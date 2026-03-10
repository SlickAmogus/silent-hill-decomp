## About this fork

  This is an experimental PC port built on top of this decompilation using PsyCross as a PSX hardware abstraction layer (SDL2 + OpenGL + OpenAL), made with Claude also with help from REDRIVER2's source code.<br/>
  <br/>PsyCross: https://github.com/OpenDriver2/PsyCross<br/>
  REDRIVER2: https://github.com/OpenDriver2/REDRIVER2

  I am going to try to get the game in as playable of a state as possible but no guarantees. I'm hoping this will be of help to someone who can make a real pc port once it is fully decompiled.

  ### Status

  The game starts, loads the logos, and enters the main menu like normal. FMVs play if they are extracted (instructions below). You can change options or start a new game. The opening FMV plays. Once ingame, it skips 
  the ingame opening cutscene for debugging purposes. You can control Harry, but it crashes almost immediately when doing so (use arrow keys). You can bring up the inventory with space. The most interesting thing you 
  can do is use the debug camera. Press * on the number pad to toggle it. Controls below:

```
  - : Lower the camera
  + : Raise the camera
  7 : Turn camera left
  8 : Move camera forward
  9 : Turn camera right
  4 : Move camera left
  5 : Move camera back
  6 : Move camera right
  . : Toggle fog
```

  Culling is disabled in debug mode, so you can see the entire loaded map. Next step is to be able to load more maps, will update with that asap. There is supposed to be a console with ~ that allows you to change
  maps, but it is not fully implemented.

  <img width="638" height="505" alt="github1" src="https://github.com/user-attachments/assets/c1a490af-ceb0-46c1-9553-eeaed32db605" />
  <img width="631" height="498" alt="harry" src="https://github.com/user-attachments/assets/723b6874-5965-40b0-8db0-f6834c1a2d30" />

  ### Prerequisites

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

  From an **MSYS2 MinGW64** shell:

  ```bash
  cd silent-hill-decomp/pc_port
  mkdir build && cd build
  cmake .. -G Ninja
  cmake --build .
  ```

  Or from PowerShell/cmd:
  ```
  "C:\msys64\usr\bin\bash.exe" -lc "cd /c/path/to/silent-hill-decomp/pc_port/build && cmake --build ."
  ```

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
  3. Select the video streams and extract them (can convert to AVI or raw frames)
  4. XA audio streams can be extracted as WAV files

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

  ### Known Limitations

  - Only Harry's character model renders (no world geometry — IPD chunk loading not yet implemented)
  - Fog/environment lighting disabled during character rendering
  - FMV playback is stubbed (movies are skipped)
  - XA audio streaming not implemented (BGM does not play)
  - SFX audio works via PsyCross SPU emulation (OpenAL)
  - Memory card save/load is stubbed
  - NPC AI is disabled (animation info tables are stubs)
  - Menu/inventory screens may crash
  - Only tested on Windows with MSYS2/MinGW64

<br/><br/>

# Original Readme

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://github.com/Vatuu/silent-hill-decomp/blob/master/docs/SHDecompLogo.png?raw=true">
  <source media="(prefers-color-scheme: light)" srcset="https://github.com/Vatuu/silent-hill-decomp/blob/master/docs/SHDecompLogo-NTSC.png?raw=true">
  <img alt="SILENT HILL DECOMPILATION PROJECT" title="SILENT HILL DECOMPILATION PROJECT" src="https://github.com/Vatuu/silent-hill-decomp/blob/master/docs/SHDecompLogo-NTSC.png?raw=true">
</picture>
<div align="center">
<br/>
An in-progress decompilation of the 1.1 US release of <i>Silent Hill</i> on the PlayStation 1.
</div>

## Progress
Due to the limited memory on the PlayStation 1, games often distribute their logic and functionality across different binary overlays. *Silent Hill* follows this approach by separating core engine code, some screen-related code, and map stage logic code into many distinct binaries. The main executable (`SLUS_007.07` on the 1.1 NTSC release) serves primarily as a memory handler.

<details>
<summary>What does the decompilation percentage mean? <b><i>(click to expand)</i></b></summary>

The percentage tracks how much of the game's compiled code has been matched, meaning we've written C code that compiles to an output identical to the original assembly code.

Reaching 100% means every function in the game is accounted for, but that's only the end of the first phase, not the project as a whole.

There's still a lot of work ahead:

- **Deobfuscation & naming**: many functions and variables still have generated names like `func_80241A30`. We'll need to figure out the actual purposes of these and name them meaningfully.
- **Data migration**: raw binary data needs to be parsed into proper C structs so the data can be made understandable.
- **Shiftability**: making the build not rely on hardcoded memory addresses so that code and data can be modified without breaking everything.
- **Documentation**: understanding and documenting how the game's systems work together to aid in mods and future projects.

100% will be a milestone worth celebrating, but there's still plenty left to do!

</details>

<table align=center>
    <tbody>
        <tr>
            <th colspan=3>Total Progress</th>
        </tr>
        <tr>
            <td colspan=3 align=center><a href="https://decomp.dev/Vatuu/silent-hill-decomp"><img src="https://decomp.dev/Vatuu/silent-hill-decomp.svg?mode=shield&measure=matched_code_percent"/></a><br/><a href="https://decomp.dev/Vatuu/silent-hill-decomp"><img src="https://decomp.dev/Vatuu/silent-hill-decomp.svg?mode=shield&label=Silent+Hill+%28Fuzzy+Match%29&measure=fuzzy_match_percent"/></a></td>
        </tr>
        <tr>
            <th colspan=3>⚙ SLUS-00707 ⚙</th>
        </tr>
        <tr>
            <td>Progress</td>
            <td colspan=2>Purpose</td>
        </tr>
        <tr>
            <td align=center><a href="https://decomp.dev/Vatuu/silent-hill-decomp?category=main"><img src="https://decomp.dev/Vatuu/silent-hill-decomp.svg?mode=shield&category=main&measure=fuzzy_match_percent&color=rgb(255,215,0)"/></a></td>
            <td colspan=2>Main executable.</td>
        </tr>
        <tr>
            <th colspan=3>🧟‍♂️⚔⚙🎮 BODYPROG.BIN 🎮⚙⚔🧟‍♂️</th>
        </tr>
        <tr>
            <td>Progress</td>
            <td colspan=2>Purpose</td>
        </tr>
        <tr>
            <td align=center><a href="https://decomp.dev/Vatuu/silent-hill-decomp?category=engine"><img src="https://decomp.dev/Vatuu/silent-hill-decomp.svg?mode=shield&category=engine&measure=fuzzy_match_percent"/></a></td>
            <td colspan=2>Main game logic.</td>
        </tr>
    </tbody>
</table>

## Contributing
Contributions are welcome! Feel free to open a pull request. To help familiarize yourself with the setup and decompilation workflow for *Silent Hill*, refer to our [Wiki Page](/../../wiki/Home).

You can also reach out to us by opening an issue or joining the `#silent-hill` channel on the [PS1/PS2 Decompilation](https://discord.gg/VwCPdfbxgm) Discord server.
