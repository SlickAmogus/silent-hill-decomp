# Silent Hill AI-Assisted PC Port

  This project started out as an experimental PC port built on top of the PSX decompilation, and has blossomed into a fully functional PC port with many new features, QoL enhancements, built-in mod support, an active community, and fan translation\multilingual support. It was built using PsyCross (PsyQ SDK Compatibility Layer originally for REDRIVER2), which we [now have our own fork of](https://github.com/SlickAmogus/PsyCross). The code for this PC port was built with heavy AI-Assistance from Claude Opus 4.6, 4.7, and the newer Fable model. However, care has been taken to leave the original game code intact everywhere possible, and to work around it where needed for the sake of the PC port. <br/>

  Project Website: https://sh1pc.com/ <br/>
  Discord: https://discord.gg/JWuNzVsQbr

## Status

The port is playable start to finish — the full game can be completed, and none of the remaining glitches are progression-blocking. Where it stands today:

- **Main menu** — fully working and now mouse-clickable; logos, FMV intro, options, and save/load all display and respond.
- **World rendering** — textured environments, fog, snow/particles, buildings and props all render; an experimental "whole-map" draw mode can show the whole town at once (off by default).
- **Player movement** — runs the original PSX lower-body state machine by default: collision-based walk/run, sidestep, wall collision, floor height, and stairs.
- **Combat** — handgun, hunting rifle, and shotgun fire reliably; melee connects; break-free, death, and grab all work. Free-aim, high-FPS double-fire, and phantom swings are fixed. Chainsaw and Rock Drill still have issues.
- **Bosses** — Twinfeeler, Split Head, Floatstinger, the Cybil fight, and Incubus all run; a few late boss effects are still being polished.
- **Cutscenes** — play in sync at any frame rate after a timing overhaul; a few per-scene pacing details remain (cosmetic, no freezes).
- **Camera** — the PSX fixed-camera system is 1:1 with the original, plus a full alternate suite: third-person, over-the-shoulder, and first-person, with FOV and aim-zoom sliders and their own control scheme.
- **Audio** — SFX, BGM, and voices all work; XA streams cutscene voices straight from the disc image. Optional surround 5.1/7.1 with true 3D positional SFX, reverb, and ADSR envelopes.
- **Maps** — all 43 map sections build as runtime DLLs and load; room and door transitions work. Map editor currently being built.
- **Enemies / AI** — full enemy AI. A global monster pool lets any enemy spawn in any map (on by default), and the concurrent-enemy cap can be raised past the original six.
- **Saves** — memory-card save/load works; quick-save (F6) and quick-load (F8) are enabled.
- **Graphics** — high resolutions, 16:9 (Hor+), high refresh rates, and uncapped FPS. Rewritten optional PGXP perspective correction (off is byte-identical to the original look). Four flashlight modes (Classic / +Shadows / Modern / +Shadows) with per-pixel lighting and shadow mapping, plus tone-mapping and post-process options.
- **Regions** — PAL/EUR (five localized languages) and NTSC-J (Japanese, with kanji font and story text) are both supported and tested; fan-translation discs work via a disc picker in the launcher.
- **Modding** — a built-in Mod Manager handles texture packs, loose-file replacement, DuckStation-style packs, and per-palette overrides, plus tools to compose and rebuild character textures.
- **Updates** — the launcher can check for and install updates; nightly builds are published, with Linux/macOS being a WIP. They are included, but have many bugs. It's recommended to use the Windows version where possible.

Nightly builds: https://github.com/SlickAmogus/silent-hill-pc-nightly

## Known Issues / Bugs

The bigger items still being worked on (roughly by priority, no strict order):

- PGXP can still show occasional seams at character joints (much improved since the rewrite)
- Chainsaw and Rock Drill weapons are broken
- Enemy AI needs another pass — some enemies jerk through the floor during combat
- Monsters spawned outside their home map (global pool) play the wrong SFX or none
- Menu re-entry can corrupt some screens on specific paths

**Full tracked bug list:** https://github.com/SlickAmogus/silent-hill-decomp/issues/13

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

## Game Files

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
- **Character textures** are one index sheet drawn through several palettes, so a single PNG only recolors part of the model. Use **Reference…** to build one correct composite from a character's model + texture, edit it in any image editor, then **Rebuild…** to slice it back into the per-row PNGs the game loads. See [`pc_port/docs/Modding_And_Extraction_Guide.md`](pc_port/docs/Modding_And_Extraction_Guide.md) for the full workflow.

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
| `flmode <0-3>` · `shadows [0\|1]` · `fl`/`wl [color]` | Flashlight mode / shadows / flashlight & world-light tint |
| `fov` · `tpsfov` · `tpsaimzoom` · `camcollide` | First-person / third-person FOV and camera feel |
| `audioout <stereo\|quad\|51\|71\|hrtf>` · `xavol <0-100>` · `adsr [0\|1]` | Speaker layout / FMV volume / SPU envelopes |
| `fmv [name]` | Play an intro / ending movie (bare `fmv` lists them) |
| `quit` | Exit the game |

Many values are also **live-tuning knobs** — run one with no argument to read the current value: `vfov`, `hfov`, `vshift`, `fogstr`, `pgxpedge`, `pgxpnearz`, `shadowbias`, `postint`, `tmint`, and the inventory `inv*` offsets. `debug 2` documents them.

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

**Debug camera (numpad):**

| Key | Action |
|-----|--------|
| Num `*` | Free debug camera on / off |
| Num `8` / `5` / `4` / `6` | Fly forward / back / strafe left / right |
| Num `7` / `9` | Turn left / right |
| Num `+` / `-` | Tilt up / down |
| PgUp / PgDn | Move up / down |
| Num `/` | Print camera coordinates to the log |
| Num `3` | Reset cam nudge / in-game rescue teleport |
| Num `.` | Log Harry's position |

With the debug camera **off**, the same numpad keys nudge the normal game camera — a live camera-tuning aid. (Camera *styles* — third-person, OTS, first-person — are cycled with the Change Camera key, `F9` by default, not a debug key.)

## Support

I'm on Discord as **@KushAstronaut**, and the project has a Discord server: https://discord.gg/JWuNzVsQbr

<br/>

Silent Hill is © Konami and this does not contain any game assets. You must provide a legally obtained dump of Silent Hill for PSX to use.
