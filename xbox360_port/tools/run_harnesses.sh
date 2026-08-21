#!/usr/bin/env bash
# Run the endian harnesses on the desktop, little- AND big-endian, and diff.
#
#   docker run --rm -v C:\Claude\silenthill-xbox360\silent-hill-decomp:/work \
#              -w /work sh360-qemu:latest bash xbox360_port/tools/run_harnesses.sh
#
# The point of these is that they cost SECONDS instead of a BadUpdate run. Every
# bug they can reproduce is a bug that must never be diagnosed on hardware: boot
# the 360, load XeLL, run, pull the stick, read the log -- minutes each, and the
# user has to be at the console. The GTE and the primitive packers are pure
# fixed-point C with no platform dependency, so powerpc-linux-gnu + qemu-ppc
# exercises the SAME big-endian behaviour the Xenon sees.
#
# Flags mirror ppc_gate.sh deliberately. -fgnu89-inline in particular is not
# cosmetic: libgte.c declares helpers as bare C99 `inline` (no extern, no
# static), which emits NO external definition, and at -O0 nothing is inlined --
# so without it the link fails on fst_min/fst_max. The real build only works
# because the gate passes this flag.
set -u

DECOMP="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$DECOMP"
PSY=pc_port/PsyCross

INC="-I$PSY/include/psx -I$PSY/include -I$PSY/src -Iinclude -Iinclude/decomp -Ipc_port/include"
# static_assert: the decomp headers use C11 static_assert, which the C++ TU has
# natively -- defining it there breaks libstdc++. C files only.
CDEFS="-Dstatic_assert=_Static_assert -DUSE_PGXP=0 -fgnu89-inline"
fail=0

# $1=cc  $2=cxx  $3=tag  $4=runner  $5=extra
build_run_gte() {
    local cc="$1" cxx="$2" tag="$3" runner="$4" extra="$5"
    local od=/tmp/gte_$tag; rm -rf "$od"; mkdir -p "$od"
    local ok=1
    for f in xbox360_port/tools/gte_harness.c $PSY/src/psx/libgte.c $PSY/src/psx/inline_c.c; do
        $cc -w $extra $CDEFS $INC -c "$f" -o "$od/$(basename "$f").o" || ok=0
    done
    $cxx -w $extra -DUSE_PGXP=0 $INC -c $PSY/src/gte/PsyX_GTE.cpp -o "$od/gtecpp.o" || ok=0
    [ "$ok" = 1 ] || { echo "  BUILD FAILED"; fail=1; return; }
    $cxx -w $extra "$od"/*.o -o "$od/gte" || { echo "  LINK FAILED"; fail=1; return; }
    $runner "$od/gte" || fail=1
}

build_run_prim() {
    local cc="$1" tag="$2" runner="$3" extra="$4"
    local out=/tmp/prim_$tag
    $cc -w $extra $CDEFS $INC xbox360_port/tools/prim_harness.c -o "$out" \
        || { echo "  BUILD FAILED"; fail=1; return; }
    $runner "$out" || fail=1
}

echo "################ prim_harness ################"
echo "--- little-endian (native x86-64) ---"
build_run_prim gcc le ""        ""
echo "--- BIG-endian (powerpc, qemu) ---"
build_run_prim powerpc-linux-gnu-gcc be qemu-ppc "-static"

echo
echo "################ gte_harness ################"
echo "--- little-endian (native x86-64) ---"
build_run_gte gcc g++ le "" ""
echo "--- BIG-endian (powerpc, qemu) ---"
build_run_gte powerpc-linux-gnu-gcc powerpc-linux-gnu-g++ be qemu-ppc "-static"

echo
echo "################ dat_harness (demo .DAT swapper) ################"
# Needs the decomp include order, PsyCross LAST: pc_port/include and
# include/psyq both define MATRIX/VECTOR/SVECTOR, and PsyCross first wins the
# wrong one.
DATINC="-Ixbox360_port/include -Ipc_port/include -Ipc_port/include/psyq_compat -Ipc_port/src
        -Iinclude -Iinclude/decomp -I$PSY/include/psx -I$PSY/include"
DATDEF="$CDEFS -DSH_PC_PORT -DVER_USA -DSKIP_ASM"
DATSRC="xbox360_port/tools/dat_harness.c pc_port/src/dat_endian.c"
echo "--- little-endian (native x86-64) ---"
gcc -w $DATDEF $DATINC $DATSRC -o /tmp/dat_le && /tmp/dat_le || fail=1
echo "--- BIG-endian (powerpc, qemu) ---"
powerpc-linux-gnu-gcc -w -static $DATDEF $DATINC $DATSRC -o /tmp/dat_be && qemu-ppc /tmp/dat_be || fail=1

echo
echo "################ tmd_harness (TMD model swapper) ################"
TMDINC="-Ipc_port/include -Iinclude"
TMDSUP='-DSH_LOG_H -include stdio.h -DSH_DBG(...)=(void)0'
echo "--- little-endian (native x86-64) ---"
gcc -w $TMDINC $TMDSUP xbox360_port/tools/tmd_harness.c pc_port/src/tmd_endian.c -o /tmp/tmd_le && /tmp/tmd_le || fail=1
echo "--- BIG-endian (powerpc, qemu) ---"
powerpc-linux-gnu-gcc -w -static $TMDINC $TMDSUP xbox360_port/tools/tmd_harness.c pc_port/src/tmd_endian.c -o /tmp/tmd_be && qemu-ppc /tmp/tmd_be || fail=1

echo
if [ -f ps3_port/tools/pack_selftest.c ]; then
    echo "################ pack_selftest (from the PS3 port) ################"
    # Cross-checks THIS branch's PsyCross against the PS3 branch's packing
    # macros: if both ports agree on byte placement, their fixes compose.
    echo "--- little-endian ---"
    gcc -w $CDEFS $INC ps3_port/tools/pack_selftest.c -o /tmp/pack_le 2>/dev/null \
        && /tmp/pack_le || { echo "  (skipped: needs PS3 headers)"; }
    echo "--- BIG-endian ---"
    powerpc-linux-gnu-gcc -w -static $CDEFS $INC ps3_port/tools/pack_selftest.c -o /tmp/pack_be 2>/dev/null \
        && qemu-ppc /tmp/pack_be || { echo "  (skipped: needs PS3 headers)"; }
fi

echo
if [ "$fail" = 0 ]; then echo "ALL HARNESSES PASSED"; else echo "SOME HARNESSES FAILED"; fi
exit $fail
