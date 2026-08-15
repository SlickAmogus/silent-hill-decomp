#!/usr/bin/env bash
# Milestone 1 gate: compile the shared game tree for 32-bit big-endian PowerPC.
#
# Compiles to real objects, never -fsyntax-only: a missing header makes
# -fsyntax-only report a file CLEAN, which is how the iOS port lost a day.
#
# Prefers xenon-gcc (the compiler that will actually build the ELF) and falls
# back to host clang, which needs no container but is not what ships.
#
#   ./ppc_gate.sh            # whole tree
#   ./ppc_gate.sh src/main   # restrict to a subtree
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECOMP="$(cd "$SCRIPT_DIR/.." && pwd)"
PCPORT="$DECOMP/pc_port"
PSYCROSS="$PCPORT/PsyCross"

if [ -n "${CC:-}" ]; then
    :
elif command -v xenon-gcc >/dev/null 2>&1; then
    CC=xenon-gcc
else
    CC="/c/Program Files/LLVM/bin/clang.exe"
fi

case "$("$CC" --version 2>&1 | head -1)" in
    *clang*) FLAVOUR=clang ;;
    *)       FLAVOUR=gcc   ;;
esac

OUT="$SCRIPT_DIR/build/gate"
LOG="$SCRIPT_DIR/build/gate.log"
mkdir -p "$OUT"; : > "$LOG"

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
# The Xbox port's SDL_* shims and xbox_respool.h are declaration-only, so shared
# game code compiles against them unchanged. Their IMPLEMENTATIONS are nxdk and
# get replaced when the 360 HAL lands; this path goes away then.
INCS="$INCS -I$DECOMP/xbox_port/include"

# Mirrors the Xbox port's suppression set: the decomp leans on gcc-permissive
# diagnostics, which clang in particular promotes to hard errors.
WARN="-Wno-implicit-function-declaration -Wno-implicit-int
      -Wno-incompatible-pointer-types -Wno-int-conversion
      -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
      -Wno-sign-compare -Wno-unused-variable -Wno-unused-function
      -Wno-missing-braces -Wno-parentheses -Wno-return-type -Wno-pointer-sign"

if [ "$FLAVOUR" = clang ]; then
    TARGETFLAGS="--target=powerpc-unknown-none-elf -ffreestanding"
    WARN="$WARN -Wno-incompatible-function-pointer-types
          -Wno-tautological-constant-out-of-range-compare"
else
    # libXenon's own MACHDEP, verbatim from $DEVKITXENON/rules.
    TARGETFLAGS="-DXENON -m32 -maltivec -fno-pic -mpowerpc64 -mhard-float"
    INCS="$INCS -I${DEVKITXENON:-/usr/local/xenon}/usr/include"
fi

