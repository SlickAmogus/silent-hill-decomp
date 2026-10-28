# Drive Xenia with keyboard presses on a schedule, screenshotting as it goes.
#   powershell -NoProfile -ExecutionPolicy Bypass -File autoplay.ps1 -Test C:\...\xdk_test -Secs 120 -Shot 5 -Keys "30:X,36:0xBA,42:0xBA"
# Keys are the Xenia keybind names from xenia-canary.config.toml (keyboard_mode
# must be 1): X = Start, 0xBA (;) = A, 0xDE (') = B, Z = Back.
param([string]$Test, [int]$Secs = 90, [int]$Shot = 5, [string]$Keys = "", [string]$Xenia = "C:\Claude\xenia\xenia_canary.exe")
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; } }
'@
$sched = @{}
if ($Keys -ne "") { foreach ($kv in $Keys.Split(",")) { $parts = $kv.Split(":"); $sched[[int]$parts[0]] = $parts[1] } }
Get-Process xenia_canary -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item "$Test\shot_*.png","$Test\xenia.log" -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $Xenia -ArgumentList "`"$Test\default.xex`" --log_file=`"$Test\xenia.log`" --log_level=2" -PassThru
"xenia pid $($p.Id)"
function Press($k) {
  if ($k -like "0x*") { $vk = [byte][Convert]::ToInt32($k, 16) } else { $vk = [byte][char]$k.ToUpper() }
  $x = Get-Process xenia_canary -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($x -and $x.MainWindowHandle -ne 0) { [void][W]::SetForegroundWindow($x.MainWindowHandle) }
  Start-Sleep -Milliseconds 100
  [W]::keybd_event($vk, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 180
  [W]::keybd_event($vk, 0, 2, [UIntPtr]::Zero)
}
function Shot($name) {
  $x = Get-Process xenia_canary -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($x -and $x.MainWindowHandle -ne 0) {
    $r = New-Object W+RECT; [void][W]::GetWindowRect($x.MainWindowHandle, [ref]$r)
    if ($r.R -gt $r.L -and $r.B -gt $r.T) {
      $bmp = New-Object Drawing.Bitmap ($r.R-$r.L), ($r.B-$r.T)
      $g = [Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
      $bmp.Save("$Test\$name"); $g.Dispose(); $bmp.Dispose()
    }
  }
}
for ($t = 1; $t -le $Secs; $t++) {
  Start-Sleep -Seconds 1
  if ($sched.ContainsKey($t)) { "t=$t press $($sched[$t])"; Press $sched[$t] }
  if (($t % $Shot) -eq 0) { Shot ("shot_{0:D3}s.png" -f $t) }
}
Get-Process xenia_canary -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
$log = Get-ChildItem "$Test\silenthill_*.log","C:\Claude\xenia\cache\silenthill_*.log" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
"gamelog: $($log.FullName)"
