#!/usr/bin/env bash
# ===========================================================================
#  Silent Hill PC Port - build script (Ninja + map DLLs)
#
#  Canonical toolchain: MSYS2 MinGW-w64 on Windows. Run this from an
#  "MSYS2 MinGW x64" shell. (On Linux it also works with gcc/clang + ninja +
#  system SDL2/OpenAL.)
#
#  Prerequisites (MSYS2):
#    pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
#        mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2 mingw-w64-x86_64-openal
#  And initialise the PsyCross submodule once (from the repo root):
#    git submodule update --init --recursive
#
#  Usage (run from anywhere):
#    ./build.sh            incremental build (configure first if needed)
#    ./build.sh rebuild    clean rebuild (cmake --build --clean-first)
#    ./build.sh configure  force a fresh cmake configure, then build
#    ./build.sh run        build, then launch the game
# ===========================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
# Executable name is platform-specific: SilentHillPC.exe on Windows/MSYS2,
# plain SilentHillPC on Linux/macOS.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) EXE="$BUILD_DIR/SilentHillPC.exe" ;;
    *)                    EXE="$BUILD_DIR/SilentHillPC" ;;
esac

MODE="${1:-build}"

# --- Tool + submodule checks ----------------------------------------------
for tool in cmake ninja; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ERROR: '$tool' not found on PATH." >&2
        echo "On MSYS2 run from the 'MSYS2 MinGW x64' shell and install:" >&2
        echo "  pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2 mingw-w64-x86_64-openal" >&2
        exit 1
    }
done
if [ ! -e "$SCRIPT_DIR/PsyCross/CMakeLists.txt" ]; then
    echo "ERROR: PsyCross submodule missing at $SCRIPT_DIR/PsyCross" >&2
    echo "From the repo root run:  git submodule update --init --recursive" >&2
    exit 1
fi

# --- The linker can't overwrite a running exe (Windows) -------------------
if command -v tasklist >/dev/null 2>&1; then
    if tasklist //FI "IMAGENAME eq SilentHillPC.exe" 2>/dev/null | grep -qi 'SilentHillPC.exe'; then
        echo "ERROR: SilentHillPC.exe is running - close the game before building." >&2
        exit 1
    fi
fi

# --- Configure when asked, or when there's no cache yet -------------------
need_configure=0
[ "$MODE" = "configure" ] && need_configure=1
[ -f "$BUILD_DIR/CMakeCache.txt" ] || need_configure=1
if [ "$MODE" = "configure" ]; then rm -f "$BUILD_DIR/CMakeCache.txt"; fi

if [ "$need_configure" = 1 ]; then
    echo "=== Configuring (Ninja, map DLLs ON) ==="
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja -DSH_BUILD_MAP_DLLS=ON
fi

# --- Build ----------------------------------------------------------------
echo "=== Building ==="
if [ "$MODE" = "rebuild" ]; then
    cmake --build "$BUILD_DIR" --clean-first
else
    cmake --build "$BUILD_DIR"
fi

[ -f "$EXE" ] || { echo "ERROR: $(basename "$EXE") was not produced." >&2; exit 1; }
echo "Build OK: $EXE"

if [ "$MODE" = "run" ]; then
    echo "Launching..."
    ( cd "$BUILD_DIR" && "$EXE" )
fi
