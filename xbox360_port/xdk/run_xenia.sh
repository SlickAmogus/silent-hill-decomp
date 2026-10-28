#!/usr/bin/env bash
# Run the XDK build under Xenia Canary for a while, then collect the evidence:
# a screenshot of the window, the game's own log, and Xenia's log.
#
#   bash xbox360_port/xdk/run_xenia.sh [seconds] [test-dir]
#
# The test dir holds default.xex and silenthill\<disc>.bin, the same layout a
# USB stick gets. Xenia maps the .xex's folder as game:\, which is exactly
# where the port looks, so one folder serves both the emulator and the stick.
set -u
SECS="${1:-40}"
TEST="${2:-/c/Claude/silenthill-xbox360/xdk_test}"
XENIA="${XENIA:-/c/Claude/xenia/xenia_canary.exe}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
w() { cygpath -w "$1"; }

cp "$SCRIPT_DIR/bin/default.xex" "$TEST/default.xex" || exit 1
rm -f "$TEST/xenia.log" "$TEST/screenshot.png"
before=$(ls "$TEST"/silenthill_*.log 2>/dev/null | wc -l)

powershell -NoProfile -Command "Get-Process xenia_canary -ErrorAction SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1
"$XENIA" "$(w "$TEST/default.xex")" --log_file="$(w "$TEST/xenia.log")" --log_level=2 >/dev/null 2>&1 &
xpid=$!
echo "xenia pid $xpid, running ${SECS}s..."

# Screenshot of the window's screen rectangle (the window must be visible),
# every SHOT_EVERY seconds (default: once, at the end).
shot() {
powershell -NoProfile -ExecutionPolicy Bypass -Command "
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public class W { [DllImport(\"user32.dll\")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; } }
'@
\$p = Get-Process xenia_canary -ErrorAction SilentlyContinue | Select-Object -First 1
if (\$p -and \$p.MainWindowHandle -ne 0) {
  \$r = New-Object W+RECT; [void][W]::GetWindowRect(\$p.MainWindowHandle, [ref]\$r)
  \$bmp = New-Object Drawing.Bitmap (\$r.R-\$r.L), (\$r.B-\$r.T)
  \$g = [Drawing.Graphics]::FromImage(\$bmp); \$g.CopyFromScreen(\$r.L, \$r.T, 0, 0, \$bmp.Size)
  \$bmp.Save('$(w "$TEST/$1")'); 'screenshot $1'
} else { 'no xenia window' }
" 2>&1 | tail -1
}
rm -f "$TEST"/shot_*.png
every="${SHOT_EVERY:-$SECS}"
elapsed=0; n=0
while [ "$elapsed" -lt "$SECS" ]; do
    step=$(( SECS - elapsed < every ? SECS - elapsed : every ))
    sleep "$step"; elapsed=$((elapsed + step)); n=$((n + 1))
    shot "shot_$(printf '%02d' $n)_${elapsed}s.png"
done
cp "$TEST/shot_$(printf '%02d' $n)_${elapsed}s.png" "$TEST/screenshot.png" 2>/dev/null

powershell -NoProfile -Command "Get-Process xenia_canary -ErrorAction SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1
sleep 1

echo
echo "=== game log ==="
# Xenia's game folder is read-only, so the port writes to its cache mount.
CACHE="$(dirname "$XENIA")/cache"
latest=$(ls -t "$TEST"/silenthill_*.log "$CACHE"/silenthill_*.log 2>/dev/null | head -1)
if [ -n "$latest" ] && [ "$(ls "$TEST"/silenthill_*.log "$CACHE"/silenthill_*.log 2>/dev/null | wc -l)" -gt "$before" ]; then
    echo "$latest ($(wc -l < "$latest") lines)"
    head -60 "$latest"
    echo "..."
    tail -25 "$latest"
else
    echo "NO NEW GAME LOG - the xex did not get as far as opening one"
fi
echo
echo "=== xenia.log: errors / unimplemented / crash ==="
grep -aE "^(!>|e>)|[Uu]nimplemented|[Cc]rash|Exception|XLaunchNewImage|Launching|failed" "$TEST/xenia.log" 2>/dev/null | head -30
