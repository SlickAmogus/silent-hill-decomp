#!/usr/bin/env bash
# Syntax-check the decompiled PSX sources with Clang.
#
# The Windows and Linux builds use GCC, and pc_port/build.sh deliberately picks
# Homebrew GCC on macOS, so nothing in normal CI ever puts this code through a
# Clang front end. iOS has no such choice: Homebrew GCC cannot target
# arm64-apple-ios, so the Apple toolchain is Clang or nothing. This is the gate
# that catches a Clang-only breakage before it costs a CI round trip.
#
# What it actually catches is GCC extensions Clang does not implement -- nested
# functions above all, which no flag can rescue and which the decomp used in
# four places. Warning-vs-error differences are suppressed here on purpose; they
# are all -Wno-able and the real builds already do so.
#
# Usage:
#   tools/check-clang.sh                          # host clang, host target
#   tools/check-clang.sh --target arm64-apple-ios # cross syntax-check for iOS
#   CLANG=/path/to/clang tools/check-clang.sh
#
# Only src/ (the decomp) is swept. pc_port/src is platform code with its own
# per-OS conditionals and is covered by the real builds.
# No `set -u`: macOS ships bash 3.2, where expanding an empty array under it is
# an "unbound variable" error, and TARGET_ARGS/SDK_ARGS are empty by default.
set -o pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLANG="${CLANG:-clang}"
VERSION="${SH_VERSION:-USA}"
TARGET_ARGS=()
SDK_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --target) TARGET_ARGS=(--target="$2"); shift 2 ;;
        --sysroot) SDK_ARGS=(-isysroot "$2"); shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

command -v "$CLANG" >/dev/null 2>&1 || { echo "ERROR: '$CLANG' not found." >&2; exit 1; }

cd "$REPO_ROOT"

INCLUDES=(
    -Ipc_port/include
    -Ipc_port/include/psyq_compat
    -Ipc_port/src
    -Iinclude
    -Iinclude/decomp
    -Ipc_port/PsyCross/include/psx
    -Ipc_port/PsyCross/include
)

# A handful of decomp TUs reach for SDL headers directly (SDL_timer.h,
# SDL_scancode.h). Only the include path is needed for a syntax check, never a
# library. Cross-compiling for iOS has no host SDL, so a miss is not fatal --
# those few files are simply skipped and reported.
SDL_CFLAGS=""
if command -v sdl2-config >/dev/null 2>&1; then
    SDL_CFLAGS="$(sdl2-config --cflags 2>/dev/null)"
elif command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl2 2>/dev/null; then
    SDL_CFLAGS="$(pkg-config --cflags sdl2 2>/dev/null)"
fi
for d in /mingw64/include/SDL2 /usr/include/SDL2 /usr/local/include/SDL2 \
         /opt/homebrew/include/SDL2 "$REPO_ROOT/pc_port/PsyCross/include/SDL2"; do
    [ -d "$d" ] && SDL_CFLAGS="$SDL_CFLAGS -I$d"
done
# Word-splitting is intended here: SDL_CFLAGS is a flag list.
# shellcheck disable=SC2206
SDL_ARGS=($SDL_CFLAGS)

# Mirrors pc_port/CMakeLists.txt: SKIP_ASM drops the MIPS asm paths, and the
# static_assert mapping is what the real build applies to every C source.
DEFINES=(
    -DSH_PC_PORT
    -DSKIP_ASM
    "-DVER_${VERSION}"
    -Dstatic_assert=_Static_assert
)

# The decomp is a work in progress: unfinalised signatures and implicit
# declarations are expected and are demoted in every real build too. They are
# not what this check is for.
SUPPRESS=(
    -Wno-implicit-function-declaration
    -Wno-int-conversion
    -Wno-incompatible-pointer-types
    -Wno-implicit-int
    -Wno-return-type
    -Wno-ignored-qualifiers
    -Wno-builtin-requires-header
    -Wno-incompatible-library-redeclaration
)

# Engine code, compiled unconditionally into the exe. src/maps is handled
# separately below: those need a per-map define, and the shared files under it
# (chara_util.c, characters/*.c) are #included by map sources rather than being
# translation units of their own, so sweeping them standalone is meaningless.
#
# main.c and memcpy.c are excluded from the build (MAIN_SOURCES filter);
# memcpy.c is raw MIPS asm and would never assemble for any PC target.
# Not `mapfile`: it does not exist in the bash 3.2 macOS still ships.
SOURCES=()
while IFS= read -r _f; do
    SOURCES+=("$_f")
done < <(find src -name '*.c' \
            | grep -v '^src/maps/' \
            | grep -vE 'src/main/(main|memcpy)\.c$' | sort)

# One representative overlay, matching MAP_SOURCES in pc_port/CMakeLists.txt.
MAP_DEFINES=(-DMAP0_S00 -DSH_MAP_NAME=map0_s00)
while IFS= read -r _f; do
    SOURCES+=("$_f")
done < <(find src/maps/map0_s00 -name '*.c' 2>/dev/null | sort)

echo "clang:   $("$CLANG" --version | head -1)"
echo "target:  ${TARGET_ARGS[*]:-<host>}"
echo "version: VER_${VERSION}"
echo "sources: ${#SOURCES[@]}"
echo

failed=0
log="$(mktemp)"
for f in "${SOURCES[@]}"; do
    case "$f" in
        src/maps/*) EXTRA=("${MAP_DEFINES[@]}") ;;
        *)          EXTRA=(-DSH_NOT_A_MAP_TU) ;;
    esac
    if ! "$CLANG" -fsyntax-only -std=gnu11 -ferror-limit=0 \
            "${TARGET_ARGS[@]}" "${SDK_ARGS[@]}" "${EXTRA[@]}" \
            "${SUPPRESS[@]}" "${DEFINES[@]}" "${INCLUDES[@]}" "${SDL_ARGS[@]}" \
            "$f" >>"$log" 2>&1; then
        failed=$((failed + 1))
        echo "FAIL $f"
    fi
done

nested=$(grep -c 'function definition is not allowed here' "$log" || true)

echo
if [ "$failed" -eq 0 ]; then
    echo "OK: all ${#SOURCES[@]} translation units pass Clang."
    rm -f "$log"
    exit 0
fi

echo "FAILED: $failed of ${#SOURCES[@]} translation units."
if [ "$nested" -gt 0 ]; then
    echo
    echo "$nested GCC nested-function definition(s) found. Clang has never supported"
    echo "these; hoist them to file scope and pass any captured parent locals in"
    echo "explicitly (see the un-nesting in events_main.c / npc_main.c)."
fi
echo
echo "--- first 60 diagnostic lines ---"
grep 'error:' "$log" | head -60
rm -f "$log"
exit 1
