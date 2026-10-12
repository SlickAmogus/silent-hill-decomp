#!/usr/bin/env bash
# Build gpu_min.elf -> xbox360_port/bin/gpu_min.elf. Runs inside free60/libxenon,
# same as build_xbox360.sh.
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/../../bin"
DK="${DEVKITXENON:-/usr/local/xenon}"
MACHDEP="-DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float -L$DK/xenon/lib/32"
TMP="$(mktemp -d)"
mkdir -p "$OUT"

xenon-gcc $MACHDEP -O2 -I"$DK/usr/include" -c "$HERE/gpu_min.c" -o "$TMP/gpu_min.o"
xenon-gcc $MACHDEP -n -T "$DK/app.lds" "$TMP/gpu_min.o" \
          -L"$DK/usr/lib" -lxenon -lm -o "$TMP/gpu_min.debug.elf"
xenon-objcopy -O elf32-powerpc --adjust-vma 0x80000000 "$TMP/gpu_min.debug.elf" "$OUT/gpu_min.elf"
xenon-strip "$OUT/gpu_min.elf"
ls -l "$OUT/gpu_min.elf"
