<p align="center">
  <img src="assets/banner.jpg" alt="Vajra — Asura's Wrath PC Recompilation" width="100%" />
</p>

<h1 align="center">
  <img src="assets/vajra.png" alt="Vajra Icon" width="44" align="absmiddle" />
  &nbsp;Vajra
</h1>

<p align="center">
  <strong>Native PC Port of <em>Asura's Wrath</em> (PS3 — BLES01227, v1.02)</strong><br/>
  Windows &amp; Linux · Vulkan · High FPS · 4K+ · Full KBM &amp; Controller · Native Trophies
</p>

---

> **Legal Notice:** Vajra contains **no** copyrighted game code or assets. You must supply your own legally owned copy of *Asura's Wrath* (`BLES01227`, update `1.02`) and PS3 firmware (`dev_flash`).

## Features

- **Native PC Performance — Up to 180+ FPS & Uncapped**
  - Runs natively on x86-64 PCs with smooth, rock-solid frame pacing and selectable in-game frame rate caps (**30 / 60 / 120 / 180 / Uncapped**).
- **True High-Resolution Rendering (1080p, 1440p, 4K & Beyond)**
  - Crisp, full-resolution 3D rendering, shadows, particles, and UI with properly scaled bloom and depth-of-field effects.
  - Windowed, Borderless Fullscreen, and Exclusive Fullscreen modes configurable right from the in-game PC Options menu.
- **Stutter-Free Vulkan Graphics**
  - Fast, non-blocking shader compilation with automatic multi-threaded startup precompilation and a tidy `cache/` folder so gameplay stays smooth from start to finish.
- **Full Keyboard & Mouse + Any Controller**
  - Instant hot-switching between **Keyboard & Mouse**, **PlayStation**, **Xbox**, and **Nintendo** button prompts — including full custom keybinding and mouse sensitivity / inversion options.
  - Tailored Keyboard & Mouse QTE prompts: stationary `WASD` prompts with directional key highlights & spin animations, plus directional mouse prompts — while automatically restoring classic analog stick animations the moment you pick up a controller.
- **Native PS3-Style Trophies**
  - Full trophy tracking and persistence, working in-game trophy icons, and an authentic **PS3 XMB-style trophy unlock popup** with trophy art, Bronze/Silver/Gold/Platinum badge, and chime.
- **Windows & Linux Support**
  - Runs natively on **Windows** (`AsurasWrath.exe`) and **Linux** (`AsurasWrath-x86_64.AppImage`, Wayland & X11, including SteamOS / Steam Deck).

---

## Quick Start

1. Place your dumped `BLES01227` disc files in `data/dev_bdvd/`, update `1.02` in `data/dev_hdd0/game/BLES01227/`, and PS3 firmware in `data/dev_flash/`.
2. Decrypt `EBOOT.BIN` with RPCS3 (`rpcs3 --decrypt data/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN`).
3. Lift and build:
   - **Lift:** `python port/lift.py` and `python port/lift_firmware.py`
   - **Windows:** `.\port\build.ps1` → `port\build\AsurasWrath.exe`
   - **Linux (from Windows):** `.\port\build.ps1 -Target linux` → `port\build-linux\AsurasWrath-x86_64.AppImage`
   - **Linux (native):** `./port/build.sh` → `port/build-linux/AsurasWrath-x86_64.AppImage`
4. Launch the game! Saves, trophies, settings, and shader cache (`cache/`) stay cleanly organized alongside the executable.

---

<p align="center">
  Built on <a href="https://github.com/sp00nznet/ps3recomp">ps3recomp</a> · Do not redistribute compiled binaries.
</p>
