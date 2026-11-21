param(
    [int]$Jobs = [Environment]::ProcessorCount,
    [ValidateSet("thin", "full")]
    [string]$Lto = "thin",
    [string]$Arch = "x86-64-v3",
    [ValidateSet("auto", "gen", "off")]
    [string]$Pgo = "auto",
    [string]$Elf = (Join-Path (Split-Path $PSScriptRoot -Parent) "data\dev_bdvd\PS3_GAME\USRDIR\EBOOT.elf"),
    [string]$BuildDir = "",
    [string]$Tools = "E:\tools"
)
$root = Split-Path $PSScriptRoot -Parent
$port = $PSScriptRoot
$build = if ($BuildDir) { $BuildDir } elseif ($Pgo -eq "gen") { Join-Path $port "build-pgo" } else { Join-Path $port "build" }
$pgoDir = Join-Path $root "pgo"
$pgoData = Join-Path $pgoDir "asura.profdata"
$env:TEMP = "E:\temp"
$env:TMP = "E:\temp"

$mingw = Join-Path $Tools "llvm-mingw"
$bin = (Join-Path $mingw "bin") -replace '\\', '/'
$triplet = "x64-llvm-mingw"

$opt = "-O3"
$cflags = "$opt -DNDEBUG -march=$Arch -fno-math-errno -fno-emulated-tls -ffunction-sections -fdata-sections"
$lflags = "-Wl,--gc-sections -Wl,--icf=all -Wl,--lto-O3 -Wl,--lto-CGO3 -Wl,--pdb= -Wl,-s"
$pgoNames = "-mllvm -static-func-full-module-prefix=false"
$pgoLink = "-Wl,-mllvm,-static-func-full-module-prefix=false"

if ($Lto -eq "full") {
    $cflags += " -flto"
    $lflags += " -Wl,--Xlink=-opt:lldltopartitions=$Jobs"
} else {
    $cflags += " -flto=thin"
    $lflags += " -Wl,--thinlto-jobs=$Jobs -Wl,--thinlto-cache-dir=$($build -replace '\\','/')/thinlto-cache -Wl,--thinlto-cache-policy=cache_size_bytes=8g"
}

if ($Pgo -eq "gen") {
    New-Item -ItemType Directory -Force (Join-Path $pgoDir "raw") | Out-Null
    Get-ChildItem (Join-Path $pgoDir "raw") -Filter *.profraw | Remove-Item -Force
    $cflags += " -fprofile-instr-generate=$($pgoDir -replace '\\','/')/raw/asura-%p.profraw -DASURA_PGO_GEN $pgoNames"
    $lflags += " $pgoLink"
} elseif ($Pgo -eq "auto") {
    $raw = @(Get-ChildItem (Join-Path $pgoDir "raw") -Filter *.profraw -ErrorAction SilentlyContinue)
    if ($raw.Count -and (-not (Test-Path $pgoData) -or ($raw | Where-Object { $_.LastWriteTime -gt (Get-Item $pgoData).LastWriteTime }))) {
        & "$bin/llvm-profdata.exe" merge -o $pgoData $raw.FullName
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        if (Test-Path (Join-Path $build "build.ninja")) { & cmake --build $build --target clean }
    }
    if (Test-Path $pgoData) {
        $cflags += " -fprofile-instr-use=$($pgoData -replace '\\','/') -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin $pgoNames"
        $lflags += " $pgoLink"
    }
}

$env:LLVM_MINGW = $mingw
& (Join-Path $Tools "vcpkg\vcpkg.exe") install --x-manifest-root=$port --overlay-triplets=$(Join-Path $port "triplets") --triplet=$triplet --host-triplet=$triplet --no-print-usage
$rc = $LASTEXITCODE
Remove-Item Env:LLVM_MINGW
if ($rc -ne 0) { exit $rc }

foreach ($m in "libsre", "libresc", "libsre_spu") {
    if (-not (Test-Path (Join-Path $port "recompiled_prx\$m"))) { "no lifted ${m}: run lift_firmware.ps1"; exit 1 }
}

& cmake -S $port -B $build -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_C_COMPILER=$bin/x86_64-w64-mingw32-clang.exe" "-DCMAKE_CXX_COMPILER=$bin/x86_64-w64-mingw32-clang++.exe" "-DCMAKE_RC_COMPILER=$bin/x86_64-w64-mingw32-windres.exe" `
    "-DCMAKE_C_FLAGS_RELEASE=$cflags" "-DCMAKE_CXX_FLAGS_RELEASE=$cflags" `
    "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=$lflags" `
    "-DPS3RECOMP_OPT_FLAG=$opt" `
    "-DPORT_SPU_INPUTS=$Elf" `
    "-DPS3RECOMP_PAD_SDL=ON" `
    "-DCMAKE_PREFIX_PATH=$($port -replace '\\','/')/vcpkg_installed/$triplet" `
    "-DPS3RECOMP_DIR=$root\ps3recomp" `
    "-DPython3_EXECUTABLE=$root\venv\Scripts\python.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$sw = [Diagnostics.Stopwatch]::StartNew()
& cmake --build $build -j $Jobs -- -k 0
$rc = $LASTEXITCODE
"build exit $rc after $([int]$sw.Elapsed.TotalMinutes) min"
exit $rc
