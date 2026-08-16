#!/usr/bin/env bash
# Build the 42 map overlays beyond map0_s00 as symbol-prefixed static archives.
#
# Same scheme the Original Xbox port uses (xbox_port/Makefile.nxdk): each map is
# compiled exactly like a PC map DLL -- every src/maps/<name>/*.c plus the map's
# extracted_data TU, with -DMAP<N>_S<M> -DSH_MAP_NAME=<name> -- and then every
# DEFINED extern symbol is renamed to <name>_<sym>.
#
# The renaming is not tidiness, it is what makes linking possible at all: the
# shared AI / particle / player sources are #included into every map, so a naive
# link of all 43 hits 500+ duplicate symbols. Prefixing gives each overlay its
# own copy, which is exactly PSX overlay semantics -- on hardware each map
# overlay is a self-contained binary loaded at 0x800C9xxx.
#
# map0_s00 stays UNPREFIXED and is built by ppu_gate.sh with the rest of the
# game, matching the Xbox/360 arrangement and map_xbox.c's registry.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECOMP="$(cd "$SCRIPT_DIR/.." && pwd)"
PCPORT="$DECOMP/pc_port"
PSYCROSS="$PCPORT/PsyCross"
PS3DEV="${PS3DEV:-/usr/local/ps3dev}"

OUT="$SCRIPT_DIR/build/maps"
LOG="$SCRIPT_DIR/build/maps.log"
mkdir -p "$OUT"; : > "$LOG"

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

# Identical to ppu_gate.sh's, deliberately: a map TU is ordinary game code and
# any divergence here shows up as a link error rather than a compile one.
DEFS="-DSH_PS3_PORT -DSH_XBOX_PORT -DSH_PC_PORT -DVER_USA -DSKIP_ASM -DUSE_PGXP=0"
DEFS="$DEFS -Dstatic_assert=_Static_assert"

INCS="-I$SCRIPT_DIR/include -I$PCPORT/include -I$PCPORT/include/psyq_compat -I$PCPORT/src
      -I$DECOMP/include -I$DECOMP/include/decomp
      -I$PSYCROSS/include/psx -I$PSYCROSS/include
      -I$PS3DEV/ppu/include -I$DECOMP/xbox_port/include"

WARN="-Wno-implicit-function-declaration -Wno-implicit-int
      -Wno-incompatible-pointer-types -Wno-int-conversion
      -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
      -Wno-sign-compare -Wno-unused-variable -Wno-unused-function
      -Wno-missing-braces -Wno-parentheses -Wno-return-type -Wno-pointer-sign"

TARGETFLAGS="-D__PS3__ -D__PSL1GHT__ -mcpu=cell -mhard-float
             -fmodulo-sched -ffunction-sections -fdata-sections -fgnu89-inline"

# map4_s02 -> MAP4_S02
mapdef() { echo "$1" | tr 'a-z' 'A-Z'; }

# Newest header anywhere the maps can see. One find, not 42.
NEWEST_HDR=$(find "$DECOMP/include" "$PCPORT/include" "$PSYCROSS/include" \
                  -name '*.h' -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)

total_ok=0; total_fail=0; libs=0
for m in $EXTRA_MAPS; do
    srcs=$(ls "$DECOMP/src/maps/$m"/*.c 2>/dev/null)
    ed="$PCPORT/build_gen/extracted_data/${m}_extracted_data.c"
    [ -f "$ed" ] && srcs="$srcs $ed"
    [ -z "$srcs" ] && { echo "  $m: NO SOURCES"; continue; }

    mdir="$OUT/$m"; mkdir -p "$mdir/raw"

    # Incremental: skip a map whose archive is newer than every one of its
    # sources. Rebuilding all 42 unconditionally costs ~10 minutes a round,
    # which is most of an iteration when the thing being changed is one line of
    # shared game code.
    if [ -f "$OUT/$m.a" ] && [ "${SH_MAPS_FORCE:-0}" != "1" ]; then
        stale=0
        for f in $srcs; do
            [ "$f" -nt "$OUT/$m.a" ] && { stale=1; break; }
        done
        # Headers matter more than the map sources here: a map TU is mostly
        # #included shared code, so a change to include/ or pc_port/include/ is
        # the common reason to rebuild and comparing only .c files would happily
        # serve stale archives. NEWEST_HDR is computed once, outside the loop.
        [ -n "$NEWEST_HDR" ] && [ "$NEWEST_HDR" -nt "$OUT/$m.a" ] && stale=1
        [ "$SCRIPT_DIR/build_maps.sh" -nt "$OUT/$m.a" ] && stale=1
        if [ "$stale" -eq 0 ]; then
            printf "  %-10s up to date\n" "$m"
            libs=$((libs+1))
            continue
        fi
    fi

    ok=0; fail=0
    for f in $srcs; do
        obj="$mdir/raw/$(basename "${f%.c}").o"
        if err=$(ppu-gcc $TARGETFLAGS $DEFS $INCS $WARN \
                    -D"$(mapdef "$m")" -DSH_MAP_NAME="$m" -c "$f" -o "$obj" 2>&1); then
            ok=$((ok+1))
        else
            fail=$((fail+1))
            { echo "########## $f"; echo "$err"; } >> "$LOG"
        fi
    done
    total_ok=$((total_ok+ok)); total_fail=$((total_fail+fail))

    if [ "$fail" -gt 0 ]; then
        printf "  %-10s %2d ok %2d FAIL\n" "$m" "$ok" "$fail"
        continue
    fi

    # Rename list from the map's own objects, then apply it to all of them so
    # definitions and intra-map references move together.
    sh "$SCRIPT_DIR/tools/gen_map_syms.sh" "$m" "$mdir/$m.syms" "$mdir"/raw/*.o
    rm -f "$mdir"/*.o
    for o in "$mdir"/raw/*.o; do
        ppu-objcopy --redefine-syms="$mdir/$m.syms" "$o" "$mdir/$(basename "$o")" || fail=$((fail+1))
    done
    rm -f "$OUT/$m.a"
    ppu-ar rcs "$OUT/$m.a" "$mdir"/*.o 2>>"$LOG" && libs=$((libs+1))
    printf "  %-10s %2d objs -> %s\n" "$m" "$ok" "$(basename "$OUT/$m.a")"
done

echo "=========================================="
echo " map overlays:  $libs archives, $total_ok objs, $total_fail failed"
echo "=========================================="
[ "$total_fail" -gt 0 ] && { echo "see $LOG"; exit 1; }
exit 0
