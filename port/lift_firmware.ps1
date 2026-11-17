param(
    [string]$Firmware = (Join-Path (Split-Path $PSScriptRoot -Parent) "data\firmware"),
    [string]$DevFlash = $(if ($env:PS3_DEV_FLASH) { $env:PS3_DEV_FLASH } else { Join-Path $PSScriptRoot "..\data\dev_flash" })
)
$root = Split-Path $PSScriptRoot -Parent
$py = Join-Path $root "venv\Scripts\python.exe"
$tools = Join-Path $root "ps3recomp\tools"
$env:TEMP = "E:\temp"
$env:TMP = "E:\temp"

function Lift-Prx([string]$Name, [string]$Base) {
    $prx = Join-Path $Firmware "$Name.prx"
    if (-not (Test-Path $prx)) { throw "missing $prx (decrypt $Name.sprx from dev_flash\sys\external)" }
    $meta = Join-Path $PSScriptRoot "meta\$Name"
    $out = Join-Path $PSScriptRoot "recompiled_prx\$Name"
    New-Item -ItemType Directory -Force $meta | Out-Null
    & $py (Join-Path $tools "lift_prx.py") $prx --base $Base --output $meta
    if ($LASTEXITCODE -ne 0) { throw "lift_prx $Name failed" }
    if (Test-Path $out) { Remove-Item $out -Recurse -Force }
    & $py (Join-Path $PSScriptRoot "tools\prxgen.py") $meta $out $Name
    if ($LASTEXITCODE -ne 0) { throw "prxgen $Name failed" }
    $exp = Get-Content (Join-Path $meta "${Name}_exports.json") -Raw | ConvertFrom-Json
    $stubs = Get-Content (Join-Path $out "${Name}_stubs.json") -Raw | ConvertFrom-Json
    $lastStub = [uint32](($stubs | ForEach-Object { [Convert]::ToUInt32(($_.stub -replace '^0x', ''), 16) } | Measure-Object -Maximum).Maximum)
    & $py (Join-Path $tools "ppu_lifter.py") (Join-Path $meta "${Name}_image.bin") `
        --raw --base $Base --toc $exp.toc --code-end ("0x{0:X8}" -f ($lastStub + 0x20)) `
        --functions (Join-Path $meta "${Name}_functions.json") `
        --hle-stubs (Join-Path $out "${Name}_stubs.json") `
        --symbol-prefix "${Name}_" --header-name "${Name}_recomp.h" --source-name "${Name}_recomp.cpp" --single-file `
        --jobs ([Environment]::ProcessorCount) --output $out
    if ($LASTEXITCODE -ne 0) { throw "ppu_lifter $Name failed" }
}

Lift-Prx "libsre" "0x30000000"
Lift-Prx "libresc" "0x30100000"
$libsreMeta = Join-Path $PSScriptRoot "meta\libsre"

$pics = @()
foreach ($p in "flashMP3.pic", "flashATRAC.pic") {
    $dst = Join-Path $Firmware $p
    $src = Join-Path $DevFlash "sys\external\$p"
    if (-not (Test-Path $dst) -and (Test-Path $src)) { Copy-Item $src $dst }
    if (Test-Path $dst) { $pics += "--pic"; $pics += $dst }
}

$spuOut = Join-Path $PSScriptRoot "recompiled_prx\libsre_spu"
if (Test-Path $spuOut) { Remove-Item $spuOut -Recurse -Force }
& $py (Join-Path $tools "lift_firmware_spu.py") --image (Join-Path $libsreMeta "libsre_image.bin") --base 0x30000000 --name libsre `
    --body 0x21480:0x2200:0xA00:0xA00:0x2110 `
    --body 0x23680:0x1E40:0xA00:0xA00:0x1C00 `
    --body 0x26300:0x3E30:0xA00:0xA00:0x3D20 `
    --body 0x2A180:0x3E30:0xA00:0xA00:0x3CF0 `
    @pics --out $spuOut
exit $LASTEXITCODE
