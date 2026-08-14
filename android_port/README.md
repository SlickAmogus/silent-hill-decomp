# Silent Hill — Android port

ARM64 Android build, targeting a softmodded Arcade1Up cabinet (MediaTek MT8163
board, `tb8163p3_64_bsp`) and modern Android phones.

## Why this shares `pc_port/` instead of forking it

`xbox_port/` had to hand-write an entire HAL — `gpu_nv2a.c`, `audio_xbox.c`,
`cd_xbox.c`, `fs_xbox.c` — because nxdk gives you a fixed-function NV2A and no
SDL. Android needs none of that:

- `PsyCross/include/PsyX/PsyX_render.h` already selects `RENDERER_OGLES` with
  `OGLES_VERSION 3` on `__ANDROID__`, and `PsyX_render.cpp` already carries a
  `#version 300 es` shader header next to the desktop `#version 140` one.
- `PsyCross/src/platform.h` already routes `eprintf` to `__android_log_print`.
- The Linux work (PR #25) left the tree POSIX-clean, ELF-clean and LP64-clean.
  `arm64-v8a` is LP64 exactly like x86-64 Linux.

So `android_port/` holds **only** the APK shell — Gradle project, manifest,
`SDLActivity` subclass, CMake shim. Every code change lives in `pc_port/` behind
an `if(ANDROID)` / `#ifdef __ANDROID__` guard, so Android keeps riding along
with pc-port development instead of drifting into a fork.

## Toolchain

Installed to `C:\Android\` (nothing registered system-wide, no elevation used):

| Component | Version | Path |
|---|---|---|
| JDK | Temurin 17.0.20 | `C:\Android\jdk-17` |
| Gradle | 8.9 | `C:\Android\gradle-8.9` |
| Android SDK | platform 34, build-tools 34.0.0 | `C:\Android\sdk` |
| NDK | 27.3.13750724 | `C:\Android\sdk\ndk\...` |
| CMake | 3.22.1 (SDK-bundled) | `C:\Android\sdk\cmake\...` |
| SDL2 | 2.32.10 (submodule) | `android_port/app/jni/SDL` |

JDK 17 specifically: AGP 8.x will not run on the machine's default JDK 24.

## Build

```sh
./android_port/build_android.sh              # assembleDebug
./android_port/build_android.sh assembleRelease
adb install -r android_port/app/build/outputs/apk/debug/app-debug.apk
```

## Where the game data goes

The process starts with its working directory at `/`, so `main()` `chdir()`s to
the app's external files dir before anything loads (see the `__ANDROID__` block
at the top of `pc_port/src/main_pc.c`). Everything relative — `config.cfg`,
`gamedata/`, the disc image, `SilentHill.log` — resolves under:

```
/sdcard/Android/data/com.silenthill.port/files/
```

That path needs **no storage permission at any API level** and is not subject to
scoped storage, which is why it was chosen over `/sdcard/SilentHill/`.

### What ships vs what you supply

**The APK contains no game content and is not meant to.** It carries the native
libraries, the dex, resources, and twelve redistributable asset files
(`decal.png`, the OFL-licensed Barlow/Oswald fonts, the Polish language pack, RA
symbol tables, port UI sounds). `SilentHillActivity` unpacks those to the data
directory at launch, skipping any that already exist so local edits survive.

You supply one file: your own disc dump. Nothing else — everything the game
loads (maps, models, audio, FMV) is read out of the image, and the loose files
it wants are the ones staged from the APK above.

```
adb install -r app/build/outputs/apk/debug/app-debug.apk

# Launch ONCE so the app creates its own directories, THEN push.
adb shell am start -n com.silenthill.port/.SilentHillActivity

adb push "Silent Hill (USA).bin" \
  /sdcard/Android/data/com.silenthill.port/files/gamedata/
```

**Do not `adb shell mkdir` those directories.** Android's FUSE layer synthesizes
ownership per package: a directory created over adb belongs to `shell`, and the
app is then denied access to its own data dir. The symptom is a window titled
"Silent Hill - no disc image found" plus `EACCES` staging the bundled assets.
Let the app create the tree and push files into it — `shell` can write into an
app-owned directory through the `ext_data_rw` group, just not the reverse.

Push to the *directory*, not to a full destination path: the stock filenames
contain parentheses, and a path spelled out in an `adb shell` argument gets
re-parsed by the device's shell.

The filename matters. `PcPort_GetGameDiscPath()` (`main_pc.c`) auto-detects
against a known-names list — `Silent Hill (USA).bin`, `Silent Hill (PAL).bin`,
`Silent Hill (Europe) (En,Fr,De,Es,It).bin`, `Silent Hill (Japan).bin` — or set
`disc_image` in `config.cfg` for anything else. `fs_pc.c` resolves `./gamedata`
against the same directory, and `xa_player_software.c` reads BIN sectors
directly for XA/CD audio, so the image itself has to be there.

## What is disabled on Android, and why

| Feature | State | Reason |
|---|---|---|
| OpenAL (legacy SPU + XA) | compiled out | No OpenAL on Android. `PsyX_SPUSoftware` + `xa_player_software.c` render the SPU in software and push through SDL2's AAudio/OpenSL sink. `SH_NO_OPENAL` / `PSYX_NO_OPENAL` collapse both dispatchers onto the software path. |
| Map overlay `.so`s | off — replaced by `SH_STATIC_MAPS` (auto-on for Android) | Android `dlopen()`s only from the APK's own extracted lib dir, and the loader builds a relative `maps/<name>.so` path. See the static-maps section below. |
| MJPG AVI FMV overrides | compiled out (`SH_FMV_MJPEG` unset) | No system libjpeg. The disc's own STR/MDEC FMVs are unaffected — `IdentifyCodec` reports MJPG unsupported, which already falls back to the disc STR. |
| ffmpeg FMV fallback | off | Runtime-`dlopen`ed shared libs, not available here. |
| RetroAchievements | off | Needs libcurl. |
| Launcher | n/a | C#/.NET. |

## All 43 maps are in the binary (`SH_STATIC_MAPS`)

Android auto-enables `SH_STATIC_MAPS`, the same path iOS uses. Plain
`SH_BUILD_MAP_DLLS=OFF` would leave **only `map0_s00`** compiled in — linking
every overlay naively collides on 500+ symbols, because each one `#include`s its
own copy of the shared AI/particle/player code (PSX overlay semantics). Android
cannot take the `dlopen` route either: it loads only from the APK's own
extracted lib dir, and the loader builds a relative `maps/<name>.so` path.

`SH_STATIC_MAPS` renames each overlay's *defined* symbols to `<map>_<sym>` with
`llvm-objcopy --redefine-syms`, archives them, force-loads all 43, and resolves
headers through a generated registry instead of `dlsym`. `SH_SYM_PREFIX` is
empty on ELF (Mach-O needs the leading underscore). The NDK ships `llvm-nm` and
`llvm-objcopy` but does not put them on `PATH`, so the build hints at
`ANDROID_TOOLCHAIN_ROOT/bin`.

**Memory is the real cost, and it matters more here than on a phone.** Every
overlay reserves its own `.bss` (chara/particle/anim working buffers) and all 43
coexist permanently rather than one at a time. Measured on this build:

| section | size |
|---|---|
| `.bss` | **194.65 MB** |
| `.text` | 3.47 MB |
| `.rodata` | 0.67 MB |
| `.data` | 0.80 MB |

So code size is a non-issue; the reservation is not. It is demand-zero, so
nothing is paid until a map is actually visited and Android's low-memory killer
scores on RSS rather than VSZ — but nothing releases it afterwards either, so
RSS climbs monotonically across a long session. On a 1 GB MT8163 cabinet that is
the thing most likely to get the process killed mid-run. Worth measuring RSS on
the cabinet after a few map transitions before assuming it is fine.

## Affine texture warping is gone on GLES (fixed, with a cost)

`PsyX_render.cpp` used to emit `noperspective` for the `g_cfg_affineTextures`
path. **`noperspective` does not exist in GLSL ES** — it has only `smooth`,
`flat` and `centroid` through 3.2 — so every shader failed to *compile at
runtime*: clean build, black game. The guard now covers both ES paths.

The cost is that UVs interpolate perspective-correct, so the PSX's affine
texture warping is absent and steeply angled surfaces look too clean. The
renderer logs a one-time warning so `affine_textures = 1` does not look like it
took effect. Restoring the warp means premultiplying the varying by `w` in the
vertex shader and undoing it in the fragment shader, which works on any GLES
device — left undone deliberately, because it is shader maths that has to be
checked against a rendered frame rather than reasoned about.

## Performance notes for the cabinet

The MT8163 is quad Cortex-A53 @ ~1.3 GHz with a Mali-T720 MP2 @ 520 MHz — a
2015 entry-level tablet part. Two project-specific things matter more here than
on desktop:

- Build unoptimized and it will crawl. `CMAKE_BUILD_TYPE=RelWithDebInfo` is
  pinned in `app/build.gradle` for this reason.
- Mali is a tile-based renderer, so framebuffer readback is disproportionately
  expensive. `PSYX_SKIP_FRAMEBUFFER_STORE` is already set by `pc_port` and
  should stay set.
- Prefer `spu_renderer = authentic` (44.1 kHz) over the 352.8 kHz modern path.
