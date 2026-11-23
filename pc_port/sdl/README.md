# SDL runtime (Windows)

The game is built against the SDL2 API but runs on SDL3 through sdl2-compat.
These DLLs are copied beside `SilentHillPC.exe` after every build and ship in
every release, replacing MinGW's own `SDL2.dll`.

| File | What | Source |
|------|------|--------|
| `SDL2.dll` | sdl2-compat 2.32.74, forwards the SDL2 API to SDL3 | [sdl2-compat releases](https://github.com/libsdl-org/sdl2-compat/releases) |
| `SDL3.dll` | SDL 3.4.18 built with libusb | `tools/build_sdl3_windows.sh` |
| `libusb-1.0.dll` | libusb 1.0.30 (MSYS2), loaded by SDL3 at runtime | MSYS2 `mingw-w64-x86_64-libusb` |

SDL3 is built here rather than downloaded because the official build has no
libusb, and SDL3's Switch 2 controller driver needs it. That driver works over
USB only for now.

To go back to plain SDL2, configure with `-DSH_SDL3_RUNTIME=OFF`, or put
MinGW's `SDL2.dll` back beside the exe and delete `SDL3.dll`.

SDL3 and sdl2-compat are zlib licensed. libusb is LGPL-2.1 and is used as an
unmodified shared library; its source is at https://github.com/libusb/libusb.
