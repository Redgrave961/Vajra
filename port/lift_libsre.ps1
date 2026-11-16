param(
    [string]$Prx = (Join-Path (Split-Path $PSScriptRoot -Parent) "data\firmware\libsre.prx"),
    [string]$Base = "0x30000000",
    [string]$DevFlash = $(if ($env:PS3_DEV_FLASH) { $env:PS3_DEV_FLASH } else { "E:\RPCS3\dev_flash" })
)
$root = Split-Path $PSScriptRoot -Parent
$py = Join-Path $root "venv\Scripts\python.exe"
$tools = Join-Path $root "ps3recomp\tools"
$firmware = Join-Path $root "data\firmware"
$meta = Join-Path $PSScriptRoot "meta\libsre"
$out = Join-Path $PSScriptRoot "recompiled_prx\libsre"
$env:TEMP = "E:\temp"
$env:TMP = "E:\temp"

New-Item -ItemType Directory -Force $meta | Out-Null
& $py (Join-Path $tools "lift_prx.py") $Prx --base $Base --output $meta
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (Test-Path $out) { Remove-Item $out -Recurse -Force }
& $py (Join-Path $PSScriptRoot "tools\prxgen.py") $meta $out libsre
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$exp = Get-Content (Join-Path $meta "libsre_exports.json") -Raw | ConvertFrom-Json
$stubs = Get-Content (Join-Path $out "libsre_stubs.json") -Raw | ConvertFrom-Json
$lastStub = [uint32](($stubs | ForEach-Object { [Convert]::ToUInt32(($_.stub -replace '^0x', ''), 16) } | Measure-Object -Maximum).Maximum)
$codeEnd = "0x{0:X8}" -f ($lastStub + 0x20)

& $py (Join-Path $tools "ppu_lifter.py") (Join-Path $meta "libsre_image.bin") `
    --raw --base $Base --toc $exp.toc --code-end $codeEnd `
    --functions (Join-Path $meta "libsre_functions.json") `
    --hle-stubs (Join-Path $out "libsre_stubs.json") `
    --symbol-prefix libsre_ --header-name libsre_recomp.h --source-name libsre_recomp.cpp --single-file `
    --jobs ([Environment]::ProcessorCount) --output $out
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$pics = @()
foreach ($p in "flashMP3.pic", "flashATRAC.pic") {
    $dst = Join-Path $firmware $p
    $src = Join-Path $DevFlash "sys\external\$p"
    if (-not (Test-Path $dst) -and (Test-Path $src)) { Copy-Item $src $dst }
    if (Test-Path $dst) { $pics += "--pic"; $pics += $dst }
}

$spuOut = Join-Path $PSScriptRoot "recompiled_prx\libsre_spu"
if (Test-Path $spuOut) { Remove-Item $spuOut -Recurse -Force }
& $py (Join-Path $tools "lift_firmware_spu.py") --image (Join-Path $meta "libsre_image.bin") --base $Base --name libsre `
    --body 0x21480:0x2200:0xA00:0xA00:0x2110 `
    --body 0x23680:0x1E40:0xA00:0xA00:0x1C00 `
    --body 0x26300:0x3E30:0xA00:0xA00:0x3D20 `
    --body 0x2A180:0x3E30:0xA00:0xA00:0x3CF0 `
    @pics --out $spuOut
exit $LASTEXITCODE
