#!/usr/bin/env bash
# lp64-smoke.sh — headless LP64 runtime smoke test.
#
# WHY: the invisible Air Screamer and the black inventory items are LP64 bugs —
# they compile clean and only fail at RUNTIME on 64-bit Linux/Steam Deck, so a
# Windows/LLP64-only test never sees them. This drives the built Linux binary
# headlessly and fails if it crashes or a known scene misbehaves.
#
# Two layers of protection, this is the second one:
#   1. COMPILE-TIME (always on, even in public CI with no game data): the
#      _Static_assert(offsetof(GsCOORDINATE2, coord) == 4) in
#      pc_port/include/psyq/libgs.h — any LP64 layout regression fails the build.
#   2. RUNTIME (this script): needs the disc image + a save, so it SKIPS cleanly
#      (exit 0) when they are absent. Runs for real locally or on a self-hosted
#      runner that has game data. Deps: Xvfb, xdotool, ffmpeg.
#
# Run from pc_port/ (or anywhere — it locates its own build/). Exit 0 = pass/skip,
# non-zero = a real LP64 runtime failure.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$HERE/build"
DISPLAY_NUM=":99"

skip() { echo "LP64 SMOKE SKIP: $1"; exit 0; }
fail() { echo "LP64 SMOKE FAIL: $1"; exit 1; }

[ -x "$BUILD/SilentHillPC" ] || skip "no built binary at $BUILD/SilentHillPC"
# Runtime needs the disc image (gitignored, never in public CI) and a save file.
ls "$BUILD"/gamedata/*.bin >/dev/null 2>&1 || skip "no disc image in build/gamedata/ (public CI has none)"
ls "$BUILD"/gamedata/save/*.MCD >/dev/null 2>&1 || skip "no save file in build/gamedata/save/"
for dep in Xvfb xdotool ffmpeg; do command -v "$dep" >/dev/null 2>&1 || skip "missing dep: $dep"; done

cleanup() { pkill -x SilentHillPC 2>/dev/null; pkill -f "Xvfb $DISPLAY_NUM" 2>/dev/null; }
trap cleanup EXIT

# Virtual display (SDL needs a window even headless — a null window is a real
# crash mode, not a false negative). Windowed 960x720 fits the 1280x800 Xvfb;
# fullscreen 1080p would fail SDL init on it.
pkill -f "Xvfb $DISPLAY_NUM" 2>/dev/null; sleep 1
Xvfb "$DISPLAY_NUM" -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 & disown
sleep 2
sed -i -e 's/^fullscreen = .*/fullscreen = 0/' -e 's/^width = .*/width = 960/' \
       -e 's/^height = .*/height = 720/' "$BUILD/config.cfg"

( cd "$BUILD" && DISPLAY="$DISPLAY_NUM" ALSOFT_DRIVERS=null SDL_AUDIODRIVER=dummy \
    LIBGL_ALWAYS_SOFTWARE=1 SDL_VIDEODRIVER=x11 ./SilentHillPC ) >/dev/null 2>&1 &
GAME_PID=$!
sleep 15

LOG="$(ls -t "$BUILD"/SilentHill_*.log 2>/dev/null | head -1)"
# Boot-level assertions (catch struct-layout / init regressions).
kill -0 "$GAME_PID" 2>/dev/null || fail "process died during boot (segfault? SDL init?)"
[ -n "$LOG" ] || fail "no game log produced"
grep -qa "Failed to initialise SDL" "$LOG" && fail "SDL init failed"
grep -qai "segmentation\|abort" "$LOG" && fail "crash in boot log"

# Scene-level: load the save, re-arm + run the Air Screamer fly-by via the dev
# console, and assert the event runs without crashing. (v2: capture a frame here
# and diff against a golden image to catch SILENT render regressions — e.g. the
# invisible bird or the black inventory items — not just crashes.)
W="$(DISPLAY="$DISPLAY_NUM" xdotool search --name 'Silent Hill' | head -1)" || fail "no window"
[ -n "$W" ] || fail "game window not found"
key() { DISPLAY="$DISPLAY_NUM" xdotool keydown --window "$W" "$1"; sleep 0.3;
        DISPLAY="$DISPLAY_NUM" xdotool keyup --window "$W" "$1"; sleep "${2:-2}"; }

key Return 4          # attract FMV -> title menu
key Return 3          # LOAD -> load screen
key c 2.5            # select saved file
key c 8             # confirm -> in-game
grep -qa "Active map: map0_s01" "$LOG" || fail "did not reach the café (map0_s01)"

key grave 0.8                                                    # open console
DISPLAY="$DISPLAY_NUM" xdotool type --window "$W" --delay 150 "setflag 42 0"; sleep 0.5
key Return 0.5; key grave 5                                      # submit, close, run fly-by

kill -0 "$GAME_PID" 2>/dev/null || fail "crashed during café / Air Screamer fly-by"
grep -qa "Active map: map0_s01" "$LOG" || fail "left the café unexpectedly"

echo "LP64 SMOKE PASS: booted, reached café, ran the Air Screamer fly-by, no crash"
exit 0
