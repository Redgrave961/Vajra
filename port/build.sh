#!/usr/bin/env bash
set -euo pipefail

port="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$port")"
jobs="$(nproc)"
lto=thin
arch=x86-64-v3
pgo=auto
elf="$root/data/dev_bdvd/PS3_GAME/USRDIR/EBOOT.elf"
build=""

while [ $# -gt 0 ]; do
    case "$1" in
        --jobs|--lto|--arch|--pgo|--elf|--build-dir)
            [ $# -ge 2 ] || { echo "missing value for $1"; exit 2; } ;;
    esac
    case "$1" in
        --jobs) jobs="$2" ;;
        --lto) lto="$2" ;;
        --arch) arch="$2" ;;
        --pgo) pgo="$2" ;;
        --elf) elf="$2" ;;
        --build-dir) build="$2" ;;
        *) echo "usage: build.sh [--jobs N] [--lto thin|full] [--arch LEVEL] [--pgo auto|gen|off] [--elf EBOOT.elf] [--build-dir DIR]"; exit 2 ;;
    esac
    shift 2
done
[ -n "$build" ] || build="$port/build-linux$([ "$pgo" = gen ] && echo -pgo || true)"
: "${VCPKG_ROOT:?set VCPKG_ROOT to a bootstrapped vcpkg checkout}"

llvm_version=23.1.6
llvm_sha256=59e14234f145927f982b50627e8bafaf2fca50151b7d973d6a0b40796ddcfe9a
cache="${XDG_CACHE_HOME:-$HOME/.cache}/asura-recomp"
llvm="$cache/LLVM-$llvm_version-Linux-X64"
if [ ! -x "$llvm/bin/clang" ]; then
    mkdir -p "$cache"
    curl -fL -o "$cache/llvm.tar.zst" "https://github.com/llvm/llvm-project/releases/download/llvmorg-$llvm_version/LLVM-$llvm_version-Linux-X64.tar.zst"
    echo "$llvm_sha256  $cache/llvm.tar.zst" | sha256sum -c -
    tar -xf "$cache/llvm.tar.zst" -C "$cache"
    rm "$cache/llvm.tar.zst"
fi
export ASURA_LLVM_BIN="$llvm/bin"

"$VCPKG_ROOT/vcpkg" install --x-manifest-root="$port" --x-install-root="$build/vcpkg_installed" --overlay-triplets="$port/triplets" \
    --triplet=x64-linux-llvm --host-triplet=x64-linux-llvm --no-print-usage

for m in libsre libresc libsre_spu; do
    [ -d "$port/recompiled_prx/$m" ] || { echo "no lifted $m: run lift_firmware.py"; exit 1; }
done

cmake -S "$port" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$port/linux.cmake" -DASURA_LLVM_BIN="$llvm/bin" \
    -DASURA_ARCH="$arch" -DASURA_LTO="$lto" -DASURA_PGO="$pgo" -DASURA_JOBS="$jobs" \
    -DPORT_SPU_INPUTS="$elf" \
    -DPS3RECOMP_PAD_SDL=ON \
    -DCMAKE_PREFIX_PATH="$build/vcpkg_installed/x64-linux-llvm" \
    -DPS3RECOMP_DIR="$root/ps3recomp" \
    -DPython3_EXECUTABLE="$root/venv/bin/python"

start=$SECONDS
status=0
cmake --build "$build" -j "$jobs" -- -k 0 || status=$?
echo "build exit $status after $(((SECONDS - start) / 60)) min"
exit $status
