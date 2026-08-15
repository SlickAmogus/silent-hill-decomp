# Silent Hill — iOS port

The iOS build of the PC port. Branch: `ios-port`, branched from `android-port`
rather than `pc-port` so it inherits the mobile work already proven there.

There is no engine here. PsyCross is the base on iOS exactly as it is on PC and
Android: it already had an OpenGL ES 3.0 renderer, and SDL2 ships complete UIKit,
CoreAudio and MFi-controller backends. Xcode is needed as a *toolchain* (clang,
the iPhoneOS SDK, ibtool) but not as an IDE, and the CMake Xcode generator is
deliberately not used.

## Layout

```
ios_port/
  CMakeLists.txt          SDL2 + pc_port; owns the bundle metadata
  build_ios.sh            configure -> build -> unsigned .ipa
  SDL/                    submodule, pinned to the same commit android_port uses
  Resources/
    Info.plist.in         configured by CMake; the file-sharing keys are load-bearing
    LaunchScreen.storyboard   mandatory, or iOS letterboxes the app at 320x480
  src/
    ios_paths.m           Documents + bundle resource paths
    ios_bootstrap.m       stages the port's shipped assets out of the read-only bundle
```

## Build

Requires macOS with Xcode. This cannot be built on Windows or Linux: only
Apple's clang emits `arm64-apple-ios`, and the Homebrew GCC that
`pc_port/build.sh` selects for the desktop macOS build cannot target iOS.

```
git submodule update --init --recursive ios_port/SDL pc_port/PsyCross
./ios_port/build_ios.sh
```

In practice nobody runs that by hand: `.github/workflows/build-ios.yml` builds
it on a GitHub macOS runner (free and unmetered for a public repo) and uploads
`SilentHill-unsigned.ipa` as an artifact.

## Installing, from Windows, with no Mac

1. Download the `.ipa` artifact from the workflow run.
2. Install **iTunes and iCloud from apple.com, not the Microsoft Store**. The
   Store builds break both Sideloadly and AltServer, and this is the single most
   common setup failure.
3. Open Sideloadly, drag the `.ipa` in, sign in with an Apple ID.

A free Apple ID gives a 7-day certificate and a 3-app limit, so the app needs
re-signing weekly; leave Sideloadly's auto-refresh running. The $99 developer
account raises that to a year and is worth buying once the game renders, not
before.

The bundle identifier (`com.silenthill.port`, matching the Android
`applicationId`) must never change: re-signing preserves the app's container
only while it stays the same, and a ~700 MB disc image should not have to be
copied back across every week.

## What ships, and what you supply

The bundle carries only the port's own assets — `decal.png`, the fonts, the
language pack and the UI sounds, all port-authored or OFL-licensed. `ios_bootstrap.m`
stages them into Documents on first launch, never overwriting a file that is
already there, because the bundle itself is read-only and the game resolves
everything relative to the working directory.

**No game data ships.** You supply your own Silent Hill disc image. Once the app
is installed, put the BIN/CUE in Files.app under
`On My iPhone > Silent Hill > gamedata`. `UIFileSharingEnabled` and
`LSSupportsOpeningDocumentsInPlace` in `Info.plist` are what make that directory
visible; without them there is no way to get a file in at all.

## Reuse / replace

| Area | iOS |
|---|---|
| Renderer | PsyCross, `RENDERER_OGLES` v3 via `TARGET_OS_IPHONE` |
| GL headers | Apple's `OpenGLES/ES3`, not Khronos `GLES3/gl3.h` |
| Context | EAGL through SDL; iOS has no EGL |
| Audio | software SPU + SDL sink; Apple removed its OpenAL framework |
| Map overlays | `SH_STATIC_MAPS` — iOS will not load a dylib outside the signed bundle |
| Entry point | SDL owns it (`UIApplicationMain`), as on Android |
| Working dir | `Documents`, set by `chdir` in `main_pc.c` |
| ffmpeg / libjpeg / RetroAchievements | off, via `SH_DESKTOP_FEATURE_DEFAULT` |
| Touch controls | not written yet — a paired MFi/Bluetooth controller needs no new code |

## Status

Builds and links. CI produces a real arm64 iOS binary (Mach-O, LC_BUILD_VERSION
platform 2, minos 13.0) with all 43 map overlays linked in, a compiled launch
storyboard and the assets bundled, packaged as an unsigned .ipa. Nothing has run
on a device, so every rendering and performance question is still open. Touch
controls are unwritten; a paired controller flows through PsyCross's existing
SDL_GameController path with no new code.
