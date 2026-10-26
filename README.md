# Silent Hill - Native iOS Port

  **A native iOS port built on the Silent Hill PSX decompilation.** It shares the whole engine with the [PC port](https://github.com/SlickAmogus/silent-hill-decomp/tree/pc-port) and the [Android port](https://github.com/SlickAmogus/silent-hill-decomp/tree/android-port): the same decompiled C source, compiled for arm64 and running on [PsyCross](https://github.com/SlickAmogus/PsyCross) (our fork of the PsyQ compatibility layer) through SDL2 and OpenGL ES 3.0. It is **not an emulator** and **not a static recompilation**. Game logic and feel match the PSX original, and every enhancement is optional.

  **Development is heavily AI-assisted** (Claude Opus 4.6, 4.7 and the newer Fable model), and we're open about that. Every change is a reviewed, hand-directed edit to real source, tested against the original behaviour.

  Project Website: https://sh1pc.com/ <br/>
  Discord: https://discord.gg/JWuNzVsQbr

  Play on the web: https://sh1pc.com/play/

## Latest Features

- **See the whole town.** Turn the fog/visibility slider in the quick menu below 1.0 and the outdoor maps render all the way out instead of fading into fog. It needs PGXP, and the game offers to turn that on for you.
- **High-poly character swaps.** Replace Harry or Cybil with a skinned glTF model: full skeletal animation, lip sync driven by the voice lines, blinking, shape keys, optional ragdoll deaths, and per-model tuning in an ini. Extra `HERO_*` models show up as playable characters in Play As.
- **Realtime updates.** Every change is built automatically for iOS, Android, Windows, Linux and macOS as soon as it lands. The newest `.ipa` is always on the [Realtime builds](https://github.com/SlickAmogus/silent-hill-decomp/releases/tag/realtime) page, with the last 15 kept on the history release.
- **Native menus.** A new Controls screen and an in-game Achievements screen, with button prompts that match the controller you are using.
- **Natural diagonal movement.** In the alternate cameras, Harry's legs now turn to follow the direction he is walking or running.
- **Quick weapon switching.** Optional, including the mouse wheel.
- **Play in your browser.** Try it without installing anything at https://sh1pc.com/play/.

## Features

- **The full game.** Every map, boss, cutscene and ending. All 43 map overlays are linked into the app.
- **Touch controls.** A floating movement stick, drag-to-look, tap to act, and on-screen Aim / Item / Map / Start buttons, with a separate Fire button while aiming (**One Button Combat** merges them). Menus work by tapping. The third-person, over-the-shoulder and first-person cameras also work on touch, and an on-screen eye button cycles between them. The buttons send whatever your controller config has bound, so rebinds carry over.
- **Controllers.** Paired Bluetooth pads work, and the touch overlay steps aside when one is in use. Remap them under **Options > Controller Config**. With more than one connected, **Options > Controller** or the quick menu's Controls page picks the one that drives the game; it is saved, and while it is disconnected every controller works as before.
- **Graphics.** Renders at the device's native resolution in widescreen (Hor+) by default, at 60 fps. PGXP perspective correction and the rest of the PC port's graphics options are available too.
- **Quick options overlay.** Graphics, HUD, audio and cheats, opened from an on-screen button.
- **RetroAchievements.** Sign in from **Options > System > Achievements** with a native sign-in sheet (softcore only). Your password is exchanged once for a token and is never stored.
- **Mods.** Loose-file replacements and DuckStation-style texture packs. See [Modding](#modding).
- **All regions.** USA, PAL and NTSC-J discs are auto-detected.

## Known Issues / Bugs

- **No FMV overrides.** The iOS build has no libjpeg or ffmpeg, so HD AVI/MP4 movie replacements are ignored. The disc's own cutscenes still play.
- **No surround audio.** Audio is stereo, rendered in software through SDL, because Apple removed OpenAL.
- **No Randomizer.** Its Lua scripting layer needs `system()`, which the iOS SDK forbids.
- **Sideload only.** The app is unsigned. With a free Apple ID it has to be re-signed every 7 days.

Everything else that's known is on the [issues page](https://github.com/SlickAmogus/silent-hill-decomp/issues).

## Short Instructions

1. Download `SilentHill-unsigned.ipa` from the latest [Build iOS Port](https://github.com/SlickAmogus/silent-hill-decomp/actions/workflows/build-ios.yml) run (iOS 13+).
2. Sideload it with **Sideloadly** or **AltStore**. On Windows, install iTunes and iCloud **from apple.com, not the Microsoft Store**; the Store versions break both tools. Leave auto-refresh running, since a free Apple ID certificate lasts only 7 days.
3. Launch the app once. Then use the **Files** app to put your disc image in `On My iPhone > Silent Hill > gamedata`. The top-level `Silent Hill` folder works too. Any filename works and no `.cue` is needed.

Saves, `config.cfg`, mods and the `SilentHill_<timestamp>.log` files are all in that same **Files** folder. The bundle ID (`com.silenthill.port`) never changes, so re-signing keeps your disc image and saves.

## Controls

| Touch | Action |
|-------|--------|
| Drag, left side | Move (push to the edge to run) |
| Drag, right side | Look |
| Tap | Action: attack with a weapon ready, interact otherwise |
| On-screen buttons | Aim (hold), Fire, Item, Map, Start, Change Camera, and the quick options overlay |

In the alternate cameras, dragging on the right turns the camera directly, a double tap on the left toggles aim, and a tap on the right fires. A physical controller uses the standard PSX layout.

## Building

### How the iOS build works

`ios_port/` holds only the app shell: a CMake project that builds SDL2 and `pc_port/` into an app bundle, plus `Info.plist`, a launch storyboard and a few small Objective-C files. The game itself is `pc_port/`. iOS-specific code stays behind `SH_IOS` / `TARGET_OS_IPHONE`, and much of it is shared with Android, so the port keeps up with PC development instead of turning into a fork.

- **Renderer:** PsyCross's GLES 3.0 path (`RENDERER_OGLES`), using Apple's `OpenGLES/ES3` headers and an EAGL context through SDL. On iOS, framebuffer 0 is not the screen, so PsyCross records SDL's real framebuffer (`PSYX_DEFAULT_FBO`) at init.
- **Audio:** OpenAL is compiled out (`SH_NO_OPENAL`). The SPU and XA audio are rendered in software and played through SDL's CoreAudio output.
- **Maps:** iOS won't load a dylib from outside the signed bundle, so `SH_STATIC_MAPS` is forced on. Each overlay's symbols get a per-map prefix via `llvm-objcopy --redefine-syms` so all 43 can be linked together, and a generated registry replaces `dlsym`.
- **Disabled:** ffmpeg, the libjpeg MJPEG decoder, Lua and the launcher (it's C#/.NET). RetroAchievements stays on: `ios_ra_http.m` implements it over NSURLSession, and `-Wl,-export_dynamic` keeps the game's globals visible for the achievement address map.
- **Startup:** SDL owns the entry point (`UIApplicationMain`). `main()` `chdir()`s into `Documents`, and `ios_bootstrap.m` copies the port's own assets out of the read-only bundle without overwriting existing files: fonts, `decal.png`, language packs, UI sounds and a default `config.cfg`. `UIFileSharingEnabled` and `LSSupportsOpeningDocumentsInPlace` make `Documents` visible in Files.

### Prerequisites

- **macOS with Xcode.** Xcode is used only as a toolchain; there is no Xcode project. Only Apple's clang can target `arm64-apple-ios`, so this can't be built on Windows or Linux.
- **cmake**, **ninja** and **llvm** (for `llvm-nm` / `llvm-objcopy`), e.g. from Homebrew.
- The SDL2, PsyCross and rcheevos submodules:
  ```
  git submodule update --init --recursive ios_port/SDL pc_port/PsyCross pc_port/third_party/rcheevos
  ```

### Build

```sh
./ios_port/build_ios.sh
```

This configures, builds and packages an unsigned `.ipa`. You don't need a Mac for this: [`.github/workflows/build-ios.yml`](.github/workflows/build-ios.yml) runs the same build on a GitHub macOS runner for every push to `ios-port` and uploads the `.ipa` as an artifact. See [`ios_port/README.md`](ios_port/README.md) for more technical notes.

## Modding

Mods are **on by default** on iOS, because there is no launcher to turn them on. On first launch the game creates the mod folders and writes a quick-reference `gamedata/load/README.txt` in the app's Files folder. Restart the game after adding or removing files. **Options > Graphics > Load Mods** turns loose-file mods off.

- **Loose-file replacements:** `gamedata/load/<FOLDER>/<NAME>`, using the disc's own folder and file names (e.g. `gamedata/load/CHARA/HERO.TIM`). Case doesn't matter.
  - **Textures:** `.png` or `.dds` next to the name (`HERO.TIM.png` or `HERO.png`), or a replacement `.TIM`
  - **Models:** replacement `.TMD` / `.ILM` / `.IPD`, and `.glb` for inventory items
  - **Sounds:** `SND/<BANK>.VAB`, or a single sound as `SND/<BANK>.001.wav`
  - **Voices:** `XA/xa_0001.wav`, or `XA/msg_<KEY>.wav` for a text box
  - **Text:** `text_overrides.txt`, or `text_overrides/<name>.txt`
- **Texture packs:** DuckStation-format packs in `gamedata/texturemods/`, either as a folder or a `.zip`. Always on. Memory budgets for packs scale down to the device's RAM.
- **Not available:** FMV overrides (see Known Issues).

The mod *tools* (disc extraction, TIM ↔ PNG, character texture reference sheets, ILM ↔ OBJ model export) are in the PC launcher's Mod Manager. Build mods on a PC, then copy the resulting `gamedata/` files to the device with Files (or Finder file sharing). Full file formats and workflows: [`Modding_And_Extraction_Guide.md`](pc_port/docs/Modding_And_Extraction_Guide.md), [`Model_Modding_Guide.md`](pc_port/docs/Model_Modding_Guide.md), [`Modern_Item_GLTF_Modding_Guide.md`](pc_port/docs/Modern_Item_GLTF_Modding_Guide.md).

## Licensing at a glance

**The PC port is GPL-3.0.** That covers `pc_port/` (engine, launcher, tooling, docs) and every `SH_PC_PORT` addition to the decompiled sources. Files carrying an `SPDX-License-Identifier: GPL-3.0-or-later` header are the ones being licensed here.

**The decompilation is not licensed by this project.** `src/`, `include/`, `configs/`, `asm/`, `lib/` and `rom/` are a decompilation of Konami's Silent Hill, and remain the work of the [silent-hill-decomp](https://github.com/shdecompilations/silent-hill-decomp) contributors. This project asserts no copyright over that material and grants no rights to it. The GitHub license badge above refers to the port only.

**No game data is included.** Silent Hill is © Konami. A legally obtained copy of the PlayStation disc is required to play; nothing from it ships here or in any release.

Full breakdown, including bundled third-party components: [COPYRIGHT.md](COPYRIGHT.md).

## Support

I'm on Discord as **@KushAstronaut**, and the project has a Discord server: https://discord.gg/JWuNzVsQbr

This is a 100% free project funded from my own pocket. If you'd like to support me, it is greatly appreciated, and you can do so on Ko-fi:<br/> [![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/F1F3K8V3B)

Silent Hill is © Konami and this does not contain any game assets. You must provide a legally obtained dump of Silent Hill for PSX to use.
