# UWP build (Xbox One / Series Dev Mode, Windows 10+)

The PC port packaged as a Universal Windows app: x64, Direct3D 11 through
ANGLE, SDL2's WinRT backend for the window, input and audio device.

## What differs from the desktop build

| | Desktop | UWP |
|---|---|---|
| Compiler | MinGW GCC | Clang, MSVC ABI (`toolchain-uwp-clang.cmake`) |
| Renderer | OpenGL, ANGLE optional | ANGLE Direct3D 11 only |
| Map overlays | DLLs | linked in (`SH_STATIC_MAPS`) |
| Working folder | next to the exe | the app's `LocalState` |
| RetroAchievements, ffmpeg FMV, Discord, launcher | yes | no |

## Where the game looks for the disc image

1. `LocalState\gamedata\*.bin` inside the app's data folder. On a console that
   is reachable from the Device Portal file explorer under
   `LocalAppData\SilentHillDecomp.SilentHill_...\LocalState`.
2. Otherwise the first drive that has `SilentHill\gamedata\*.bin`,
   `Silent Hill\gamedata\*.bin` or `gamedata\*.bin`. A USB drive must be NTFS
   and the folder must grant **ALL APPLICATION PACKAGES** read access
   (Properties, Security, Edit, Add), as for any Dev Mode app.

Saves, `config.cfg` and logs always live in `LocalState`.

## Building

Needs LLVM 18+, CMake, Ninja, Python 3 with Pillow, the Visual Studio 2022 C++
toolset and a Windows 10 SDK. The Visual Studio UWP workload is not needed.

```
vcpkg install openal-soft libjpeg-turbo angle --triplet x64-uwp
uwp\build-sdl2-uwp.bat <SDL2 2.32 source> <scratch build dir> <vcpkg>\installed\x64-uwp
powershell uwp\build-uwp.ps1 -Deps <vcpkg>\installed\x64-uwp
```

SDL2 is built by hand because the vcpkg port uses the Visual Studio generator,
which does need the UWP workload. It has to be SDL2: SDL3 dropped WinRT.

`build-uwp.ps1` leaves `build-uwp\SilentHill_UWP_<version>_x64.appx`, signed
with a self-made certificate, and the loose layout in `build-uwp\appx`.

## Installing

**Xbox (Dev Mode):** Device Portal, My games & apps, Add. Pick the `.appx`; add
`Microsoft.VCLibs.x64.14.00.appx` as a dependency if the portal asks for it.
Set the app's type to **Game** so it gets a game's memory and CPU share.

**Windows:** turn on Developer Mode (Settings, System, For developers), then

```
Add-AppxPackage -Register build-uwp\appx\AppxManifest.xml
```
