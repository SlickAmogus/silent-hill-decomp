#!/usr/bin/env bash
# Recompile a few TUs of the XDK gate in place (same flags, same object
# names as gate.sh) without wiping the rest, for a quick relink:
#   bash xbox360_port/xdk/cc_one.sh src/bodyprog/gfx/world_draw.c xbox360_port/xdk/src/audio_xaudio2.cpp
#   SKIP_GATE=1 SKIP_MAPS=1 bash xbox360_port/xdk/build_xex.sh
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="$(cd "$SCRIPT_DIR/.." && pwd)"
DECOMP="$(cd "$PORT/.." && pwd)"
PCPORT="$DECOMP/pc_port"
PSYCROSS="$PCPORT/PsyCross"
. "$SCRIPT_DIR/xdk_env.sh" || exit 1
OUT="$PORT/build/xdk/gate"
w() { cygpath -w "$1"; }
export DEFS="-D_XBOX -DSH_XBOX360_PORT -DSH_XBOX360_XDK -DSH_XBOX_PORT -DSH_PC_PORT -DVER_USA -DSKIP_ASM -DUSE_PGXP=0"
export INCS="-I$(w "$SCRIPT_DIR/include") -I$(w "$PORT/include") \
 -I$(w "$PCPORT/include") -I$(w "$PCPORT/include/psyq_compat") -I$(w "$PCPORT/src") \
 -I$(w "$DECOMP/include") -I$(w "$DECOMP/include/decomp") \
 -I$(w "$PSYCROSS/include/psx") -I$(w "$PSYCROSS/include")"
export FLAGS="-nologo -c -w -FI$(w "$SCRIPT_DIR/xdk_compat.h") ${SH_OPT:--Od}"
export XDK_TOOLS="$SCRIPT_DIR/tools"
export XDK_COMPAT="$SCRIPT_DIR/xdk_compat.h"
export CLANG="${CLANG:-/c/Program Files/LLVM/bin/clang.exe}"
rc=0
for rel in "$@"; do
    f="$DECOMP/$rel"
    obj="$OUT/$(echo "$rel" | tr '/' '_' | sed 's/\.\(c\|cpp\)$/.obj/')"
    case "$f" in
        */maps/map0_s00/*|*map0_s00_extracted_data.c) extra="-DMAP0_S00 -DSH_MAP_NAME=map0_s00" ;;
        *) extra="" ;;
    esac
    bash "$SCRIPT_DIR/cc1.sh" "$f|$obj|$extra"
    if [ -f "$obj.err" ]; then echo "FAILED: $rel"; cat "$obj.err"; rc=1; else echo "ok: $rel -> $(basename "$obj")"; fi
done
exit $rc
