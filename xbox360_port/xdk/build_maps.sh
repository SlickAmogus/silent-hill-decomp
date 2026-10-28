#!/usr/bin/env bash
# Build the 42 map overlays beyond map0_s00 as symbol-prefixed static
# libraries for the XDK build. Same scheme as xbox360_port/build_maps.sh
# (libXenon) and the Original Xbox port: each map is compiled like a PC map
# DLL -- every src/maps/<name>/*.c plus the map's extracted_data TU, with
# -DMAP<N>_S<M> -DSH_MAP_NAME=<name> -- and then every DEFINED extern symbol
# is renamed to <name>_<sym> (tools/coff_rename.py, the COFF stand-in for
# objcopy --redefine-syms).
#
# map0_s00 stays UNPREFIXED and is built by gate.sh with the rest of the
# game, matching map_xbox.c's registry.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="$(cd "$SCRIPT_DIR/.." && pwd)"
DECOMP="$(cd "$PORT/.." && pwd)"
PCPORT="$DECOMP/pc_port"
PSYCROSS="$PCPORT/PsyCross"
. "$SCRIPT_DIR/xdk_env.sh" || exit 1

OUT="$PORT/build/xdk/maps"
LOG="$PORT/build/xdk/maps.log"
mkdir -p "$OUT"; : > "$LOG"
w() { cygpath -w "$1"; }

EXTRA_MAPS="
map0_s01 map0_s02
map1_s00 map1_s01 map1_s02 map1_s03 map1_s04 map1_s05 map1_s06
map2_s00 map2_s01 map2_s02 map2_s03 map2_s04
map3_s00 map3_s01 map3_s02 map3_s03 map3_s04 map3_s05 map3_s06
map4_s00 map4_s01 map4_s02 map4_s03 map4_s04 map4_s05 map4_s06
map5_s00 map5_s01 map5_s02 map5_s03
map6_s00 map6_s01 map6_s02 map6_s03 map6_s04 map6_s05
map7_s00 map7_s01 map7_s02 map7_s03
"

# Identical to gate.sh's, deliberately: a map TU is ordinary game code and
# any divergence here shows up as a link error rather than a compile one.
export DEFS="-D_XBOX -DSH_XBOX360_PORT -DSH_XBOX360_XDK -DSH_XBOX_PORT -DSH_PC_PORT -DVER_USA -DSKIP_ASM -DUSE_PGXP=0"
export INCS="-I$(w "$SCRIPT_DIR/include") -I$(w "$PORT/include") \
 -I$(w "$PCPORT/include") -I$(w "$PCPORT/include/psyq_compat") -I$(w "$PCPORT/src") \
 -I$(w "$DECOMP/include") -I$(w "$DECOMP/include/decomp") \
 -I$(w "$PSYCROSS/include/psx") -I$(w "$PSYCROSS/include")"
export FLAGS="-nologo -c -w -FI$(w "$SCRIPT_DIR/xdk_compat.h") ${SH_OPT:--Od}"
export XDK_TOOLS="$SCRIPT_DIR/tools"
export XDK_COMPAT="$SCRIPT_DIR/xdk_compat.h"
export CLANG="${CLANG:-/c/Program Files/LLVM/bin/clang.exe}"

mapdef() { echo "$1" | tr 'a-z' 'A-Z'; }

JOBFILE="$OUT/jobs.txt"; : > "$JOBFILE"
for m in $EXTRA_MAPS; do
    mdir="$OUT/$m"; mkdir -p "$mdir"
    rm -f "$mdir"/*.obj "$mdir"/*.err "$mdir"/*.warn "$mdir"/*.c89.c "$OUT/$m.lib"
    for f in "$DECOMP/src/maps/$m"/*.c "$PCPORT/build_gen/extracted_data/${m}_extracted_data.c"; do
        [ -f "$f" ] || continue
        echo "$f|$mdir/$(basename "${f%.c}").obj|-D$(mapdef "$m") -DSH_MAP_NAME=$m" >> "$JOBFILE"
    done
done

echo "compiling $(wc -l < "$JOBFILE") map TUs..."
xargs -P "${JOBS:-6}" -d '\n' -I{} bash "$SCRIPT_DIR/cc1.sh" "{}" < "$JOBFILE"

total_ok=0; total_fail=0; libs=0
for m in $EXTRA_MAPS; do
    mdir="$OUT/$m"
    ok=$(ls "$mdir"/*.obj 2>/dev/null | wc -l)
    fail=$(ls "$mdir"/*.err 2>/dev/null | wc -l)
    total_ok=$((total_ok+ok)); total_fail=$((total_fail+fail))
    for e in "$mdir"/*.err; do [ -f "$e" ] && { echo "########## $e"; cat "$e"; } >> "$LOG"; done
    if [ "$fail" -gt 0 ] || [ "$ok" -eq 0 ]; then
        printf "  %-10s %2d ok %2d FAIL\n" "$m" "$ok" "$fail"
        continue
    fi
    # Rename definitions and intra-map references together, then archive.
    python "$(w "$SCRIPT_DIR/tools/coff_rename.py")" "${m}_" $(for o in "$mdir"/*.obj; do w "$o"; done) >> "$LOG" || { echo "  $m: rename FAILED"; continue; }
    if lib -nologo -out:"$(w "$OUT/$m.lib")" $(for o in "$mdir"/*.obj; do w "$o"; done) >> "$LOG" 2>&1; then
        libs=$((libs+1))
        printf "  %-10s %2d objs -> %s\n" "$m" "$ok" "$m.lib"
    else
        printf "  %-10s lib FAILED\n" "$m"
    fi
done

echo "=========================================="
echo " map overlays:  $libs libraries, $total_ok objs, $total_fail failed"
echo "=========================================="
[ "$total_fail" -gt 0 ] && { echo "see $LOG"; exit 1; }
exit 0
