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

echo "=== shaders ==="
# cgcomp is only a front end: it dlopen's NVIDIA's libCg.so and dies with
# "Unable to load Cg, aborting" without it, so this needs the sh-ps3dev:cg image
# (ps3dev_cg.dockerfile), not the stock toolchain image.
SHD="$SCRIPT_DIR/build/shaders"
mkdir -p "$SHD"
command -v cgcomp >/dev/null 2>&1 || { echo "cgcomp not found"; exit 1; }
cgcomp -v "$SCRIPT_DIR/shaders/sh_vp.vcg" "$SHD/sh_vp.vpo" || exit 1
cgcomp -f "$SCRIPT_DIR/shaders/sh_fp.fcg" "$SHD/sh_fp.fpo" || exit 1
# bin2s emits `.global <name>_vpo` / `<name>_fpo`, which is what rsx_video.c
# declares extern. 64-byte aligned because the RSX wants its ucode aligned.
( cd "$SHD" && bin2s -a 64 sh_vp.vpo > sh_vp.s && bin2s -a 64 sh_fp.fpo > sh_fp.s )
ppu-gcc -c "$SHD/sh_vp.s" -o "$SHD/sh_vp.o" || exit 1
ppu-gcc -c "$SHD/sh_fp.s" -o "$SHD/sh_fp.o" || exit 1
ls -l "$SHD"/*.vpo "$SHD"/*.fpo

echo
echo "=== compile ==="
bash "$SCRIPT_DIR/ppu_gate.sh"

ls "$GATE"/*.o >/dev/null 2>&1 || { echo "no objects to link"; exit 1; }

echo
echo "=== map overlays ==="
bash "$SCRIPT_DIR/build_maps.sh" || exit 1
MAPLIBS=$(ls "$SCRIPT_DIR"/build/maps/*.a 2>/dev/null | tr '
' ' ')

# PSL1GHT's own MACHDEP, verbatim from $PS3DEV/ppu_rules.
MACHDEP="-mhard-float -fmodulo-sched -ffunction-sections -fdata-sections"

# lv2 last: it is the syscall floor everything else resolves down into.
LIBS="-lrsx -lgcm_sys -lsysutil -lsysmodule -laudio -lio -lnet -lsysfs -llv2 -lm"

echo
echo "=== link ($(ls "$GATE"/*.o | wc -l) objects) ==="
# --whole-archive: some map objects are reached only through constructors
# and data tables, never by a symbol the linker is already looking for, so
# lazy archive semantics would silently drop them.
if ! ppu-gcc $MACHDEP -o "$OUT/sh.elf" "$GATE"/*.o "$SHD"/sh_vp.o "$SHD"/sh_fp.o \
        -Wl,--whole-archive $MAPLIBS -Wl,--no-whole-archive \
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
