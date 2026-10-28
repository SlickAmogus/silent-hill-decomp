#!/usr/bin/env bash
# Build the Silent Hill Xbox 360 XDK port -> xbox360_port/xdk/bin/default.xex
#
# Compilation is delegated to gate.sh, so the XEX is by construction exactly
# what the gate verified (two source lists would drift, and the failure mode
# of that drift is "gate green, link red"). Then link.exe makes a PE for the
# PPCBE target and imagexex wraps it as a XEX.
#
#   bash xbox360_port/xdk/build_xex.sh            # full build
#   SKIP_GATE=1 bash xbox360_port/xdk/build_xex.sh # relink only
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="$(cd "$SCRIPT_DIR/.." && pwd)"
. "$SCRIPT_DIR/xdk_env.sh" || exit 1

GATE="$PORT/build/xdk/gate"
OUT="$SCRIPT_DIR/bin"
BUILD="$PORT/build/xdk"
mkdir -p "$OUT"
w() { cygpath -w "$1"; }

if [ -z "${SKIP_GATE:-}" ]; then
    echo "=== compile ==="
    bash "$SCRIPT_DIR/gate.sh" || exit 1
    if ls "$GATE"/*.err >/dev/null 2>&1; then
        echo "compile errors - not linking"
        exit 1
    fi
fi
ls "$GATE"/*.obj >/dev/null 2>&1 || { echo "no objects to link"; exit 1; }

if [ -z "${SKIP_MAPS:-}" ]; then
    echo
    echo "=== map overlays ==="
    bash "$SCRIPT_DIR/build_maps.sh" || exit 1
fi
MAPLIBS=$(ls "$BUILD"/maps/*.lib 2>/dev/null | while read -r l; do w "$l"; done | tr '\n' ' ')

echo
echo "=== link ($(ls "$GATE"/*.obj | wc -l) objects, $(echo $MAPLIBS | wc -w) map libs) ==="
{ ls "$GATE"/*.obj | while read -r o; do w "$o"; done; echo "$MAPLIBS"; } > "$BUILD/link_objs.rsp"
# Release libraries only. xbdm.lib (the debug monitor) is deliberately
# absent: it imports xbdm.xex, which a retail console does not have, so a
# XEX linked against it fails to load there.
LIBS="xapilib.lib d3d9.lib d3dx9.lib xgraphics.lib xaudio2.lib xmcore.lib xboxkrnl.lib"
if ! link -nologo -machine:PPCBE -subsystem:XBOX -xex:no \
        -out:"$(w "$OUT/silenthill.exe")" -map:"$(w "$OUT/silenthill.map")" \
        @"$(w "$BUILD/link_objs.rsp")" $LIBS > "$BUILD/link.log" 2>&1; then
    echo "LINK FAILED - unresolved symbols by count:"
    grep -oE "unresolved external symbol [^ ]+" "$BUILD/link.log" | sed 's/unresolved external symbol //' \
        | sort | uniq -c | sort -rn | head -40
    echo
    echo "total distinct unresolved: $(grep -oE "unresolved external symbol [^ ]+" "$BUILD/link.log" | sort -u | wc -l)"
    grep -oE "unresolved external symbol [^ ]+" "$BUILD/link.log" | sed 's/unresolved external symbol //' \
        | sort -u > "$BUILD/undefined.txt"
    grep -vE "unresolved external symbol|^\s*$" "$BUILD/link.log" | head -20
    echo "full log: $BUILD/link.log"
    exit 1
fi

echo
echo "=== imagexex ==="
if ! imagexex -nologo -in:"$(w "$OUT/silenthill.exe")" -out:"$(w "$OUT/default.xex")" > "$BUILD/imagexex.log" 2>&1; then
    cat "$BUILD/imagexex.log"
    exit 1
fi
ls -l "$OUT/default.xex"
echo
echo "Deploy: copy bin/default.xex into a folder with silenthill\\<disc>.bin beside it"
