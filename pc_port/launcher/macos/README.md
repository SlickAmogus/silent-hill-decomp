# Silent Hill PC — macOS launcher

`Silent Hill PC.app` is a self-contained app for Apple Silicon Macs. It holds a
small SwiftUI launcher, the game, its map overlays and assets, plus every
library the game needs (SDL2/SDL3, OpenAL, libjpeg). Players don't need
Homebrew.

## Playing

1. Open `Silent Hill PC.dmg` and drag **Silent Hill PC** into Applications.
2. Open it. The build is ad-hoc signed, not notarized, so the first launch is
   blocked: open **System Settings → Privacy & Security** and click
   **Open Anyway**. You only do this once.
3. Click **Add Disc Image…**, or drop your `.bin` / `.cue` onto the window. The
   USA, PAL/European and Japanese (Rev 1/2) releases are supported.
4. Pick your settings and press **Play**.

The launcher keeps everything you own in
`~/Library/Application Support/SilentHillPC` (**Open Game Folder**):
`config.cfg`, disc images and saves in `gamedata/`, `SilentHill.log`, and
`console.log` (the game's terminal output, useful for bug reports).

Shipped files appear in that folder as symlinks into the app. To mod or replace
one, put a real file at the same path and the launcher will leave it alone.
Language packs dropped into `gamedata/lang` show up in the Language list.

## Building

```sh
cd pc_port
./build.sh app        # build the game, then package build/macos/Silent Hill PC.app + .dmg
```

Or, if the game is already built: `launcher/macos/make_app.sh [--dmg]`.
You need the Xcode command line tools and the Homebrew packages from `build.sh`.

## Notes

- `DiscProbe.swift` ports `SilentHillPC_Launcher/DiscProbe.cs`, which mirrors
  the game's `Pc_DetectRegionFromBin`. Keep all three in agreement.
- `ConfigFile.swift` follows the Windows `ConfigManager` contract. It only
  writes back keys the launcher changed, and re-reads the file before every
  save, so changes the game makes to `config.cfg` are kept.
