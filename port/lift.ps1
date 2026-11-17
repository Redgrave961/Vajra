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
$wrap = Join-Path $meta "wrap.json"
$overrides = Select-String -Path (Join-Path $PSScriptRoot "*.cpp") -Pattern '^void func_([0-9A-F]{8})\(ppu_context\* ctx\)' | ForEach-Object { """0x$($_.Matches[0].Groups[1].Value)""" } | Sort-Object -Unique
"[$($overrides -join ', ')]" | Set-Content -Encoding ascii $wrap
& $py (Join-Path $tools "ppu_loader.py") $Elf --output $meta
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$name = [IO.Path]::GetFileNameWithoutExtension($Elf)
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
New-Item -ItemType Directory -Force $out | Out-Null
& $py (Join-Path $tools "ppu_lifter.py") $Elf `
    --functions (Join-Path $meta "$name.functions.json") `
    --hle-stubs (Join-Path $meta "$name.imports.json") `
    --wrap $wrap `
    --jobs ([Environment]::ProcessorCount) --chunks $Chunks --output $out
exit $LASTEXITCODE
