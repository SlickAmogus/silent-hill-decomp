#!/usr/bin/env bash
# Build the Silent Hill Xbox port (default.xbe) via the nxdk toolchain.
#   ./build_xbox.sh          # build
#   ./build_xbox.sh clean    # clean
#   ./build_xbox.sh iso      # build + pack ISO
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

export NXDK_DIR="$SCRIPT_DIR/nxdk"
# LLVM (clang/lld), msys2 mingw runtime (host clang++ + tools), nxdk tools
# (cxbe/extract-xiso), msys2 coreutils.
export PATH="/c/Program Files/LLVM/bin:/c/msys64/mingw64/bin:${NXDK_DIR}/bin:/c/msys64/usr/bin:$PATH"

# vcvarsall.bat (if it ran in this environment) sets INCLUDE/LIB/LIBPATH, which
# clang picks up in MSVC-compat mode and pulls in conflicting host SDK headers.
# nxdk-cc gets every path it needs via explicit -I flags, so scrub these.
unset INCLUDE LIB LIBPATH

# nxdk's host tools (cxbe, vp20compiler, fp20compiler) are compiled with the host
# C++ compiler ($(CXX), default g++). MSYS2 g++ 13.1.0 on this machine cannot
# create its temporary files (it always probes C:\Windows\ -> "Permission denied"),
# so build the host tools with MSYS2 mingw clang++ (integrated assembler, no
# external temp) up front. Once present the main make sees them up-to-date and
# skips them. The Xbox target itself always uses nxdk-cc, never $(CXX), so this
# does not affect game-code compilation.
#   NOTE: vp20compiler/fp20compiler (shaders, milestone 2) and extract-xiso (iso)
#   need the same CXX=clang++ treatment when first built.
HOST_CXX="/c/msys64/mingw64/bin/clang++"
build_host_tool() {
    local dir="$1" out="$2"
    if [ ! -f "$NXDK_DIR/$dir/$out" ]; then
        echo "[ HOSTCXX  ] $dir/$out (clang++)"
        make -C "$NXDK_DIR/$dir" CXX="$HOST_CXX"
    fi
}
build_host_tool tools/cxbe cxbe

exec /c/msys64/usr/bin/make.exe -f Makefile.nxdk "$@"
