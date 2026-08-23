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
| ffmpeg / libjpeg | off, via `SH_DESKTOP_FEATURE_DEFAULT` |
| RetroAchievements | **on** — `ios_ra_http.m` (NSURLSession) replaces `pc_ra_http.c` |
| Lua / randomizer | off — `loslib.c` needs `system()`, which the SDK forbids |
| Touch controls | `pc_touch.c`, shared with Android and ungated; on by default here |
| Controller | paired MFi/Bluetooth works through PsyCross's `SDL_GameController` path |

## RetroAchievements

On by default, and the only port that signs in from inside the game. Everywhere
else the launcher authenticates and leaves a connect token in `config.cfg`; an
iPhone has no launcher, so **Options → System → Achievements** opens a native
sign-in sheet instead. Your password is exchanged once for a token and is never
written anywhere — the token is what lands in `config.cfg`, and selecting the
row again signs out and clears it.

Softcore only, as on every other port: the quick save/load, debug controls,
alternate cameras and gamemodes rule out any hardcore ruleset.

Two things are worth knowing about how this is built:

- `pc_ra_http.c` has exactly two backends, WinHTTP and libcurl, and iOS has
  neither — which is the only reason RA was ever in the desktop-only feature
  bucket. `ios_ra_http.m` implements the same two entry points over
  NSURLSession, which needs no third-party library and is what App Transport
  Security is configured against, so `retroachievements.org` TLS works with no
  bundled CA store. rcheevos itself is libc-only and always could have built.
- The address map is rebuilt by `dlsym`ing the decomp's globals out of the
  running image. Mach-O only resolves what the link left in the export table,
  so this needs `-Wl,-export_dynamic`; the Windows build gets the same from
  `--export-all-symbols`. Without it the map comes up empty and every
  achievement reads zero.

## Status

Runs on device. CI produces a real arm64 iOS binary (Mach-O, LC_BUILD_VERSION
platform 2, minos 13.0) with all 43 map overlays linked in, a compiled launch
storyboard and the assets bundled, packaged as an unsigned .ipa.

Touch controls came across from Android. `pc_touch.c` is plain SDL touch with no
platform conditionals — a floating left thumbstick, right-side drag to look, tap
to act — presented to the engine as an ordinary analog pad, so nothing below
libpad knows a finger is involved. It is enabled by default here for the same
reason as Android: the touchscreen *is* the controller. iOS adds a separate fire
button that appears while aiming, and a One Button Combat option that folds the
two back together.

Two iOS-specific traps are worth recording, because both cost a session:

- **Framebuffer 0 is not the screen.** SDL's UIKit backend renders into a
  framebuffer it builds around a `CAEAGLLayer`, and the driver picks its name.
  Binding 0 selects a framebuffer that does not exist, so every draw is
  discarded silently — the game runs, audio plays, the screen stays black. Both
  `GR_ScreenFBO()` and `GR_ScreenReadFBO()` fall back to `PSYX_DEFAULT_FBO`,
  captured once from `GL_FRAMEBUFFER_BINDING` in `GR_InitialiseGLExt`.
- **Points are not pixels.** `SDL_GetWindowSize` reports points and
  `SDL_GL_GetDrawableSize` reports pixels, three times apart under
  `ALLOW_HIGHDPI`. There are two separate pointer paths — the touch stick and
  the menu cursor — and fixing only one leaves the other visibly offset.
