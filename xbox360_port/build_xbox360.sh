#!/usr/bin/env bash
# Build the Silent Hill Xbox 360 port -> bin/xenon.elf
#
# Runs INSIDE the free60/libxenon container (xenon-gcc + newlib + libXenon).
# From Windows:
#   docker run --rm -v C:\Claude\silenthill-xbox360\silent-hill-decomp:/work \
#              -w /work free60/libxenon:latest \
#              /bin/bash -lc 'bash xbox360_port/build_xbox360.sh'
#
# Milestone 2 builds the BOOT TEST only (main + log sink). The game tree compiles
# (ppc_gate.sh, 171/171) but cannot link yet -- see hal_census.sh for the 284
# symbols the HAL still owes.
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECOMP="$(cd "$SCRIPT_DIR/.." && pwd)"
PCPORT="$DECOMP/pc_port"
DK="${DEVKITXENON:-/usr/local/xenon}"

OUT="$SCRIPT_DIR/bin"
OBJ="$SCRIPT_DIR/build/obj"
mkdir -p "$OUT" "$OBJ"

# -L.../xenon/lib/32 belongs to MACHDEP, not LDFLAGS. Drop it and the linker
# finds the 64-bit newlib, reports "skipping incompatible libc.a" and then
# "cannot find -lc".
MACHDEP="-DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float -L$DK/xenon/lib/32"

DEFS="-DSH_XBOX360_PORT -DSH_XBOX_PORT -DSH_PC_PORT -DVER_USA -DSKIP_ASM -DUSE_PGXP=0
      -Dstatic_assert=_Static_assert"

INCS="-I$SCRIPT_DIR/include -I$PCPORT/include -I$PCPORT/include/psyq_compat -I$PCPORT/src
      -I$DECOMP/include -I$DECOMP/include/decomp
      -I$PCPORT/PsyCross/include/psx -I$PCPORT/PsyCross/include
      -I$DECOMP/xbox_port/include
      -I$DK/usr/include"

SRCS="$SCRIPT_DIR/src/main_xbox360.c
      $SCRIPT_DIR/src/sh_log_xbox360.c"

OBJS=""
for f in $SRCS; do
    o="$OBJ/$(basename "${f%.c}").o"
    echo "[ CC  ] $(basename "$f")"
    xenon-gcc $MACHDEP -O2 -g $DEFS $INCS -c "$f" -o "$o"
    OBJS="$OBJS $o"
done

echo "[ LD  ] xenon.elf"
xenon-gcc $MACHDEP -n -T "$DK/app.lds" $OBJS \
          -L"$DK/usr/lib" -lfat -lxenon -lm \
          -Wl,-Map,"$OUT/xenon.map" -o "$OUT/xenon.debug.elf"

# XeLL loads the flat elf32 image; --adjust-vma matches libXenon's own rule.
xenon-objcopy -O elf32-powerpc --adjust-vma 0x80000000 \
              "$OUT/xenon.debug.elf" "$OUT/xenon.elf"
xenon-strip "$OUT/xenon.elf"

echo
file "$OUT/xenon.elf"
ls -l "$OUT/xenon.elf"
echo
echo "Deploy: copy bin/xenon.elf to the USB stick ROOT as xenon.elf"
echo "        (BadUpdate -> FreeMyXe -> XeLL picks it up automatically)"
