#!/usr/bin/env bash
# What does the game reference that nothing on 360 provides yet?
#
# Undefined symbols across every game object, minus what the game itself
# defines, minus what libXenon and newlib provide. What is left is exactly the
# HAL that has to be written before the ELF can link.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GATE="$SCRIPT_DIR/build/gate"
NM="${NM:-xenon-nm}"
DK="${DEVKITXENON:-/usr/local/xenon}"

# Stale objects from an earlier scope would donate symbols the current build
# never compiles, which is the one way this census can lie.
rm -rf "$GATE"
bash "$SCRIPT_DIR/ppc_gate.sh" "$@" || true

cd "$GATE" || exit 1
ls *.o >/dev/null 2>&1 || { echo "no objects"; exit 1; }

$NM --defined-only *.o 2>/dev/null | awk '{print $NF}' | sort -u > /tmp/defined.txt
$NM --undefined-only *.o 2>/dev/null | awk '{print $NF}' | sort -u > /tmp/undef.txt

: > /tmp/lib.txt
for a in "$DK"/usr/lib/libxenon.a "$DK"/xenon/lib/32/libc.a "$DK"/xenon/lib/32/libm.a; do
    [ -f "$a" ] && $NM --defined-only "$a" 2>/dev/null | awk '{print $NF}' >> /tmp/lib.txt
done
sort -u /tmp/lib.txt -o /tmp/lib.txt

comm -13 /tmp/defined.txt /tmp/undef.txt > /tmp/notlocal.txt
comm -13 /tmp/lib.txt /tmp/notlocal.txt > /tmp/missing.txt

echo "=========================================="
echo " HAL census"
echo "   defined by game objects : $(wc -l < /tmp/defined.txt)"
echo "   undefined references    : $(wc -l < /tmp/undef.txt)"
echo "   provided by libXenon/libc: $(wc -l < /tmp/lib.txt)"
echo "   STILL MISSING           : $(wc -l < /tmp/missing.txt)"
echo "=========================================="
cat /tmp/missing.txt
