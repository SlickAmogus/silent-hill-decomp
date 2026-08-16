#!/usr/bin/env bash
# Boot a PS3 build under RPCS3 and report whether it reached a given marker.
#
# This is the port's regression harness. RPCS3 runs PSL1GHT homebrew, so unlike
# the 360 (where Xenia cannot load libXenon ELFs at all) the whole boot / crash /
# load-path loop can run on the desktop with no console attached.
#
#   ps3_port/tools/rpcs3_smoke.sh [-t SECONDS] [-m MARKER_REGEX] [-g] [EBOOT]
#
#     -t  wall-clock budget before the run is killed  (default 90)
#     -m  extended regex the run must produce to pass (default the SH-PS3 tag)
#     -g  render for real (Vulkan) instead of the Null renderer
#
# Exit status is the result: 0 = marker seen, 1 = booted but no marker,
# 2 = never reached emulation, 3 = setup problem. That makes it usable from CI
# or a git hook without parsing the output.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PS3PORT="$(cd "$SCRIPT_DIR/.." && pwd)"
RPCS3_DIR="${RPCS3_DIR:-/c/Games/PS3}"
RPCS3_EXE="$RPCS3_DIR/rpcs3.exe"

TIMEOUT=90
MARKER='\[SH-PS3\]'
GRAPHICS=0
INTERP=0
while getopts "t:m:gi" opt; do
    case "$opt" in
        t) TIMEOUT="$OPTARG" ;;
        m) MARKER="$OPTARG" ;;
        g) GRAPHICS=1 ;;
        i) INTERP=1 ;;
        *) sed -n '2,14p' "$0"; exit 3 ;;
    esac
done
shift $((OPTIND - 1))

EBOOT="${1:-$PS3PORT/build/smoke/EBOOT.BIN}"
OUT="$PS3PORT/build/smoke"
mkdir -p "$OUT"

[ -x "$RPCS3_EXE" ] || { echo "no rpcs3.exe at $RPCS3_EXE (set RPCS3_DIR)"; exit 3; }
[ -f "$EBOOT" ]     || { echo "no boot image at $EBOOT"; exit 3; }

# The config is GENERATED from the user's global one rather than shipped as a
# fixed file. Two reasons: it inherits their working setup (GPU device, firmware
# paths) so the harness does not drift from the emulator that actually runs, and
# it means we never edit the global config, which is shared with their real
# games. Only the keys that matter to an automated run are overridden.
GLOBAL_CFG="$RPCS3_DIR/config/config.yml"
SMOKE_CFG="$OUT/rpcs3_smoke.yml"
[ -f "$GLOBAL_CFG" ] || { echo "no global config at $GLOBAL_CFG"; exit 3; }

# "Music Handler: Qt" makes --headless abort outright ("Headless mode can not be
# used with this music handler"), which is what forced this override into
# existence. Null audio keeps a batch run silent and deterministic; the Null
# renderer skips GPU work entirely, which is what we want until there is an RSX
# backend to exercise -- -g switches it back on.
sed -e 's/^\( *\)Music Handler: .*/\1Music Handler: "Null"/' \
    -e "s/^\( *\)Renderer: Cubeb.*/\1Renderer: \"Null\"/" \
    "$GLOBAL_CFG" > "$SMOKE_CFG"
if [ "$GRAPHICS" -eq 0 ]; then
    sed -i 's/^\( *\)Renderer: Vulkan.*/\1Renderer: "Null"/' "$SMOKE_CFG"
fi

# -i: PPU INTERPRETER. The LLVM recompiler reports a fault PC that is only
# approximate to the compiled block, which sent a whole debugging round down
# the wrong function: three separate probes all showed valid pointers at the
# address it named. The interpreter is slower but its CIA is the instruction
# that actually faulted.
if [ "$INTERP" -eq 1 ]; then
    sed -i 's/^\( *\)PPU Decoder: .*/\1PPU Decoder: "Interpreter (precise)"/' "$SMOKE_CFG"
fi

