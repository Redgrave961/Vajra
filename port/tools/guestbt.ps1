param(
    [int]$AfterSeconds = 50,
    [string]$Sp = "",
    [uint32]$StackTop = 0x0FF00000,
    [string]$Tag = "guestbt",
    [string[]]$ExtraMem = @()
)
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$a = Join-Path $root "analysis"
New-Item -ItemType Directory -Force $a | Out-Null
$cmd = Join-Path $a "$Tag.cmd"
$res = "$cmd.out"
Remove-Item $cmd, $res -ErrorAction SilentlyContinue
$env:PS3_DEBUG = $cmd
$env:WATCHDOG_AT = "$([Math]::Max(5, $AfterSeconds - 5)),$($AfterSeconds + 30)"
$p = Start-Process cmd.exe -ArgumentList "/c", "`"$root\port\run.bat`"" -RedirectStandardOutput (Join-Path $a "$Tag.log") -RedirectStandardError (Join-Path $a "$Tag.err.log") -PassThru -WindowStyle Hidden
Start-Sleep -Seconds $AfterSeconds
if (-not $Sp) {
    $line = Select-String -Path (Join-Path $a "$Tag.err.log") -Pattern "guest-tid 1 .*sp=0x([0-9A-F]+)" | Select-Object -Last 1
    $Sp = $line.Matches[0].Groups[1].Value
}
$spv = [Convert]::ToUInt32($Sp, 16)
for ($at = $spv; $at -lt $StackTop; $at += 4096) {
    $len = [Math]::Min(4096, $StackTop - $at)
    Set-Content -Path $cmd -Value ("mem {0:X8} {1}" -f $at, $len) -Encoding ascii
    $deadline = (Get-Date).AddSeconds(5)
    while ((Get-Date) -lt $deadline -and (Get-Item $cmd).Length -gt 0) { Start-Sleep -Milliseconds 100 }
    Start-Sleep -Milliseconds 300
}
foreach ($m in $ExtraMem) {
    Set-Content -Path $cmd -Value "mem $m" -Encoding ascii
    $deadline = (Get-Date).AddSeconds(5)
    while ((Get-Date) -lt $deadline -and (Get-Item $cmd).Length -gt 0) { Start-Sleep -Milliseconds 100 }
    Start-Sleep -Milliseconds 300
}
Get-Process AsuraRecomp -ErrorAction SilentlyContinue | Stop-Process -Force
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
"sp=0x$Sp"
& (Join-Path $root "venv\Scripts\python.exe") -I (Join-Path $PSScriptRoot "stackwalk.py") $res ("0x{0:X8}" -f $spv) ("0x{0:X8}" -f $StackTop) (Join-Path $root "port\meta\EBOOT.functions.json")
