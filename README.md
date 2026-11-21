# Asura's Wrath recomp

Static recompilation of Asura's Wrath (PS3, BLES01227, update 1.02) on [ps3recomp](https://github.com/sp00nznet/ps3recomp) (`ps3recomp/`, a submodule: a fork of upstream with local fixes). No game code or data is in this repository.

## Requirements

- Your own BLES01227 disc image and the official 1.02 update
- RPCS3 with PS3 firmware installed (decryption, `dev_flash`)
- Windows: `E:\tools\setup.ps1` installs everything (or `build.ps1 -Tools <dir>`): [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (UCRT x86_64), vcpkg, CMake, Ninja, Python 3.10+; for the Linux target also the Linux sysroot (Debian 12 glibc 2.36 + LLVM runtimes built with llvm-mingw's clang), pkgconf and squashfs-tools-ng. No Visual Studio or Windows SDK.
- Linux (`build.sh`): a bootstrapped vcpkg in `$VCPKG_ROOT`, and on Debian/Ubuntu `cmake ninja-build python3-venv git curl zstd pkg-config squashfs-tools-ng libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxrender-dev libxtst-dev libgl-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev libfontconfig-dev libwayland-dev libxkbcommon-dev libegl-dev libdecor-0-dev`. `build.sh` downloads the pinned clang 23.1.6 release (same compiler as llvm-mingw) into `~/.cache/asura-recomp`.

## Layout

```
data/dev_bdvd/   disc: PS3_GAME/, PS3_DISC.SFB, PS3_GAME/USRDIR/EBOOT.elf (decrypted)
data/dev_hdd0/   game/BLES01227/ (1.02 update), saves
data/dev_hdd1/   system cache
data/dev_flash/  PS3 firmware dev_flash (copy from an RPCS3 install)
data/firmware/   libsre.prx, libresc.prx (decrypted from dev_flash/sys/external)
port/            port sources, lift and build scripts
ps3recomp/       toolkit
```

## Steps

1. Copy the disc's `PS3_GAME` and `PS3_DISC.SFB` to `data/dev_bdvd`, the 1.02 update to `data/dev_hdd0/game/BLES01227`.
2. `rpcs3 --decrypt data/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN` (writes `EBOOT.elf` beside it).
3. `python -m venv venv`, `venv/Scripts/pip` (Linux: `venv/bin/pip`) `install -r ps3recomp/requirements.txt`
4. `venv/Scripts/python port/lift.py` and `port/lift_firmware.py` (Linux: `venv/bin/python`).
5. Build: ThinLTO, `-O3`, `-march=x86-64-v3`, PGO from `pgo/asura.profdata` when present (`-Pgo gen` / `--pgo gen` builds an instrumented binary).
   - Windows: `port\build.ps1` → `port\build\AsurasWrath.exe` (static).
   - Linux from Windows: `port\build.ps1 -Target linux` → `port\build-linux\AsurasWrath-x86_64.AppImage`.
   - Linux: `port/build.sh` → `port/build-linux/AsurasWrath-x86_64.AppImage`. It links against the build machine's glibc; the Windows-built AppImage runs on glibc 2.36+ (Debian 12, Ubuntu 23.04, SteamOS 3.5 and newer).
6. Run the exe or the AppImage (`chmod +x` it after copying from Windows). Game paths, `user/` (saves, firmware), settings, the shader cache and `asura.log` live next to the exe or the `.AppImage`. The AppImage needs only glibc, a Wayland or X11 display and a Vulkan driver; it contains translated game code, so keep it to yourself.
