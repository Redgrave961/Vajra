<p align="center">
  <img src="assets/banner.jpg" alt="Vajra — Asura's Wrath PC Recompilation" width="100%" />
</p>

<h1 align="center">
  <img src="assets/vajra.png" alt="Vajra Icon" width="44" align="absmiddle" />
  &nbsp;Vajra
</h1>

<p align="center">
  <strong>Native PC Static Recompilation of <em>Asura's Wrath</em> (PS3 — BLES01227, v1.02)</strong><br/>
  Built on <a href="https://github.com/sp00nznet/ps3recomp">ps3recomp</a> · Windows &amp; Linux (AppImage) · Vulkan
</p>

---

> **Legal Notice:** This repository contains **zero** copyrighted game code, binaries, or assets. You must provide your own legally dumped copy of *Asura's Wrath* (`BLES01227`), official update `1.02`, and PS3 firmware (`dev_flash`).

## Core Features

- **High-Framerate Gameplay (Up to 180+ FPS & Uncapped)**
  - Native x86-64 static recompilation of both PPU and SPU code (`-O3`, `-march=x86-64-v3`, ThinLTO, PGO) with native C++ fast paths for hot engine, PhysX, and audio paths.
  - Smooth high-FPS pacing with selectable frame rate caps (30 / 60 / 120 / 180 / Uncapped) directly from the in-game PC Options menu.
- **High-Resolution Vulkan Rendering (1080p / 1440p / 4K+)**
  - Full resolution scaling for the 3D scene, shadows, particles, and UI, paired with clean box-downscaled native bloom and depth-of-field passes so post-processing halos remain smooth without grid artifacts at 4K+.
  - Non-blocking asynchronous shader compilation with `VK_EXT_graphics_pipeline_library` fast-linking, parallel multi-threaded startup precompilation, and a clean `cache/` folder (`vk_pipelines.bin` + `vk_shaders.db`).
- **Full Keyboard & Mouse + Multi-Controller Support**
  - Seamless hot-switching between **Keyboard & Mouse**, **PlayStation**, **Xbox**, and **Nintendo** button prompts with custom keybinding support.
  - Purpose-built KBM QTE prompts: stationary centered `WASD` cluster with directional key highlighting and rotating spin animations for left-stick QTEs, directional mouse movement prompts for right-stick QTEs, and automatic restoration of full analog stick animations when switching back to a controller.
- **Native PS3-Style Trophy Support**
  - Full `TROPHY.TRP` parsing, persistent local unlocks, in-game trophy icon loading (`sceNpTrophyGetTrophyIcon`), and an authentic cross-platform PS3 XMB-style top-right trophy unlock popup with icon, Bronze/Silver/Gold/Platinum badge, and chime.
- **Cross-Platform (Windows & Linux)**
  - Produces a standalone static `AsurasWrath.exe` on Windows and a portable `AsurasWrath-x86_64.AppImage` for Linux (glibc 2.36+, Wayland/X11, Vulkan).

---

## Requirements

- Your own `BLES01227` disc dump and official `1.02` update
- [RPCS3](https://rpcs3.net/) with PS3 firmware installed (for `EBOOT.BIN` decryption and `dev_flash`)
- **Windows:** `E:\tools\setup.ps1` (or `port\build.ps1 -Tools <dir>`) installs [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (UCRT x86_64), vcpkg, CMake, Ninja, Python 3.10+; for cross-compiling the Linux AppImage from Windows, also the Linux sysroot (Debian 12 glibc 2.36 + LLVM runtimes), `pkgconf`, and `squashfs-tools-ng`. No Visual Studio or Windows SDK required.
- **Linux (`port/build.sh`):** a bootstrapped vcpkg in `$VCPKG_ROOT`, and on Debian/Ubuntu:
  ```bash
  sudo apt install cmake ninja-build python3-venv git curl zstd pkg-config squashfs-tools-ng \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev \
    libxrender-dev libxtst-dev libgl-dev libasound2-dev libpulse-dev libpipewire-0.3-dev \
    libudev-dev libdbus-1-dev libfontconfig-dev libwayland-dev libxkbcommon-dev libegl-dev libdecor-0-dev
  ```
  `port/build.sh` automatically fetches the pinned Clang 23.1.6 toolchain into `~/.cache/asura-recomp`.

---

## Directory Layout

```text
assets/          repository media (README banner)
data/dev_bdvd/   disc files: PS3_GAME/, PS3_DISC.SFB, PS3_GAME/USRDIR/EBOOT.elf (decrypted)
data/dev_hdd0/   game/BLES01227/ (1.02 update), saves
data/dev_hdd1/   system cache
data/dev_flash/  PS3 firmware dev_flash (from RPCS3)
data/firmware/   libsre.prx, libresc.prx (decrypted from dev_flash/sys/external)
port/            Vajra port sources, PPU/SPU lift scripts, Windows & Linux build scripts
ps3recomp/       ps3recomp runtime & lifter submodule
```

---

## Building

1. **Prepare game files**
   - Copy the disc's `PS3_GAME` and `PS3_DISC.SFB` into `data/dev_bdvd/`.
   - Copy the `1.02` update into `data/dev_hdd0/game/BLES01227/`.
   - Decrypt the executable:
     ```bash
     rpcs3 --decrypt data/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN
     ```
     (Writes `EBOOT.elf` beside `EBOOT.BIN`.)
2. **Set up Python environment**
   ```bash
   python -m venv venv
   venv/Scripts/pip install -r ps3recomp/requirements.txt   # Linux: venv/bin/pip
   ```
3. **Lift PPU & firmware modules**
   ```bash
   venv/Scripts/python port/lift.py            # Linux: venv/bin/python
   venv/Scripts/python port/lift_firmware.py
   ```
4. **Compile** (ThinLTO, `-O3`, `-march=x86-64-v3`, automatic PGO from `pgo/asura.profdata` when present; pass `-Pgo gen` / `--pgo gen` to build an instrumented binary):
   - **Windows:**
     ```powershell
     .\port\build.ps1
     ```
     Output: `port\build\AsurasWrath.exe`
   - **Linux AppImage (cross-compiled from Windows):**
     ```powershell
     .\port\build.ps1 -Target linux
     ```
     Output: `port\build-linux\AsurasWrath-x86_64.AppImage`
   - **Linux (native):**
     ```bash
     ./port/build.sh
     ```
     Output: `port/build-linux/AsurasWrath-x86_64.AppImage`

5. **Run**
   - Launch `AsurasWrath.exe` (or `chmod +x AsurasWrath-x86_64.AppImage && ./AsurasWrath-x86_64.AppImage`).
   - User data (`user/` saves, trophies, firmware), `asura_settings.ini`, `cache/` (`vk_pipelines.bin`, `vk_shaders.db`), and `asura.log` are stored cleanly alongside the executable.
   - **Do not redistribute compiled binaries**, as they contain translated game code.
