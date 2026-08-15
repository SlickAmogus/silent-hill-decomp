#!/usr/bin/env bash
# Build the Silent Hill Xbox 360 port -> bin/xenon.elf
#
# Runs INSIDE the free60/libxenon container (xenon-gcc + newlib + libXenon):
#   docker run --rm -v C:\Claude\silenthill-xbox360\silent-hill-decomp:/work \
#              -w /work free60/libxenon:latest \
#              /bin/bash -lc 'bash xbox360_port/build_xbox360.sh'
#
# Compilation is delegated to ppc_gate.sh rather than duplicated here. That is
# the point: the gate owns the one source list and the one flag set, so the ELF
# is by construction exactly what the gate verified. Two lists would drift, and
# the failure mode of that drift is "gate green, link red".
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DK="${DEVKITXENON:-/usr/local/xenon}"
OUT="$SCRIPT_DIR/bin"
GATE="$SCRIPT_DIR/build/gate"
mkdir -p "$OUT"

echo "=== compile ==="
bash "$SCRIPT_DIR/ppc_gate.sh"

ls "$GATE"/*.o >/dev/null 2>&1 || { echo "no objects to link"; exit 1; }

# -L.../xenon/lib/32 belongs to MACHDEP, not LDFLAGS. Drop it and the linker
# finds the 64-bit newlib, reports "skipping incompatible libc.a" and then
# "cannot find -lc".
MACHDEP="-DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float -L$DK/xenon/lib/32"

echo
echo "=== link ($(ls "$GATE"/*.o | wc -l) objects) ==="
if ! xenon-gcc $MACHDEP -n -T "$DK/app.lds" "$GATE"/*.o \
        -L"$DK/usr/lib" -lfat -lxenon -lm \
        -Wl,-Map,"$OUT/xenon.map" -o "$OUT/xenon.debug.elf" 2> "$SCRIPT_DIR/build/link.log"; then
    echo "LINK FAILED - undefined symbols by count:"
    grep -oP "undefined reference to \`\K[^']+" "$SCRIPT_DIR/build/link.log" \
        | sort | uniq -c | sort -rn | head -40
    echo
    echo "total distinct undefined: $(grep -oP "undefined reference to \`\K[^']+" "$SCRIPT_DIR/build/link.log" | sort -u | wc -l)"
    grep -oP "undefined reference to \`\K[^']+" "$SCRIPT_DIR/build/link.log" \
        | sort -u > "$SCRIPT_DIR/build/undefined.txt"
    echo "full list: xbox360_port/build/undefined.txt"
    exit 1
fi

# XeLL loads the flat elf32 image; --adjust-vma matches libXenon's own rule.
xenon-objcopy -O elf32-powerpc --adjust-vma 0x80000000 \
              "$OUT/xenon.debug.elf" "$OUT/xenon.elf"
xenon-strip "$OUT/xenon.elf"

echo
file "$OUT/xenon.elf"
ls -l "$OUT/xenon.elf"
echo
echo "Deploy: copy bin/xenon.elf to the USB stick ROOT as xenon.elf"
