# Boot a PS3 build under RPCS3 and SCREENSHOT the game window.
#
# The counting probes can say "N primitives emitted, bounding box B" and still
# be describing a black screen. Past a certain point the only honest check on a
# renderer is to look at it, so this launches RPCS3 windowed, waits for the game
# to settle, captures RPCS3's own window and writes a PNG.
#
#   powershell -File ps3_port/tools/rpcs3_shot.ps1 [-Eboot <path>] [-Wait 15] [-Out shot.png]
param(
    [string]$Eboot = "ps3_port/bin/EBOOT.BIN",
    [int]$Wait     = 15,
    [string]$Out   = "ps3_port/build/smoke/frame.png",
    [string]$Rpcs3 = "C:\Games\PS3\rpcs3.exe",
    [string]$Config = "ps3_port/build/smoke/rpcs3_shot.yml"
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class Win {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  // The game window is the one whose title RPCS3 stamps with the FPS counter.
  public static IntPtr FindGame() {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr p) {
      if (!IsWindowVisible(h)) return true;
      StringBuilder sb = new StringBuilder(512);
      GetWindowText(h, sb, 512);
      string t = sb.ToString();
      if (t.StartsWith("FPS:")) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
"@

$Eboot  = (Resolve-Path $Eboot).Path
$OutDir = Split-Path $Out -Parent
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force $OutDir | Out-Null }

# A config of our own, so the user's global one (shared with their real games)
# is never touched. Vulkan, because we are trying to SEE something.
$global = "C:\Games\PS3\config\config.yml"
if (Test-Path $global) {
    (Get-Content $global) -replace '^(\s*)Music Handler: .*', '$1Music Handler: "Null"' |
        Set-Content $Config -Encoding utf8
}
$Config = (Resolve-Path $Config).Path

Get-Process rpcs3 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$p = Start-Process -FilePath $Rpcs3 `
        -ArgumentList @("--no-gui", "--config", "`"$Config`"", "`"$Eboot`"") `
        -PassThru
Start-Sleep -Seconds $Wait

$h = [Win]::FindGame()
if ($h -eq [IntPtr]::Zero) {
    Write-Output "game window not found; capturing whole screen instead"
    $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $x = $b.X; $y = $b.Y; $w = $b.Width; $hgt = $b.Height
} else {
    [Win]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 400
    $r = New-Object Win+RECT
    [Win]::GetClientRect($h, [ref]$r) | Out-Null
    $pt = New-Object Win+POINT
    [Win]::ClientToScreen($h, [ref]$pt) | Out-Null
    $x = $pt.X; $y = $pt.Y; $w = $r.R - $r.L; $hgt = $r.B - $r.T
}
if ($w -lt 8 -or $hgt -lt 8) { $w = 640; $hgt = 480 }

$bmp = New-Object System.Drawing.Bitmap($w, $hgt)
$g   = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($x, $y, 0, 0, $bmp.Size)
$bmp.Save((Join-Path (Get-Location) $Out), [System.Drawing.Imaging.ImageFormat]::Png)

# A quick non-black census, so a run can be scored without opening the file.
$nonBlack = 0; $sx = [Math]::Max(1, [int]($w / 160)); $sy = [Math]::Max(1, [int]($hgt / 90))
for ($iy = 0; $iy -lt $hgt; $iy += $sy) {
  for ($ix = 0; $ix -lt $w; $ix += $sx) {
    $c = $bmp.GetPixel($ix, $iy)
    if ($c.R -gt 8 -or $c.G -gt 8 -or $c.B -gt 8) { $nonBlack++ }
  }
}
$g.Dispose(); $bmp.Dispose()

Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Get-Process rpcs3 -ErrorAction SilentlyContinue | Stop-Process -Force

Write-Output ("wrote {0}  ({1}x{2}, non-black samples: {3})" -f $Out, $w, $hgt, $nonBlack)
