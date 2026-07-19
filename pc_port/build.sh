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
#  Optional, only for the ffmpeg FMV fallback (cmake .. -DSH_FMV_FFMPEG=ON):
#        pacman -S mingw-w64-x86_64-ffmpeg     # dev/link only — see docs/fmv_files.md
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

# Array is always non-empty (the -S/-B/-G/-D flags below), so "${...[@]}"
# expansion is safe under `set -u` even on macOS' stock bash 3.2.
CONFIGURE_ARGS=(-S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja -DSH_BUILD_MAP_DLLS=ON)

# --- macOS: AppleClang rejects the PSX decomp C (nested funcs, implicit
# decls), so use Homebrew GCC for C; point CMake at Homebrew openal-soft
# instead of the system OpenAL framework. (No effect on Windows/Linux.) ------
if [ "$(uname -s)" = "Darwin" ]; then
    # Pick the highest-versioned Homebrew gcc-N. Use shell globbing (not `ls`,
    # whose non-zero exit on a non-matching path — e.g. /usr/local on arm64 —
    # would trip `set -euo pipefail` and abort silently before the check below).
    GCC_C=""
    for _c in /opt/homebrew/bin/gcc-[0-9]* /usr/local/bin/gcc-[0-9]*; do
        [ -x "$_c" ] || continue
        if [ -z "$GCC_C" ] || [ "$(printf '%s\n%s\n' "$GCC_C" "$_c" | sort -V | tail -1)" = "$_c" ]; then
            GCC_C="$_c"
        fi
    done
    if [ -z "$GCC_C" ]; then
        echo "ERROR: Homebrew GCC not found. Install it:  brew install gcc" >&2
        exit 1
    fi
    CONFIGURE_ARGS+=(-DCMAKE_C_COMPILER="$GCC_C")
    OPENAL_PREFIX="$(brew --prefix openal-soft 2>/dev/null || true)"
    if [ -n "$OPENAL_PREFIX" ]; then
        CONFIGURE_ARGS+=(
            -DOPENAL_LIBRARY="$OPENAL_PREFIX/lib/libopenal.dylib"
            -DOPENAL_INCLUDE_DIR="$OPENAL_PREFIX/include"
        )
    fi
    # jpeg-turbo (FMV MJPG decode) is keg-only, so CMake's find_library/
    # find_path won't see it without an explicit prefix.
    JPEG_PREFIX="$(brew --prefix jpeg-turbo 2>/dev/null || true)"
    if [ -n "$JPEG_PREFIX" ]; then
        CONFIGURE_ARGS+=(
            -DJPEG_LIBRARY="$JPEG_PREFIX/lib/libjpeg.dylib"
            -DJPEG_INCLUDE_DIR="$JPEG_PREFIX/include"
        )
    fi
fi

if [ "$need_configure" = 1 ]; then
    echo "=== Configuring (Ninja, map DLLs ON) ==="
    cmake "${CONFIGURE_ARGS[@]}"
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
