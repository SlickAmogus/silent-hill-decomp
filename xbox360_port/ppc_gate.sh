#!/usr/bin/env bash
# Milestone 1 gate: compile the shared game tree for 32-bit big-endian PowerPC.
#
# Compiles to real objects, never -fsyntax-only: a missing header makes
# -fsyntax-only report a file CLEAN, which is how the iOS port lost a day.
#
#   ./ppc_gate.sh            # whole tree, summary + error histogram
#   ./ppc_gate.sh src/main   # restrict to a subtree
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECOMP="$(cd "$SCRIPT_DIR/.." && pwd)"
PCPORT="$DECOMP/pc_port"
PSYCROSS="$PCPORT/PsyCross"
SYSROOT="${DEVKITXENON:-$SCRIPT_DIR/devkitxenon}"

CLANG="${CLANG:-/c/Program Files/LLVM/bin/clang.exe}"
OUT="$SCRIPT_DIR/build/gate"
LOG="$SCRIPT_DIR/build/gate.log"
mkdir -p "$OUT"; : > "$LOG"

TARGET=--target=powerpc-unknown-none-elf

# SH_XBOX_PORT rides along because its gates are what select the NATIVE 32-BIT
# PSX struct layout in the reformat walkers, which 32-bit PPC wants unchanged.
# Where a gate turns out to mean "nxdk" rather than "32-bit", it gets split.
DEFS="-DSH_XBOX360_PORT -DSH_XBOX_PORT -DSH_PC_PORT -DVER_USA -DSKIP_ASM -DUSE_PGXP=0"
# PsyCross asserts every PSX primitive's size in longs. Wiring these up is the
# point, not a formality: they are the first thing that would catch PPC packing
# drift in the structs that overlay disc data.
DEFS="$DEFS -Dstatic_assert=_Static_assert"

INCS="-I$SCRIPT_DIR/include -I$PCPORT/include -I$PCPORT/include/psyq_compat -I$PCPORT/src
      -I$DECOMP/include -I$DECOMP/include/decomp
      -I$PSYCROSS/include/psx -I$PSYCROSS/include"
[ -d "$SYSROOT/usr/include" ] && INCS="$INCS -isystem $SYSROOT/usr/include"

# Mirrors the Xbox port's suppression set: the decomp leans on gcc-permissive
# diagnostics that clang promotes to hard errors.
WARN="-Wno-implicit-function-declaration -Wno-implicit-int
      -Wno-incompatible-function-pointer-types -Wno-incompatible-pointer-types
      -Wno-int-conversion -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
      -Wno-sign-compare -Wno-unused-variable -Wno-unused-function
      -Wno-missing-braces -Wno-parentheses
      -Wno-tautological-constant-out-of-range-compare -Wno-return-type
      -Wno-pointer-sign"

collect_srcs() {
    local root="${1:-}"
    if [ -n "$root" ]; then
        find "$DECOMP/$root" -name '*.c'
        return
    fi
    find "$DECOMP/src/main" -maxdepth 1 -name '*.c' ! -name 'main.c' ! -name 'memcpy.c'
    find "$DECOMP/src/bodyprog" -name '*.c' ! -name 'bodyprog_80032D1C.c' ! -name 'text_draw_jp.c'
    find "$DECOMP/src/screens" -name '*.c' ! -name 'hp_safe1.c' ! -name 's__safe2.c'
    find "$PCPORT/src" -maxdepth 1 -name '*.c'
}

pass=0; fail=0; failed_files=()
while IFS= read -r f; do
    [ -z "$f" ] && continue
    obj="$OUT/$(echo "${f#$DECOMP/}" | tr '/' '_').o"
    if err=$("$CLANG" $TARGET -ffreestanding $DEFS $INCS $WARN -c "$f" -o "$obj" 2>&1); then
        pass=$((pass+1))
    else
        fail=$((fail+1)); failed_files+=("$f")
        { echo "########## $f"; echo "$err"; } >> "$LOG"
    fi
done < <(collect_srcs "${1:-}")

echo "=========================================="
echo " PPC big-endian compile gate"
echo "   passed : $pass"
echo "   failed : $fail"
echo "   total  : $((pass+fail))"
echo "=========================================="
if [ "$fail" -gt 0 ]; then
    echo
    echo "Top error kinds:"
    grep -h "error:" "$LOG" | sed 's/.*error: //' \
        | sed "s/'[^']*'/'X'/g" | sort | uniq -c | sort -rn | head -20
    echo
    echo "Full log: $LOG"
fi
