#!/usr/bin/env bash
# Milestone 1 gate: compile the shared game tree for 64-bit big-endian PowerPC
# (Cell PPU, PSL1GHT).
#
# Compiles to real objects, never -fsyntax-only: a missing header makes
# -fsyntax-only report a file CLEAN, which is how the iOS port lost a day.
#
# Runs inside the ps3dev container:
#   docker run --rm -v C:\Claude\silenthill-ps3\silent-hill-decomp:/work -w /work \
#     scrapes/ps3toolchain-minimal:latest bash -lc 'bash ps3_port/ppu_gate.sh'
#
#   bash ps3_port/ppu_gate.sh            # whole tree
#   bash ps3_port/ppu_gate.sh src/main   # restrict to a subtree
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECOMP="$(cd "$SCRIPT_DIR/.." && pwd)"
PCPORT="$DECOMP/pc_port"
PSYCROSS="$PCPORT/PsyCross"

CC="${CC:-ppu-gcc}"
PS3DEV="${PS3DEV:-/usr/local/ps3dev}"

OUT="$SCRIPT_DIR/build/gate"
LOG="$SCRIPT_DIR/build/gate.log"
mkdir -p "$OUT"; : > "$LOG"

# SH_XBOX_PORT rides along, but NOT for the reason the 360 gate carries it. Its
# blocks in the four reformat walkers are 64MB heap-leak fixes (LmTrack /
# IpdTrack / deep DMS free), not a 32-bit struct-layout switch -- there is no
# such switch, both ports share one parse path. PS3 has ~213MB of usable XDR,
# far closer to Xbox's 64MB than to PC's gigabytes, so those leak fixes are
# wanted on their own merit. Nothing here implies a 32-bit host.
DEFS="-DSH_PS3_PORT -DSH_XBOX_PORT -DSH_PC_PORT -DVER_USA -DSKIP_ASM -DUSE_PGXP=0"
# PsyCross asserts every PSX primitive's size in longs. Wiring these up is the
# point, not a formality: they are the first thing that would catch PPC packing
# drift in the structs that overlay disc data.
DEFS="$DEFS -Dstatic_assert=_Static_assert"

INCS="-I$SCRIPT_DIR/include -I$PCPORT/include -I$PCPORT/include/psyq_compat -I$PCPORT/src
      -I$DECOMP/include -I$DECOMP/include/decomp
      -I$PSYCROSS/include/psx -I$PSYCROSS/include
      -I$PS3DEV/ppu/include"
# The Xbox port's SDL_* shims and xbox_respool.h are declaration-only, so shared
# game code compiles against them unchanged. Their IMPLEMENTATIONS are nxdk and
# get replaced when the PS3 HAL lands; this path goes away then.
INCS="$INCS -I$DECOMP/xbox_port/include"

# Mirrors the Xbox/360 suppression set: the decomp leans on gcc-permissive
# diagnostics.
WARN="-Wno-implicit-function-declaration -Wno-implicit-int
      -Wno-incompatible-pointer-types -Wno-int-conversion
      -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
      -Wno-sign-compare -Wno-unused-variable -Wno-unused-function
      -Wno-missing-braces -Wno-parentheses -Wno-return-type -Wno-pointer-sign"

# PSL1GHT's own MACHDEP (ppu_rules) plus the decomp's two standing needs.
# -mcpu=cell is the PPU; ppu-gcc is powerpc64-ps3-elf and emits ELF64 MSB.
TARGETFLAGS="-D__PS3__ -D__PSL1GHT__ -mcpu=cell -mhard-float
             -fmodulo-sched -ffunction-sections -fdata-sections"
