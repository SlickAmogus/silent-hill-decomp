#!/usr/bin/env bash
# XDK compile gate: build every TU of the 360 game tree with the XDK's cl.exe
# (MSVC 16 for PowerPC) and hold it at zero errors.
#
# Same source list as ppc_gate.sh (the libXenon gate) minus the libXenon HAL,
# plus xdk/src. The two gates share one tree; only the HAL differs.
#
#   ./gate.sh            # whole tree
#   ./gate.sh src/main   # restrict to a subtree
#   SH_OPT=-O2 ./gate.sh # optimisation override (default -Od: fast, and it is
#                        # a compile gate, not a benchmark)
#   JOBS=4 ./gate.sh     # parallelism (default: all cores)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="$(cd "$SCRIPT_DIR/.." && pwd)"
DECOMP="$(cd "$PORT/.." && pwd)"
PCPORT="$DECOMP/pc_port"
PSYCROSS="$PCPORT/PsyCross"

. "$SCRIPT_DIR/xdk_env.sh" || exit 1

OUT="$PORT/build/xdk/gate"
LOG="$PORT/build/xdk/gate.log"
WARNLOG="$PORT/build/xdk/gate_warnings.log"
mkdir -p "$OUT"
# Not incremental, on purpose: a stale object whose source was renamed would
# otherwise keep linking (that bit the libXenon build once already).
rm -f "$OUT"/*.obj "$OUT"/*.err "$OUT"/*.warn
: > "$LOG"; : > "$WARNLOG"

w() { cygpath -w "$1"; }

# SH_XBOX_PORT rides along as in the libXenon gate: the Original Xbox port is
# the base and its gates carry the console-sized memory behaviour.
# SH_XBOX360_XDK is the new gate: XDK HAL where the libXenon one would be.
export DEFS="-D_XBOX -DSH_XBOX360_PORT -DSH_XBOX360_XDK -DSH_XBOX_PORT -DSH_PC_PORT -DVER_USA -DSKIP_ASM -DUSE_PGXP=0"

# Order matters and mirrors ppc_gate.sh: pc_port/include shadows include/.
# xbox_port/include is deliberately ABSENT (its nxdk xtl.h / memory.h shims
# would shadow the XDK's); the shims this build needs are forwarded one by one
# from xdk/include.
export INCS="-I$(w "$SCRIPT_DIR/include") -I$(w "$PORT/include") \
 -I$(w "$PCPORT/include") -I$(w "$PCPORT/include/psyq_compat") -I$(w "$PCPORT/src") \
 -I$(w "$DECOMP/include") -I$(w "$DECOMP/include/decomp") \
 -I$(w "$PSYCROSS/include/psx") -I$(w "$PSYCROSS/include")"

# -w: a gate wants errors, not the decomp's thousands of level-4 notes.
# -FI xdk_compat.h is what makes gcc spellings compile at all.
export FLAGS="-nologo -c -w -FI$(w "$SCRIPT_DIR/xdk_compat.h") ${SH_OPT:--Od}"

# For the transpile fallback in cc1.sh.
export XDK_TOOLS="$SCRIPT_DIR/tools"
export XDK_COMPAT="$SCRIPT_DIR/xdk_compat.h"
export CLANG="${CLANG:-/c/Program Files/LLVM/bin/clang.exe}"

collect_srcs() {
    local root="${1:-}"
    if [ -n "$root" ]; then
        find "$DECOMP/$root" -name '*.c'
        return
    fi
    find "$DECOMP/src/main" -maxdepth 1 -name '*.c' ! -name 'main.c' ! -name 'memcpy.c'
    find "$DECOMP/src/bodyprog" -name '*.c' ! -name 'bodyprog_80032D1C.c' ! -name 'text_draw_jp.c'
    find "$DECOMP/src/screens" -name '*.c' ! -name 'hp_safe1.c' ! -name 's__safe2.c'
    local excl="main_pc pc_combat pc_console_cmd pc_crash pc_quicksave dbg_overlay
                warning_screen hires_override fs_pc dll_loader map_overlay_loader
                map_registry xa_player control_style pc_mouse_cursor combat_target
                miniz tex_pack map7_s03_boss_motion"
    local pat=""
    for f in $excl; do pat="$pat ! -name $f.c"; done
    find "$PCPORT/src" -maxdepth 1 -name '*.c' $pat
    find "$PCPORT/src/stubs" -name '*.c' ! -name 'map_overlay_stub.c'
    find "$DECOMP/src/maps/map0_s00" -name '*.c'
    ls "$PCPORT/build_gen/extracted_data/map0_s00_extracted_data.c" 2>/dev/null
    # 360 HAL: the libXenon-bound files are replaced by xdk/src, the rest is
    # shared between the two 360 builds.
    for f in "$PORT"/src/*.c; do
        case "$(basename "$f")" in
            audio_out_xbox360.c|fs_xbox360.c|gpu_xenos.c|main_xbox360.c|\
            pad_xbox360.c|sdl_compat_xbox360.c|crash_xbox360.c|sh_log_xbox360.c|\
            stubs_xbox360.c) ;;
            *) echo "$f" ;;
        esac
    done
    find "$SCRIPT_DIR/src" \( -name '*.c' -o -name '*.cpp' \) 2>/dev/null
    for f in "$DECOMP"/xbox_port/src/*.c; do
        case "$(basename "$f")" in
            crash_xbox.c|dbg_overlay_xbox.c|dsound_bridge.c|dsound_xbox.c|\
            gpu_nv2a.c|net_xbox.c|pad_xbox.c|ra_badge_xbox.c|sdl_compat_xbox.c|\
            sh_log_xbox.c|cd_xbox.c|fs_xbox.c|main_xbox.c|\
            ra_xbox.c|msvc_compat.c|map7_s03_boss_motion.c) ;;
            # map7_s03_boss_motion.c: its sub-table aliases are gcc .set asm at
            #   offsets into one pool, which MSVC cannot express. Final-boss
            #   motion scripts; needs a C-only rewrite before map7_s03 links.
            *) echo "$f" ;;
        esac
    done
    # FMV: STR demux + MDEC decoder (fmv_xbox.c's MDEC path).
    echo "$PCPORT/src/fmv/mdec.c"
    echo "$PCPORT/src/fmv/str_demux.c"
    echo "$PSYCROSS/src/psx/inline_c.c"
    echo "$PSYCROSS/src/psx/libgte.c"
    echo "$PSYCROSS/src/psx/abs.c"
    echo "$PSYCROSS/src/gte/PsyX_GTE.cpp"
}

JOBFILE="$OUT/jobs.txt"; : > "$JOBFILE"
while IFS= read -r f; do
    [ -z "$f" ] && continue
    obj="$OUT/$(echo "${f#$DECOMP/}" | tr '/' '_' | sed 's/\.\(c\|cpp\)$/.obj/')"
    case "$f" in
        */maps/map0_s00/*|*map0_s00_extracted_data.c) extra="-DMAP0_S00 -DSH_MAP_NAME=map0_s00" ;;
        *) extra="" ;;
    esac
    echo "$f|$obj|$extra" >> "$JOBFILE"
done < <(collect_srcs "${1:-}")

xargs -P "${JOBS:-6}" -d '\n' -I{} bash "$SCRIPT_DIR/cc1.sh" "{}" < "$JOBFILE"

pass=$(ls "$OUT"/*.obj 2>/dev/null | wc -l)
fail=$(ls "$OUT"/*.err 2>/dev/null | wc -l)
for e in "$OUT"/*.err;  do [ -f "$e" ] && { echo "########## $(basename "${e%.obj.err}")"; cat "$e"; } >> "$LOG"; done
for e in "$OUT"/*.warn; do [ -f "$e" ] && { echo "########## $(basename "${e%.obj.warn}")"; cat "$e"; } >> "$WARNLOG"; done

echo "=========================================="
echo " XDK (cl.exe PowerPC) compile gate"
echo "   passed : $pass"
echo "   failed : $fail"
echo "   total  : $((pass+fail))"
echo "=========================================="
if [ "$fail" -gt 0 ]; then
    echo
    echo "Top error kinds:"
    grep -h "error C" "$LOG" | sed 's/.*error \(C[0-9]*\): /\1 /' \
        | sed "s/'[^']*'/'X'/g" | sort | uniq -c | sort -rn | head -20
    echo
    echo "Full log: $LOG"
fi
