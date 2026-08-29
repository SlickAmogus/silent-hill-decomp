# Silent Hill - Native PC Port

  **A native PC port built on the Silent Hill PSX decompilation.** What started as an experiment has become a fully playable port with a large feature set, built-in mod support, an active community, and fan translation/multilingual support. It uses PsyCross (a PsyQ SDK compatibility layer originally written for REDRIVER2) as its hardware abstraction layer, which we [now maintain our own fork of](https://github.com/SlickAmogus/PsyCross).  Care has been taken to leave the original game code intact wherever possible and to work around it where needed, so game logic and feel still match the PSX original. Most if not all enhancements are optional, and always will be.
  
  The game runs as a real Windows/Linux executable compiled from decompiled C source — it is **not an emulator**, and it is **not a static recompilation**: no part of the original binary is translated by a tool.  **NOTE: Linux and Mac support are a WIP. On Linux, it is recommended to use wine for the time being.**

  **Development is heavily AI-assisted** (Claude Opus 4.6, 4.7 and the newer Fable model), and we're open about that. It is not automated translation of the game: every change is a reviewed, hand-directed edit to real source, tested against the original behaviour.

  Project Website: https://sh1pc.com/ <br/>
  Discord: https://discord.gg/JWuNzVsQbr

## Licensing at a glance

**The PC port is GPL-3.0.** That covers `pc_port/` (engine, launcher, tooling, docs) and every `SH_PC_PORT` addition to the decompiled sources. Files carrying an `SPDX-License-Identifier: GPL-3.0-or-later` header are the ones being licensed here.

**The decompilation is not licensed by this project.** `src/`, `include/`, `configs/`, `asm/`, `lib/` and `rom/` are a decompilation of Konami's Silent Hill, and remain the work of the [silent-hill-decomp](https://github.com/shdecompilations/silent-hill-decomp) contributors. This project asserts no copyright over that material and grants no rights to it — the GitHub license badge above refers to the port only.

**No game data is included.** Silent Hill is © Konami. A legally obtained copy of the PlayStation disc is required to play; nothing from it ships here or in any release.

Full breakdown, including bundled third-party components: [COPYRIGHT.md](COPYRIGHT.md).

## Features

**The game is fully playable start to finish.** Every map, boss, cutscene, ending, and system works — you can complete the full game today, and the remaining known bugs are minor. Enhancements are **optional and always will be**: at default-faithful settings the port plays like the original PSX game (same logic, same feel, same look), and everything beyond that is opt-in.

- **Faithful core** — original decompiled game logic, PSX fixed-camera system 1:1, original movement/combat/AI behaviour; play it exactly like 1999, just natively.
- **Graphics** — any resolution, widescreen 16:9/21:9+, high refresh / uncapped FPS, optional rewritten PGXP perspective correction, MSAA, texture filtering, tone mapping and post-process looks (CRT, scanlines, film grain, and more).
- **Flashlight & lighting** — four flashlight modes (Classic / Modern, each with optional per-pixel shadow mapping up to 8192x8192).
- **Cameras** — classic fixed cameras plus a full alternate suite: third-person, over-the-shoulder with free-aim, and first-person, each with FOV/aim options and its own control scheme; mouse-look free camera for exploring.
- **Audio** — optional surround 5.1/7.1 with true 3D positional SFX and reverb; XA cutscene voices streamed straight from the disc image; FMV playback with HD AVI & MP4 override support.
- **Quality of life** — mouse-clickable menus, quick save/load, reload key, quick-heal, in-game **PC Options** menu and an **F10 quick-options overlay** (graphics / HUD / audio / cheats / debug, live while playing), minimap options, low-health screen glow (off by default), uncensored toggle.
- **Cheats & tools** — in-game console (warps, spawns, god/noclip, live tuning of nearly everything), cheat menu, play as any of 14 characters, spawn any monster in any map (global monster pool), raised enemy cap, collision visualizer, keyframe inspector.
- **Modding** — built-in Mod Manager (extract the disc, texture packs, loose-file replacement, DuckStation-style packs), character retextures, full character model replacement (ILM <-> OBJ / Blender), modern glTF inventory item models, VAB audio extraction/repacking, AVI FMV replacement. See [Game Files / Modding](#game-files--modding).
- **Integrations** — RetroAchievements and Discord Rich Presence.
- **Experimental** — a Lua-scripted Randomizer mode, and a TrenchBroom-based level-editing pipeline in development.
- **Languages & regions** — USA, PAL/EUR (English, French, German, Italian, Spanish) and NTSC-J (Japanese) discs all supported and auto-detected; **fan-translation discs work** via the launcher's disc picker; Russian and Polish translation packs; Chinese playable on NTSC-J; the launcher UI itself is localized into 10 languages.
- **Multiplatform** — the same codebase runs on far more than PC. Windows is the primary target; Linux and macOS builds exist (WIP); and ports are in development for **Xbox 360**, **PS3**, **PSP**, **iOS**, **Android**, and even an **N64 demake** (Expansion Pak required).

Nightly builds: https://github.com/SlickAmogus/silent-hill-pc-nightly

## Known Issues / Bugs

- Linux and macOS builds still have issues — Windows (or wine) is recommended where possible.
- Visual glitches or artifacts can appear on certain systems/hardware (try the `renderer=` config option: d3d11 / vulkan / warp).
- Some minor visual bugs and cosmetic issues remain.

Everything else that's known is tracked on the [issues page](https://github.com/SlickAmogus/silent-hill-decomp/issues).

<!-- Screenshots go here -->

## Short Instructions

- Extract the release anywhere on your PC.
- Put your game disc image in the `gamedata` folder. Any filename works — the launcher auto-detects it by region — but `Silent Hill (USA).bin` is a safe default.
- Run `SilentHillPC_Launcher` to configure, or run `SilentHillPC` to play. The build is unsigned, so Windows SmartScreen may block it the first time; run the game once, then the launcher.

## Controls

Default keyboard bindings (fixed-camera "Classic" scheme):

| PSX Button | Key | In-game |
|------------|-----|---------|
| Cross | C | Action / shoot |
| Circle | V | Flashlight |
| Triangle | Z | Map |
| Square | X | Run |
| L1 / R1 | A / D | Sidestep left / right |
| L2 / R2 | Right Shift / Left Shift | View / aim |
| D-Pad | Arrow keys | Move |
| Start | Enter | Pause |
| Select | Space | Inventory |

PC-only keys:

| Action | Key |
|--------|-----|
| Reload | R |
| Quick save / load | F6 / F8 |
| Change camera style | F9 |
| Quick options overlay | F10 |

**Quick options (F10)** is a live in-game overlay with four pages — Graphics, HUD & Audio, Cheats, and Debug — navigable with keyboard or mouse (left-click raises a value, right-click lowers it). The Cheats page includes **Free camera**: once enabled, fly with the mouse + `W/A/S/D`, `Space`/`C` for up/down, `Shift` fast, `Ctrl` slow.

**Rebinding:** open the launcher and click **Controls** to remap keys and controller buttons — click a bind box, then press the key (Esc or Backspace clears it). The alternate cameras (third-person / OTS / first-person) use a separate control scheme; toggle **"Alt. Cam Controls"** in that window to edit it. Everything is also stored as plain `key_*` lines in `config.cfg`. (Reload is fixed to `R`.)

## Building

### Prerequisites

- **MSYS2 / MinGW64** (Windows). Install MSYS2 from https://www.msys2.org/, then from an **MSYS2 MinGW x64** shell install the toolchain:
  ```
  pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2 mingw-w64-x86_64-openal mingw-w64-x86_64-libjpeg-turbo
  ```
- **PsyCross** — pulled in as a git **submodule** (the SH-specific fork). From the repo root:
  ```
  git submodule update --init --recursive
  ```
  This populates `pc_port/PsyCross`. Do **not** clone PsyCross separately — the build uses the submodule.
- **Game disc image** — a BIN dump of *Silent Hill* (USA) (SLUS-00707). Place it at:
  ```
  pc_port/build/gamedata/Silent Hill (USA).bin
  ```

### Build

**Easiest (Windows): one command.**
```
pc_port\build.bat
```
Double-click `build.bat` for a menu (incremental / clean rebuild / build+run / nuke), or pass a mode: `build.bat` (incremental), `build.bat rebuild`, `build.bat configure`, `build.bat run`, `build.bat nuke`. It auto-configures (Ninja + map DLLs) on the first run and rebuilds incrementally afterward. If MSYS2 isn't at `C:\msys64`, set `MSYS2_ROOT` first (e.g. `set MSYS2_ROOT=D:\msys64 && pc_port\build.bat`).

**Manual** — from an **MSYS2 MinGW64** shell (first time):
```bash
cd silent-hill-decomp/pc_port
cmake -S . -B build -G Ninja -DSH_BUILD_MAP_DLLS=ON
cmake --build build
```
Subsequent incremental builds: `cmake --build build`. `-DSH_BUILD_MAP_DLLS=ON` builds the maps as DLLs loaded at runtime; without it only the starting area is available.

> **Note:** close the game before rebuilding — the linker cannot overwrite a running `SilentHillPC.exe` (`cannot open output file SilentHillPC.exe: Permission denied`). `build.bat` detects this and tells you.

## Game Files / Modding

The game's assets are packed into two archives (`SILENT.` and `HILL.`) on the disc — there is no real filesystem. They're indexed by a 2074-entry table with 6-bit-encoded names; the full enum with paths is in [`include/main/fileenum.h.USA.inc`](include/main/fileenum.h.USA.inc).

| Ext | Type ID | Description |
|-----|---------|-------------|
| .TIM | 0 | Texture image |
| .VAB | 1 | Sound bank |
| .BIN | 2 | Overlay code / data |
| .DMS | 3 | Cutscene data |
| .ANM | 4 | Animation data |
| .PLM | 5 | Map geometry |
| .IPD | 6 | Map data |
| .ILM | 7 | Character model |
| .TMD | 8 | Mesh data |
| .DAT | 9 | Demo data |
| .KDT | 10 | Audio metadata (sequence / score) |
| .CMP | 11 | Compressed data |

**Extracting and replacing files — launcher → Mod Manager:**

- **Extract BIN…** unpacks a disc `.bin` into a loose asset tree (`<FOLDER>/<NAME>.<TYPE>`), the same result as the dev extract with no extra tools. Tick "Convert textures to PNG" to also dump every `.TIM` as a `.png`.
- **TIM → PNG… / Bulk → PNG…** convert one texture or a whole folder to editable PNGs.
- **Loose-file overrides** — enable loose files (the checkbox on the window) and drop replacements in `gamedata/load/<FOLDER>/<NAME>` (e.g. `gamedata/load/SND/MAP000.VAB`); the game swaps them in at load. Replacements must be ≤ the original's size (oversized textures excepted).
- **Character textures** are one index sheet drawn through several palettes, so a single PNG only recolors part of the model. Use **Reference…** to build one correct composite from a character's model + texture, edit it in any image editor, then **Rebuild…** to slice it back into the per-row PNGs the game loads.
- **Texture packs** — loose PNG overrides, `.dds` (BC7) support, per-palette/per-CLUT-row overrides, and DuckStation-style replacement packs, all managed from the Mod Manager.

**Beyond textures:**

- **Character models (ILM)** — export any character to OBJ, edit or fully re-rig in Blender, and import back; the toolchain supports complete model replacement (a high-poly pipeline with automatic seam welding). See [`Model_Modding_Guide.md`](pc_port/docs/Model_Modding_Guide.md).
- **Inventory item models (glTF/GLB)** — drop modern glTF models in to replace the rotating inventory/examine items, keyed by item ID. See [`Modern_Item_GLTF_Modding_Guide.md`](pc_port/docs/Modern_Item_GLTF_Modding_Guide.md).
- **Audio (VAB)** — extract every SFX/instrument sample out of the game's VAB sound banks (vgmstream workflow documented in the guide), modify, and repack; loose-file VAB overrides load without touching the disc.
- **FMVs** — drop `.avi` files in as movie overrides; no disc rebuild needed.
- **Disc rebuild** — for changes that outgrow the loose-file size limits, the documented two-stage extract/rebuild produces a modified disc image with **no size ceiling**.

Full workflows: [`Modding_And_Extraction_Guide.md`](pc_port/docs/Modding_And_Extraction_Guide.md).

## Console Commands

An in-game console for cheats, warps, and live tuning. Enable it with `allow_debug_controls = 1` in `config.cfg`, then hold **`` ` ``** (backtick / tilde) in-game to open it (the game pauses while it's open). Type a command and press **Enter** — commands are case-insensitive. Type **`help`** or **`debug`** to list everything live.

| Command | What it does |
|---------|--------------|
| `give <item>` | Give a weapon / ammo / health — `give allweapons`, `give ammo`, `give health` (`help give` lists items) |
| `kill` / `killall` | Kill Harry / kill nearby enemies |
| `spawn <name> [state]` | Spawn a monster in front of Harry (`spawn list`) |
| `unlimited [0\|1]` | Raise the concurrent-enemy cap |
| `map <name>` | Set the **New-Game start map** — loads on the next New Game, does not warp mid-session |
| `noclip` / `obst` / `god` | Walk through walls / round-obstacle collision / invincibility |
| `getflags` · `setflag <n> <0\|1>` · `setending <bad\|bad+\|good\|good+>` | Inspect / force story flags and the ending (set before the ending) |
| `pgxp [0\|1]` | Toggle PGXP perspective correction |
| `cull` | Toggle world visibility culling (draw everything) |
| `shadowres <n>` | Flashlight shadow-map resolution (up to 8192) |
| `flmode <0-3>` · `shadows [0\|1]` · `fl`/`wl [color]` | Flashlight mode / shadows / flashlight & world-light tint |
| `fov` · `tpsfov` · `tpsaimzoom` · `camcollide` | First-person / third-person FOV and camera feel |
| `audioout <stereo\|quad\|51\|71\|hrtf>` · `xavol <0-100>` · `adsr [0\|1]` | Speaker layout / FMV volume / SPU envelopes |
| `fmv [name]` | Play an intro / ending movie (bare `fmv` lists them) |
| `quit` | Exit the game |

Many values are also **live-tuning knobs** — run one with no argument to read the current value: `vfov`, `hfov`, `vshift`, `cutshift`, `vcropanchor`, `par`, `drawdist`, `fogstr`, `pgxpedge`, `pgxpnearz`, `shadowbias`, `postint`, `tmint`, and the inventory `inv*` offsets. `debug 2` documents them.

## Debug Controls

Also requires `allow_debug_controls = 1`. Debug events echo to the per-run **`SilentHill_<timestamp>.log`**.

**Cheats & tools (top-row keys):**

| Key | Action |
|-----|--------|
| `Esc` | Warm reset to the title screen |
| `0` | Noclip toggle |
| `4` / `5` | New-Game start-map prev / next |
| `6` | Kill nearby enemies |
| `7` | Invincibility toggle |
| `8` | +15 handgun bullets |
| `9` | No-target toggle (enemies ignore Harry) |
| `-` / `=` | Give Hunting Rifle / Shotgun (+ shells) |
| `'` | Collision visualizer panel |
| `K` · `,` `.` | Keyframe inspector / scrub |

**Graphics hotkeys** (no dev controls needed):

| Key | Action |
|-----|--------|
| `F1` | Toggle PGXP |
| `F2` | Cycle post-process effect |
| `F3` | Cycle tone mapping |
| `F4` | Cycle flashlight mode |
| `[` / `]` | Lower / raise the selected effect's intensity |
| `\` | Choose which effect `[` / `]` adjusts |

**Free camera** (also toggleable from the F10 quick-options Cheats page, no dev controls needed there):

| Key | Action |
|-----|--------|
| Num `*` | Free camera on / off (Harry stays put; his input is ignored) |
| Mouse | Look (follows your mouse sensitivity / invert settings) |
| `W/A/S/D` | Fly forward / left / back / right along the view |
| `Space` / `C` | Move up / down |
| `Shift` / `Ctrl` | Fast / slow |
| Num `3` | Reset cam nudge / in-game rescue teleport |
| Num `.` | Log Harry's position |

With the free camera **off**, the numpad keys nudge the normal game camera — a live camera-tuning aid. (Camera *styles* — third-person, OTS, first-person — are cycled with the Change Camera key, `F9` by default; the **F10 quick-options overlay** exposes most of the graphics/debug toggles below without memorizing keys.)

## Support

I'm on Discord as **@KushAstronaut**, and the project has a Discord server: https://discord.gg/JWuNzVsQbr

<br/>

Silent Hill is © Konami and this does not contain any game assets. You must provide a legally obtained dump of Silent Hill for PSX to use.