# PsyCross declares helpers like fst_min/fst_max as plain `inline`. Under C99
# semantics that emits NO out-of-line definition, so they link as undefined;
# gnu89 semantics (what PC/Xbox effectively use) emit one.
TARGETFLAGS="$TARGETFLAGS -fgnu89-inline"

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
    # console replaces wholesale (SDL/OpenAL/dlfcn-backed).
    # Note map7_s03_boss_motion is NOT excluded here, unlike the Xbox/360 lists.
    # Its sh_scr is {s32, ptr, s32}: 12B on ILP32, 24B on LP64, and each port has
    # a variant whose baked pool-offset aliases stride that size. The Xbox copy
    # is the i386 one and its _Static_assert(sizeof(sh_scr)==12) fires on the
    # PPU. The 64-bit PPU wants the x64 copy pc_port generates -- the first
    # concrete case of the rule that PS3 takes pc_port's 64-bit variants rather
    # than xbox_port's 32-bit ones.
    local excl="main_pc pc_combat pc_console_cmd pc_crash pc_quicksave dbg_overlay
                warning_screen hires_override fs_pc dll_loader map_overlay_loader
                map_registry xa_player control_style pc_mouse_cursor combat_target
                miniz tex_pack"
    local pat=""
    for f in $excl; do pat="$pat ! -name $f.c"; done
    find "$PCPORT/src" -maxdepth 1 -name '*.c' $pat
    find "$PCPORT/src/stubs" -name '*.c' ! -name 'map_overlay_stub.c'
    # Only map0_s00, matching the Xbox/360 early milestones.
    find "$DECOMP/src/maps/map0_s00" -name '*.c'
    ls "$PCPORT/build_gen/extracted_data/map0_s00_extracted_data.c" 2>/dev/null
    [ -d "$SCRIPT_DIR/src" ] && find "$SCRIPT_DIR/src" -name '*.c'
    # Xbox HAL files that pull in NO nxdk headers, compiled straight out of
    # xbox_port/src rather than copied so the console ports cannot drift. Same
    # split the 360 gate uses; the excluded set is the genuinely nxdk-bound HAL
    # plus RetroAchievements (needs the rcheevos include path).
    for f in "$DECOMP"/xbox_port/src/*.c; do
        case "$(basename "$f")" in
            crash_xbox.c|dbg_overlay_xbox.c|dsound_bridge.c|dsound_xbox.c|\
            gpu_nv2a.c|net_xbox.c|pad_xbox.c|ra_badge_xbox.c|sdl_compat_xbox.c|\
            sh_log_xbox.c|xa_xbox.c|cd_xbox.c|fs_xbox.c|main_xbox.c|\
            ra_xbox.c|msvc_compat.c|fmv_xbox.c|\
            map7_s03_boss_motion.c) ;;
            *) echo "$f" ;;
        esac
    done
    # The software GTE: portable C/C++, and the one piece of PsyCross a
    # bare-metal console keeps.
    echo "$PSYCROSS/src/psx/inline_c.c"
    echo "$PSYCROSS/src/psx/libgte.c"
    echo "$PSYCROSS/src/psx/abs.c"
    echo "$PSYCROSS/src/gte/PsyX_GTE.cpp"
}

pass=0; fail=0
while IFS= read -r f; do
    [ -z "$f" ] && continue
    obj="$OUT/$(echo "${f#$DECOMP/}" | tr '/' '_').o"
    case "$f" in
        */maps/map0_s00/*|*map0_s00_extracted_data.c) EXTRA="-DMAP0_S00 -DSH_MAP_NAME=map0_s00" ;;
        *) EXTRA="" ;;
    esac
    case "$f" in
        *.cpp) TOOL="ppu-g++" ;;
        *)     TOOL="$CC" ;;
    esac
    if err=$("$TOOL" $TARGETFLAGS $DEFS $INCS $WARN $EXTRA -c "$f" -o "$obj" 2>&1); then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        # Delete the object. Without this a failed TU leaves the PREVIOUS
        # build's .o in place and the link happily succeeds using stale code --
        # the gate says "failed: 1" and you still get an EBOOT that boots,
        # which is the worst possible combination.
        rm -f "$obj"
        { echo "########## $f"; echo "$err"; } >> "$LOG"
    fi
done < <(collect_srcs "${1:-}")

echo "=========================================="
echo " PPU 64-bit big-endian compile gate  [$CC]"
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
    echo -n "Files with errors: "; grep -c "^##########" "$LOG"
    echo "Full log: $LOG"
fi
