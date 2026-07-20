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
# C/C++ compiler ($(CC)/$(CXX), default gcc/g++). MSYS2 gcc/g++ 13.1.0 on this
# machine cannot create its temporary files (it always probes C:\Windows\ ->
# "Permission denied"), so build the host tools with MSYS2 mingw clang/clang++
# (integrated assembler, no external temp) up front. Once present the main make
# sees them up-to-date and skips them. The Xbox target itself always uses
# nxdk-cc, never $(CC)/$(CXX), so this does not affect game-code compilation.
#   (extract-xiso, for the .iso target, uses CMake->gcc and needs the same
#   treatment when an ISO is first built — not wired up here.)
HOST_CC="/c/msys64/mingw64/bin/clang"
HOST_CXX="/c/msys64/mingw64/bin/clang++"
build_host_tool() {
    # $1=dir  $2=output-binary  $3..=make var overrides (e.g. CXX=...)
    local dir="$1" out="$2"; shift 2
    if [ ! -f "$NXDK_DIR/$dir/$out" ] && [ ! -f "$NXDK_DIR/$dir/$out.exe" ]; then
        echo "[ HOSTCC   ] $dir/$out"
        make -C "$NXDK_DIR/$dir" "$@"
    fi
}
build_host_tool tools/cxbe         cxbe         CXX="$HOST_CXX"
build_host_tool tools/vp20compiler vp20compiler CC="$HOST_CC"
build_host_tool tools/fp20compiler fp20compiler CXX="$HOST_CXX"

# Build identification header (mirrors pc_port's cmake/gen_build_info.cmake):
# first thing to check in a user log is which xbe produced it. The .c.d
# dependency tracking does NOT cover this header (a hardware log carried a
# stale sha), so when the content changes, delete main_xbox.obj to force the
# recompile ourselves.
GIT_SHA="$(git -C "$SCRIPT_DIR/.." rev-parse --short=9 HEAD 2>/dev/null || echo unknown)"
if [ -n "$(git -C "$SCRIPT_DIR/.." status --porcelain --untracked-files=no 2>/dev/null | head -1)" ]; then
    GIT_SHA="${GIT_SHA}+dirty"
fi
printf '#define SH_XBOX_BUILD_HASH "%s"\n#define SH_XBOX_BUILD_STAMP "%s"\n' \
    "$GIT_SHA" "$(date '+%Y-%m-%d %H:%M:%S')" > include/sh_build_info_xbox.h.new
if ! cmp -s include/sh_build_info_xbox.h.new include/sh_build_info_xbox.h 2>/dev/null; then
    mv include/sh_build_info_xbox.h.new include/sh_build_info_xbox.h
    rm -f src/main_xbox.obj
else
    rm -f include/sh_build_info_xbox.h.new
fi

# Orphan-object sweep. An .obj whose .c.d dependency file is missing NEVER
# rebuilds on header changes (make sees obj newer than .c and knows no header
# deps) — that hole shipped a torn binary: 98 pre-merge objects incl. the
# g_SysWork definer linked against post-merge headers, so every post-npcs
# g_SysWork access landed in foreign globals (looked exactly like wild memory
# corruption on hardware). Delete any orphan so it recompiles with tracking.
# src/dsound_objs are prebuilt XDK objects with no sources — skip them.
for o in $(find "$SCRIPT_DIR/../src" "$SCRIPT_DIR/../pc_port/src" "$SCRIPT_DIR/src" \
           -name '*.obj' -not -path '*dsound_objs*' 2>/dev/null); do
    base="${o%.obj}"
    if [ ! -f "$base.c.d" ] && [ ! -f "$base.cpp.d" ]; then
        echo "[ ORPHAN   ] $o (no .d — forcing rebuild)"
        rm -f "$o"
    fi
done

exec /c/msys64/usr/bin/make.exe -f Makefile.nxdk "$@"
