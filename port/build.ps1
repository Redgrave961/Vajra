param(
    [ValidateSet("windows", "linux")]
    [string]$Target = "windows",
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
$linux = $Target -eq "linux"
$build = if ($BuildDir) { $BuildDir } else { Join-Path $port ("build" + $(if ($linux) { "-linux" } else { "" }) + $(if ($Pgo -eq "gen") { "-pgo" } else { "" })) }
if (Test-Path "E:\temp") {
    $env:TEMP = "E:\temp"
    $env:TMP = "E:\temp"
}

$mingw = Join-Path $Tools "llvm-mingw"
$bin = (Join-Path $mingw "bin") -replace '\\', '/'
$host_triplet = "x64-llvm-mingw"
if ($linux) {
    $triplet = "x64-linux-llvm"
    $sysroot = (Join-Path $Tools "linux-sysroot") -replace '\\', '/'
    $env:ASURA_LLVM_BIN = $bin
    $env:ASURA_SYSROOT = $sysroot
    $env:ASURA_SYSROOT_VERSION = [IO.File]::ReadAllText("$sysroot/.version")
    $env:PKG_CONFIG = Join-Path $Tools "pkgconf\bin\pkgconf.exe"
    $env:PKG_CONFIG_SYSROOT_DIR = $sysroot
    $env:PKG_CONFIG_LIBDIR = "$sysroot/usr/lib/x86_64-linux-gnu/pkgconfig;$sysroot/usr/share/pkgconfig"
    $env:PKG_CONFIG_DONT_DEFINE_PREFIX = "1"
    $env:ASURA_WAYLAND_SCANNER = (Join-Path $Tools "wayland-scanner\bin\wayland-scanner.exe") -replace '\\', '/'
    $toolchain = @("-DCMAKE_TOOLCHAIN_FILE=$port\linux.cmake", "-DASURA_LLVM_BIN=$bin", "-DASURA_SYSROOT=$sysroot", "-DASURA_GENSQUASHFS=$Tools\squashfs-tools-ng\bin\gensquashfs.exe")
} else {
    $triplet = $host_triplet
    $toolchain = @("-DCMAKE_C_COMPILER=$bin/x86_64-w64-mingw32-clang.exe", "-DCMAKE_CXX_COMPILER=$bin/x86_64-w64-mingw32-clang++.exe", "-DCMAKE_RC_COMPILER=$bin/x86_64-w64-mingw32-windres.exe")
}
$installed = Join-Path $build "vcpkg_installed"

$env:LLVM_MINGW = $mingw
& (Join-Path $Tools "vcpkg\vcpkg.exe") install --x-manifest-root=$port --x-install-root=$installed --overlay-triplets=$(Join-Path $port "triplets") --triplet=$triplet --host-triplet=$host_triplet --no-print-usage
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

foreach ($m in "libsre", "libresc", "libsre_spu") {
    if (-not (Test-Path (Join-Path $port "recompiled_prx\$m"))) { "no lifted ${m}: run lift_firmware.py"; exit 1 }
}

& cmake -S $port -B $build -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    @toolchain `
    "-DASURA_ARCH=$Arch" "-DASURA_LTO=$Lto" "-DASURA_PGO=$Pgo" "-DASURA_JOBS=$Jobs" `
    "-DPORT_SPU_INPUTS=$Elf" `
    "-DPS3RECOMP_PAD_SDL=ON" `
    "-DCMAKE_PREFIX_PATH=$($installed -replace '\\','/')/$triplet" `
    "-DPS3RECOMP_DIR=$root\ps3recomp" `
    "-DPython3_EXECUTABLE=$root\venv\Scripts\python.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$sw = [Diagnostics.Stopwatch]::StartNew()
& cmake --build $build -j $Jobs -- -k 0
$rc = $LASTEXITCODE
"build exit $rc after $([int]$sw.Elapsed.TotalMinutes) min"
exit $rc
