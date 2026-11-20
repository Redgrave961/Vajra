param(
    [int]$Jobs = [Environment]::ProcessorCount,
    [ValidateSet("thin", "full")]
    [string]$Lto = "thin",
    [string]$Arch = "x86-64-v3",
    [ValidateSet("auto", "gen", "off")]
    [string]$Pgo = "auto",
    [string]$Elf = (Join-Path (Split-Path $PSScriptRoot -Parent) "data\dev_bdvd\PS3_GAME\USRDIR\EBOOT.elf"),
    [string]$BuildDir = ""
)
$root = Split-Path $PSScriptRoot -Parent
$port = $PSScriptRoot
$build = if ($BuildDir) { $BuildDir } elseif ($Pgo -eq "gen") { Join-Path $port "build-pgo" } else { Join-Path $port "build" }
$pgoDir = Join-Path $root "pgo"
$pgoData = Join-Path $pgoDir "asura.profdata"
$env:TEMP = "E:\temp"
$env:TMP = "E:\temp"

$clang = (Get-Command clang-cl -ErrorAction Stop).Source
$llvm = (Split-Path $clang -Parent) -replace '\\', '/'
$resource = (& $clang /clang:-print-resource-dir).Trim()

$opt = "/clang:-O3"
$cflags = "/Oi /Oy /Gy /GF $opt /DNDEBUG /GS- /Gw /clang:-march=$Arch /clang:-fno-math-errno"
$lflags = "/INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF /opt:lldlto=3 /opt:lldltocgo=3"

if ($Lto -eq "full") {
    $cflags += " /clang:-flto"
    $lflags += " /opt:lldltopartitions=$Jobs"
} else {
    $cflags += " /clang:-flto=thin"
    $lflags += " /opt:lldltojobs=$Jobs /lldltocache:$($build -replace '\\','/')/thinlto-cache /lldltocachepolicy:cache_size_bytes=8g"
}

if ($Pgo -eq "gen") {
    New-Item -ItemType Directory -Force (Join-Path $pgoDir "raw") | Out-Null
    Get-ChildItem (Join-Path $pgoDir "raw") -Filter *.profraw | Remove-Item -Force
    $cflags += " /clang:-fprofile-instr-generate=$($pgoDir -replace '\\','/')/raw/asura-%p.profraw /DASURA_PGO_GEN"
    $lflags += " `"$(Join-Path $resource 'lib\windows\clang_rt.profile-x86_64.lib')`""
} elseif ($Pgo -eq "auto") {
    $raw = @(Get-ChildItem (Join-Path $pgoDir "raw") -Filter *.profraw -ErrorAction SilentlyContinue)
    if ($raw.Count -and (-not (Test-Path $pgoData) -or ($raw | Where-Object { $_.LastWriteTime -gt (Get-Item $pgoData).LastWriteTime }))) {
        & (Join-Path $llvm "llvm-profdata.exe") merge -o $pgoData $raw.FullName
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        if (Test-Path (Join-Path $build "build.ninja")) { & cmake --build $build --target clean }
    }
    if (Test-Path $pgoData) {
        $cflags += " /clang:-fprofile-instr-use=$($pgoData -replace '\\','/') /clang:-Wno-profile-instr-unprofiled /clang:-Wno-profile-instr-out-of-date /clang:-Wno-backend-plugin"
    }
}

$vcpkg = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "E:\tools\vcpkg" }
$sdlPrefix = Join-Path $vcpkg "installed\x64-windows-static-md"
if (-not (Test-Path (Join-Path $sdlPrefix "share\sdl3"))) { "SDL3 missing: $vcpkg\vcpkg install sdl3:x64-windows-static-md"; exit 1 }

foreach ($m in "libsre", "libresc", "libsre_spu") {
    if (-not (Test-Path (Join-Path $port "recompiled_prx\$m"))) { "no lifted ${m}: run lift_firmware.ps1"; exit 1 }
}

& cmake -S $port -B $build -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `
    "-DCMAKE_LINKER=$llvm/lld-link.exe" "-DCMAKE_AR=$llvm/llvm-lib.exe" "-DCMAKE_RC_COMPILER=$llvm/llvm-rc.exe" "-DCMAKE_MT=$llvm/llvm-mt.exe" `
    "-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS" "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc" `
    "-DCMAKE_C_FLAGS_RELEASE=$cflags" "-DCMAKE_CXX_FLAGS_RELEASE=$cflags" `
    "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=$lflags" `
    "-DPS3RECOMP_OPT_FLAG=$opt" `
    "-DPORT_SPU_INPUTS=$Elf" `
    "-DPS3RECOMP_PAD_SDL=ON" `
    "-DCMAKE_PREFIX_PATH=$($sdlPrefix -replace '\\','/')" `
    "-DPS3RECOMP_DIR=$root\ps3recomp" `
    "-DPython3_EXECUTABLE=$root\venv\Scripts\python.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$sw = [Diagnostics.Stopwatch]::StartNew()
& cmake --build $build -j $Jobs -- -k 0
$rc = $LASTEXITCODE
"build exit $rc after $([int]$sw.Elapsed.TotalMinutes) min"
exit $rc
