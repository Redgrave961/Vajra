param(
    [int]$Seconds = 120,
    [int[]]$ShotsAt = @(15, 30, 45, 60, 90, 115),
    [string]$Tag = "boot2",
    [long]$LogCapBytes = 200MB
)
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$a = Join-Path $root "analysis"
$out = Join-Path $a "$Tag.log"
$err = Join-Path $a "$Tag.err.log"
$shots = Join-Path $a "$Tag-shots"
New-Item -ItemType Directory -Force $shots | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@

$p = Start-Process cmd.exe -ArgumentList "/c", "`"$root\port\run.bat`"" -RedirectStandardOutput $out -RedirectStandardError $err -PassThru -WindowStyle Hidden
$sw = [Diagnostics.Stopwatch]::StartNew()
$reason = "timeout"
$next = 0
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    Start-Sleep -Milliseconds 500
    $game = Get-Process AsuraRecomp -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p.HasExited -and -not $game) { $reason = "exited with code $($p.ExitCode)"; break }
    if (((Get-Item $out -ErrorAction SilentlyContinue).Length + (Get-Item $err -ErrorAction SilentlyContinue).Length) -gt $LogCapBytes) { $reason = "log cap"; break }
    if ($next -lt $ShotsAt.Count -and $sw.Elapsed.TotalSeconds -ge $ShotsAt[$next]) {
        if ($game -and $game.MainWindowHandle -ne 0) {
            $r = New-Object Win+RECT
            [void][Win]::GetClientRect($game.MainWindowHandle, [ref]$r)
            $w = [Math]::Max(1, $r.R - $r.L); $h = [Math]::Max(1, $r.B - $r.T)
            $bmp = New-Object System.Drawing.Bitmap $w, $h
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            $hdc = $g.GetHdc()
            [void][Win]::PrintWindow($game.MainWindowHandle, $hdc, 3)
            $g.ReleaseHdc($hdc); $g.Dispose()
            $bmp.Save((Join-Path $shots ("t{0:D3}s.png" -f $ShotsAt[$next])), [System.Drawing.Imaging.ImageFormat]::Png)
            $bmp.Dispose()
        }
        $next++
    }
}
Get-Process AsuraRecomp -ErrorAction SilentlyContinue | Stop-Process -Force
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
"ended after {0:N0}s: {1}" -f $sw.Elapsed.TotalSeconds, $reason
"stdout {0:N0} KB, stderr {1:N0} KB, shots: {2}" -f ((Get-Item $out).Length / 1KB), ((Get-Item $err).Length / 1KB), ((Get-ChildItem $shots -Filter *.png | ForEach-Object Name) -join ", ")