collect_srcs() {
    local root="${1:-}"
    if [ -n "$root" ]; then
        find "$DECOMP/$root" -name '*.c'
        return
    fi
    # Exclusions mirror xbox_port/Makefile.nxdk: main.c and memcpy.c carry MIPS
    # register-asm bodies that no native port compiles.
    find "$DECOMP/src/main" -maxdepth 1 -name '*.c' ! -name 'main.c' ! -name 'memcpy.c'
    find "$DECOMP/src/bodyprog" -name '*.c' ! -name 'bodyprog_80032D1C.c' ! -name 'text_draw_jp.c'
    find "$DECOMP/src/screens" -name '*.c' ! -name 'hp_safe1.c' ! -name 's__safe2.c'
    # PCPORT_HAL_EXCLUDE from xbox_port/Makefile.nxdk: the pc_port sources a
    # console replaces wholesale (SDL/OpenAL/dlfcn-backed). Compiling them for
    # PPC would only ever report the HAL we have not written yet.
    local excl="main_pc pc_combat pc_console_cmd pc_crash pc_quicksave dbg_overlay
                warning_screen hires_override fs_pc dll_loader map_overlay_loader
                map_registry xa_player control_style pc_mouse_cursor combat_target
                miniz tex_pack map7_s03_boss_motion"
    local pat=""
    for f in $excl; do pat="$pat ! -name $f.c"; done
    find "$PCPORT/src" -maxdepth 1 -name '*.c' $pat
    find "$PCPORT/src/stubs" -name '*.c' ! -name 'map_overlay_stub.c'
    # Only map0_s00, matching the Xbox port's early milestones. The other 400
    # map TUs come once the overlay mechanism is proven on PPC.
    find "$DECOMP/src/maps/map0_s00" -name '*.c'
    ls "$PCPORT/build_gen/extracted_data/map0_s00_extracted_data.c" 2>/dev/null
    # Xbox HAL files that pull in NO nxdk headers. Compiled straight out of
    # xbox_port/src rather than copied, so the two console ports cannot drift.
    # The excluded ones are the genuinely nxdk-bound HAL (GPU/pad/CD/FS/audio/
    # crash/log) plus RetroAchievements, which needs the rcheevos include path.
    for f in "$DECOMP"/xbox_port/src/*.c; do
        case "$(basename "$f")" in
            crash_xbox.c|dbg_overlay_xbox.c|dsound_bridge.c|dsound_xbox.c|\
            gpu_nv2a.c|net_xbox.c|pad_xbox.c|ra_badge_xbox.c|sdl_compat_xbox.c|\
            sh_log_xbox.c|xa_xbox.c|cd_xbox.c|fs_xbox.c|main_xbox.c|\
            ra_xbox.c|\
            msvc_compat.c|\
            fmv_xbox.c) ;;
            # msvc_compat.c: _except_handler3/_fpclass, shims for nxdk's
            #   MSVC-ABI clang. xenon-gcc needs none of it.
            # fmv_xbox.c: pulls nxdk's windows.h. FMV is not on the path to a
            #   first boot; it needs a 360 decoder of its own later.
            *) echo "$f" ;;
        esac
    done
    # GTE_SRCS from xbox_port/Makefile.nxdk: the software GTE. Portable C/C++,
    # and the one piece of PsyCross a bare-metal console keeps.
    echo "$PSYCROSS/src/psx/inline_c.c"
    echo "$PSYCROSS/src/psx/libgte.c"
    echo "$PSYCROSS/src/psx/abs.c"
    echo "$PSYCROSS/src/gte/PsyX_GTE.cpp"
}

pass=0; fail=0
while IFS= read -r f; do
    [ -z "$f" ] && continue
    obj="$OUT/$(echo "${f#$DECOMP/}" | tr '/' '_').o"
    # Per-map define, mirroring the Makefile's target-specific CFLAGS. Covers
    # the shared chara/particle code the map TUs #include.
    case "$f" in
        */maps/map0_s00/*|*map0_s00_extracted_data.c) EXTRA="-DMAP0_S00 -DSH_MAP_NAME=map0_s00" ;;
        *) EXTRA="" ;;
    esac
    # PsyX_GTE.cpp is the only C++ TU in scope.
    case "$f" in
        *.cpp) TOOL="${CXX:-${CC%gcc}g++}"; [ "$FLAVOUR" = clang ] && TOOL="$CC" ;;
        *)     TOOL="$CC" ;;
    esac
    if err=$("$TOOL" $TARGETFLAGS $DEFS $INCS $WARN $EXTRA -c "$f" -o "$obj" 2>&1); then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        { echo "########## $f"; echo "$err"; } >> "$LOG"
    fi
done < <(collect_srcs "${1:-}")

echo "=========================================="
echo " PPC big-endian compile gate  [$CC]"
echo "   passed : $pass"
echo "   failed : $fail"
echo "   total  : $((pass+fail))"
echo "=========================================="
if [ "$fail" -gt 0 ]; then
    echo
    echo "Top error kinds:"
    grep -h "error:" "$LOG" | sed 's/.*error: //' \
        | sed "s/'[^']*'/'X'/g; s/\"[^\"]*\"/\"X\"/g" | sort | uniq -c | sort -rn | head -20
    echo
    echo "Files with errors:"
    grep -c "^##########" "$LOG"
    echo "Full log: $LOG"
fi
