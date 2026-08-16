#!/usr/bin/env bash
# Build the Silent Hill PS3 port -> bin/sh.elf, bin/EBOOT.BIN
#
# Runs INSIDE the ps3dev container:
#   docker run --rm -v C:\Claude\silenthill-ps3\silent-hill-decomp:/work -w /work \
#     scrapes/ps3toolchain-minimal:latest bash -lc 'bash ps3_port/build_ps3.sh'
#
# Compilation is delegated to ppu_gate.sh rather than duplicated here. That is
# the point: the gate owns the one source list and the one flag set, so the ELF
# is by construction exactly what the gate verified. Two lists would drift, and
# the failure mode of that drift is "gate green, link red".
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PS3DEV="${PS3DEV:-/usr/local/ps3dev}"
OUT="$SCRIPT_DIR/bin"
GATE="$SCRIPT_DIR/build/gate"
mkdir -p "$OUT"

echo "=== compile ==="
bash "$SCRIPT_DIR/ppu_gate.sh"

ls "$GATE"/*.o >/dev/null 2>&1 || { echo "no objects to link"; exit 1; }

# PSL1GHT's own MACHDEP, verbatim from $PS3DEV/ppu_rules.
MACHDEP="-mhard-float -fmodulo-sched -ffunction-sections -fdata-sections"

# lv2 last: it is the syscall floor everything else resolves down into.
LIBS="-lrsx -lgcm_sys -lsysutil -lsysmodule -laudio -lio -lnet -lsysfs -llv2 -lm"

echo
echo "=== link ($(ls "$GATE"/*.o | wc -l) objects) ==="
if ! ppu-gcc $MACHDEP -o "$OUT/sh.elf" "$GATE"/*.o \
        -L"$PS3DEV/ppu/lib" $LIBS \
        -Wl,-Map,"$OUT/sh.map" 2> "$SCRIPT_DIR/build/link.log"; then
    echo "LINK FAILED - undefined symbols by count:"
    grep -oP "undefined reference to \`\K[^']+" "$SCRIPT_DIR/build/link.log" \
        | sort | uniq -c | sort -rn | head -40
    echo
    echo "total distinct undefined: $(grep -oP "undefined reference to \`\K[^']+" "$SCRIPT_DIR/build/link.log" | sort -u | wc -l)"
    grep -oP "undefined reference to \`\K[^']+" "$SCRIPT_DIR/build/link.log" \
        | sort -u > "$SCRIPT_DIR/build/undefined.txt"
    echo "full list: ps3_port/build/undefined.txt"
    echo "other link errors:"
    grep -v "undefined reference to" "$SCRIPT_DIR/build/link.log" | head -20
    exit 1
fi

# ELF -> EBOOT.BIN, the bootable artifact. sprxlinker rewrites the PRX import
# stubs in place, so it runs on a STRIPPED COPY and never on the debug ELF we
# want to keep for symbolised crash addresses.
cp "$OUT/sh.elf" "$OUT/sh.stripped.elf"
ppu-strip "$OUT/sh.stripped.elf"
sprxlinker "$OUT/sh.stripped.elf"
make_self "$OUT/sh.stripped.elf" "$OUT/EBOOT.BIN"

echo
ppu-size "$OUT/sh.elf" 2>/dev/null || true
ls -l "$OUT/sh.elf" "$OUT/EBOOT.BIN"
echo
echo "Run: ps3_port/tools/rpcs3_smoke.sh ps3_port/bin/EBOOT.BIN"
