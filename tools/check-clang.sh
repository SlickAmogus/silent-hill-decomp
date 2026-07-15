#!/usr/bin/env bash
# Local strict-compiler check. Compiles the port's C++ TUs with clang
# -fsyntax-only, using the exact flags from the CMake compile database, to catch
# C++-conformance errors that the local MinGW GCC (and the Ubuntu-GCC Linux CI)
# accept but the stricter macOS CI GCC rejects -- e.g. the "declaration has a
# different language linkage" error (extern "C" mismatch) that silently broke the
# macOS build for days. clang is a locally-available proxy for that stricter
# compiler; no macOS needed.
#
# Run it before pushing a change that touches C++ (PsyCross or pc_port), and
# always before cutting a release:
#
#   ./tools/check-clang.sh                # C++ TUs only (default)
#   ./tools/check-clang.sh --filter PsyCross   # just the HAL
#   ./tools/check-clang.sh --all          # every TU incl. the decomp C (noisy)
#
# Exit 0 = clean, non-zero = a TU failed clang (fix before pushing).
set -uo pipefail

export MSYSTEM=MINGW64
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/pc_port/build"

if ! command -v clang++ >/dev/null 2>&1; then
    echo "clang not found in mingw64. Install it:" >&2
    echo "    pacman -S mingw-w64-x86_64-clang" >&2
    exit 1
fi

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    echo "No configured build at $BUILD -- configure/build the project first." >&2
    exit 1
fi

# Non-destructive: reuses the existing cache (generator, flags, compiler); only
# turns on + regenerates the compile database. Does NOT rebuild. Re-run each time
# so newly added source files are covered.
echo "Generating compile database (compile_commands.json)..."
if ! cmake "$BUILD" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null 2>cmake_err.log; then
    echo "cmake reconfigure failed:" >&2
    cat cmake_err.log >&2
    rm -f cmake_err.log
    exit 1
fi
rm -f cmake_err.log

echo "Running clang -fsyntax-only (proxy for the stricter macOS CI GCC)..."
echo ""
python "$ROOT/tools/check_clang.py" "$BUILD" "$@"
