param(
    [string]$Elf = (Join-Path (Split-Path $PSScriptRoot -Parent) "data\dev_bdvd\PS3_GAME\USRDIR\EBOOT.elf"),
    [int]$Chunks = 200
)
$root = Split-Path $PSScriptRoot -Parent
$py = Join-Path $root "venv\Scripts\python.exe"
$tools = Join-Path $root "ps3recomp\tools"
$meta = Join-Path $PSScriptRoot "meta"
$out = Join-Path $PSScriptRoot "recompiled"
$env:TEMP = "E:\temp"
$env:TMP = "E:\temp"

New-Item -ItemType Directory -Force $meta | Out-Null
& $py (Join-Path $tools "ppu_loader.py") $Elf --output $meta
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$name = [IO.Path]::GetFileNameWithoutExtension($Elf)
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null
& $py (Join-Path $tools "ppu_lifter.py") $Elf `
    --functions (Join-Path $meta "$name.functions.json") `
    --hle-stubs (Join-Path $meta "$name.imports.json") `
    --wrap (Join-Path $PSScriptRoot "wrap.json") `
    --jobs ([Environment]::ProcessorCount) --chunks $Chunks --output $out
exit $LASTEXITCODE
