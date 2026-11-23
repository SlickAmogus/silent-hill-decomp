#!/bin/bash
# Rebuilds pc_port/sdl/SDL3.dll and refreshes libusb-1.0.dll beside it.
# Run from an MSYS2 MINGW64 shell:  bash tools/build_sdl3_windows.sh [workdir]
#
# Why a custom SDL3 at all: the official Windows SDL3.dll and MSYS2's package are
# both built without libusb, and SDL3 compiles its Switch 2 controller driver
# only under HAVE_LIBUSB (the controller needs a USB bulk-endpoint handshake
# before it sends input). libusb is loaded at runtime (SDL_LIBUSB_DYNAMIC), so a
# missing libusb-1.0.dll only loses those controllers, nothing else.
#
# sdl2-compat (pc_port/sdl/SDL2.dll) is taken as released, not rebuilt:
#   https://github.com/libsdl-org/sdl2-compat/releases
set -euo pipefail

SDL_VER=3.4.18
USB_PKG=mingw-w64-x86_64-libusb-1.0.30-1-any.pkg.tar.zst
REPO="$(cd "$(dirname "$0")/.." && pwd)"
W="${1:-$REPO/pc_port/build/sdl3_work}"
mkdir -p "$W/usb"
cd "$W"

[ -f "SDL3-$SDL_VER.tar.gz" ] || curl -fsSL -o "SDL3-$SDL_VER.tar.gz" \
    "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL3-$SDL_VER.tar.gz"
[ -d "SDL3-$SDL_VER" ] || tar xzf "SDL3-$SDL_VER.tar.gz"
[ -f "usb/$USB_PKG" ] || curl -fsSL -o "usb/$USB_PKG" "https://repo.msys2.org/mingw/mingw64/$USB_PKG"
tar --zstd -xf "usb/$USB_PKG" -C usb
USB="$W/usb/mingw64"

cmake -S "SDL3-$SDL_VER" -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_HIDAPI_LIBUSB=ON -DSDL_HIDAPI_LIBUSB_SHARED=ON \
    -DLibUSB_LIBRARY="$USB/lib/libusb-1.0.dll.a" \
    -DLibUSB_INCLUDE_PATH="$USB/include/libusb-1.0"
ninja -C build

grep -q '#define SDL_LIBUSB_DYNAMIC' build/include-config-release/build_config/SDL_build_config.h \
    || { echo "libusb was not enabled; the Switch 2 driver is missing" >&2; exit 1; }

cp build/SDL3.dll "$REPO/pc_port/sdl/SDL3.dll"
strip -s "$REPO/pc_port/sdl/SDL3.dll"
cp "$USB/bin/libusb-1.0.dll" "$REPO/pc_port/sdl/libusb-1.0.dll"
cp "SDL3-$SDL_VER/LICENSE.txt" "$REPO/pc_port/sdl/LICENSE-SDL3.txt"
cp "$USB/share/licenses/libusb/COPYING" "$REPO/pc_port/sdl/LICENSE-libusb.txt"
echo "pc_port/sdl updated: SDL3 $SDL_VER with libusb"
