#!/bin/bash
# Silent Hill PC Port - Linux/macOS Build Script
#
# Prerequisites:
#   - CMake 3.16+
#   - GCC or Clang
#   - SDL2 development libraries
#   - OpenAL-soft development libraries
#   - OpenGL development headers
#
# macOS (Apple Silicon / Intel) additionally:
#   brew install cmake gcc sdl2 openal-soft
#
# PsyCross is pulled in as a git submodule at pc_port/PsyCross. If the
# submodule isn't initialized, the script falls back to a sibling checkout:
#   silenthill/
#     silent-hill-decomp/
#     PsyCross/

set -e

echo "=== Silent Hill PC Port Build ==="
echo

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# PsyCross: prefer the git submodule, fall back to a sibling checkout.
if [ -f "$SCRIPT_DIR/PsyCross/CMakeLists.txt" ]; then
    PSYCROSS_DIR="$SCRIPT_DIR/PsyCross"
elif [ -f "$SCRIPT_DIR/../../PsyCross/CMakeLists.txt" ]; then
    PSYCROSS_DIR="$SCRIPT_DIR/../../PsyCross"
else
    echo "ERROR: PsyCross not found."
    echo "Initialize the submodule:  git submodule update --init pc_port/PsyCross"
    exit 1
fi

# Create build directory
mkdir -p "$SCRIPT_DIR/build"
cd "$SCRIPT_DIR/build"

CMAKE_ARGS=(
    -DCMAKE_BUILD_TYPE=Debug
    -DPSYCROSS_DIR="$PSYCROSS_DIR"
    -DSH_VERSION=USA
    -DSH_DEBUG=ON
)

if [ "$(uname -s)" = "Darwin" ]; then
    # The PSX decomp C sources use GCC nested functions and rely on implicit
    # declarations, which AppleClang rejects. Use Homebrew GCC for C; PsyCross
    # and fmv C++ stay on the default (AppleClang) C++ compiler.
    GCC_C="$(ls /opt/homebrew/bin/gcc-[0-9]* /usr/local/bin/gcc-[0-9]* 2>/dev/null | sort -V | tail -1)"
    if [ -z "$GCC_C" ]; then
        echo "ERROR: Homebrew GCC not found. Install it:  brew install gcc"
        exit 1
    fi
    CMAKE_ARGS+=(-DCMAKE_C_COMPILER="$GCC_C")

    # Point CMake at Homebrew's openal-soft instead of the system OpenAL
    # framework.
    OPENAL_PREFIX="$(brew --prefix openal-soft 2>/dev/null)"
    if [ -n "$OPENAL_PREFIX" ]; then
        CMAKE_ARGS+=(
            -DOPENAL_LIBRARY="$OPENAL_PREFIX/lib/libopenal.dylib"
            -DOPENAL_INCLUDE_DIR="$OPENAL_PREFIX/include"
            -DCMAKE_EXE_LINKER_FLAGS="-L$OPENAL_PREFIX/lib"
            -DCMAKE_SHARED_LINKER_FLAGS="-L$OPENAL_PREFIX/lib"
        )
    fi
fi

# Configure with CMake
cmake .. "${CMAKE_ARGS[@]}"

echo
echo "CMake configured. Building..."
echo

cmake --build . -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo
echo "Build successful! Binary at: build/SilentHillPC"
