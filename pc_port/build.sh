#!/bin/bash
# Silent Hill PC Port - Linux/macOS Build Script
#
# Prerequisites:
#   - CMake 3.16+
#   - GCC or Clang
#   - SDL2 development libraries (libsdl2-dev)
#   - OpenAL-soft development libraries (libopenal-dev)
#   - OpenGL development headers (libgl-dev)
#
# PsyCross must be cloned alongside the decomp repo:
#   silenthill/
#     silent-hill-decomp/
#     PsyCross/

echo "=== Silent Hill PC Port Build ==="
echo

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PSYCROSS_DIR="$SCRIPT_DIR/../../PsyCross"

# Create build directory
mkdir -p "$SCRIPT_DIR/build"
cd "$SCRIPT_DIR/build"

# Configure with CMake
cmake .. \
    -DCMAKE_BUILD_TYPE=Debug \
    -DPSYCROSS_DIR="$PSYCROSS_DIR" \
    -DSH_VERSION=USA \
    -DSH_DEBUG=ON

if [ $? -ne 0 ]; then
    echo
    echo "CMake configuration failed!"
    echo "Make sure SDL2 and OpenAL are installed:"
    echo "  Ubuntu/Debian: sudo apt install libsdl2-dev libopenal-dev"
    echo "  macOS: brew install sdl2 openal-soft"
    exit 1
fi

echo
echo "CMake configured. Building..."
echo

cmake --build . -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

if [ $? -eq 0 ]; then
    echo
    echo "Build successful! Binary at: build/SilentHillPC"
else
    echo
    echo "Build failed."
    exit 1
fi
