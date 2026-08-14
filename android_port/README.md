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
scoped storage, which is why it was chosen over `/sdcard/SilentHill/`. Push data
with `adb push <file> /sdcard/Android/data/com.silenthill.port/files/`.

## What is disabled on Android, and why

| Feature | State | Reason |
|---|---|---|
| OpenAL (legacy SPU + XA) | compiled out | No OpenAL on Android. `PsyX_SPUSoftware` + `xa_player_software.c` render the SPU in software and push through SDL2's AAudio/OpenSL sink. `SH_NO_OPENAL` / `PSYX_NO_OPENAL` collapse both dispatchers onto the software path. |
| Map overlay DLLs | off (already the default) | Android only `dlopen()`s from the APK's own lib dir. Maps link statically. |
| MJPG AVI FMV overrides | compiled out (`SH_FMV_MJPEG` unset) | No system libjpeg. The disc's own STR/MDEC FMVs are unaffected — `IdentifyCodec` reports MJPG unsupported, which already falls back to the disc STR. |
| ffmpeg FMV fallback | off | Runtime-`dlopen`ed shared libs, not available here. |
| RetroAchievements | off | Needs libcurl. |
| Launcher | n/a | C#/.NET. |

## Known outstanding — GLES3 shader gap

`PsyCross/src/render/PsyX_render.cpp` emits `noperspective` for the
`g_cfg_affineTextures` path. **`noperspective` does not exist in GLES3** — ES
has only `smooth`, `flat` and `centroid`. The existing `SH_TC_CENTROID` guard
covers ES2 but nothing guards the ES3 case, so affine texturing will fail to
compile the shader *at runtime*, not at build time. This is exactly the trap the
project's shader-gate rule warns about: a clean build does not mean valid GLSL.

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