# RPCS3 is a Windows binary: hand it Windows paths, not MSYS ones.
win() { printf '%s' "$(cd "$(dirname "$1")" && pwd -W)/$(basename "$1")"; }

RUNLOG="$OUT/rpcs3_stdout.txt"
: > "$RUNLOG"

# The live logs are under log/, NOT the RPCS3.log sitting in the emulator root --
# that one is a leftover from an older install and never updates, which is good
# for an afternoon of reading stale output if you trust the obvious path.
# TTY.log is where guest printf lands.
LOG_DIR="$RPCS3_DIR/log"
# Truncated up front so a run that produces nothing cannot be scored against the
# previous run's output.
: > "$LOG_DIR/RPCS3.log" 2>/dev/null
: > "$LOG_DIR/TTY.log"   2>/dev/null

echo "=== booting $(basename "$EBOOT") under RPCS3 (${TIMEOUT}s budget, renderer=$([ "$GRAPHICS" -eq 1 ] && echo Vulkan || echo Null))"

# Headless RPCS3 does NOT exit when the guest returns from main -- it keeps the
# emulator alive -- so waiting for the process is waiting for the full timeout,
# every run. Poll the logs instead and stop as soon as the verdict is in. That
# turns a fixed 45s per iteration into about a second.
# --headless is only legal with the Null renderer ("Headless mode can only be
# used with the Null video renderer"), so a graphics run has to fall back to
# --no-gui, which opens a real window. That is the point of -g: you are asking
# to look at something.
MODE=--headless
[ "$GRAPHICS" -eq 1 ] && MODE=--no-gui

"$RPCS3_EXE" "$MODE" --stdout \
        --config "$(win "$SMOKE_CFG")" "$(win "$EBOOT")" \
        > "$RUNLOG" 2>&1 &
pid=$!
rc=0
waited=0
while [ "$waited" -lt "$TIMEOUT" ]; do
    kill -0 "$pid" 2>/dev/null || { wait "$pid"; rc=$?; break; }
    if grep -qaE "$MARKER" "$LOG_DIR/TTY.log" 2>/dev/null; then break; fi
    # A fatal error FREEZES emulation rather than exiting, so without this the
    # run would sit out its whole budget after already having failed.
    if grep -qa "Fatal error" "$LOG_DIR/RPCS3.log" 2>/dev/null; then break; fi
    sleep 1
    waited=$((waited + 1))
done
[ "$waited" -ge "$TIMEOUT" ] && rc=124
if kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null
    sleep 1
    kill -0 "$pid" 2>/dev/null && taskkill //F //PID "$pid" >/dev/null 2>&1
fi

# Keep copies beside the build so a failing run stays readable after the next one
# has overwritten the emulator's copies.
for f in RPCS3.log TTY.log; do
    [ -f "$LOG_DIR/$f" ] && cp "$LOG_DIR/$f" "$OUT/$f"
done

strip_ansi() { sed 's/\x1b\[[0-9;]*m//g'; }
cat "$RUNLOG" "$OUT/TTY.log" "$OUT/RPCS3.log" 2>/dev/null | strip_ansi > "$OUT/combined.txt"

echo
echo "--- guest output"
{ cat "$OUT/TTY.log" 2>/dev/null | strip_ansi
  grep -aE "$MARKER" "$OUT/combined.txt"; } | grep -av '^$' | head -40
echo
echo "--- faults"
grep -aE "·F |Fatal error|Segfault|Access violation|Unimplemented|todo:|\bcaught an exception" \
     "$OUT/combined.txt" | head -20

if grep -qaE "$MARKER" "$OUT/combined.txt"; then
    echo
    echo "PASS - marker /$MARKER/ seen  (rpcs3 rc=$rc)"
    exit 0
fi
if grep -qa "emulation is running" "$OUT/combined.txt"; then
    echo
    echo "BOOTED but marker /$MARKER/ never appeared  (rpcs3 rc=$rc)"
    echo "full log: $OUT/combined.txt"
    exit 1
fi
echo
echo "NEVER REACHED EMULATION  (rpcs3 rc=$rc)"
echo "full log: $OUT/combined.txt"
exit 2
