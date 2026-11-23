<p align="center">
  <img src="assets/banner.jpg" alt="Vajra — Asura's Wrath PC Recompilation" width="100%" />
</p>

<h1 align="center">
  <img src="assets/vajra.png" alt="Vajra Icon" width="44" align="absmiddle" />
  &nbsp;Vajra
</h1>

<p align="center">
  <strong>Native PC Recompilation of <em>Asura's Wrath</em> (PS3 — BLES01227, v1.02)</strong><br/>
  Windows &amp; Linux · Vulkan · High FPS · 4K+ · KBM &amp; Controller · Native Trophies
</p>

---

> **Legal Notice:** Vajra contains **no** copyrighted game code or assets. You must provide your own legally owned copy of *Asura's Wrath* (`BLES01227`, update `1.02`) and PS3 firmware (`dev_flash`).

## Features

- **High FPS & Uncapped** — Display-matched frame rate caps (plus Uncapped) via the in-game PC Options menu.
- **4K+ Resolution Scaling** — Full high-res rendering, shadows, and UI with properly scaled bloom and depth-of-field; Windowed, Borderless, and Exclusive Fullscreen modes.
- **Full Keyboard & Mouse + Controller Support** — Hot-switching PlayStation, Xbox, Nintendo, and KBM prompts, custom keybinds, mouse sensitivity/invert, and dedicated KBM QTE prompts.
- **Native PS3-Style Trophies** — Persistent local unlocks, working in-game trophy icons, and a PS3 XMB-style unlock popup with icon, grade badge, and chime.
- **Windows & Linux** — Native `AsurasWrath.exe` on Windows and portable `AsurasWrath-x86_64.AppImage` on Linux (Wayland & X11, including SteamOS / Steam Deck).

## Quick Start

1. Place your dumped `BLES01227` disc files in `data/dev_bdvd/`, update `1.02` in `data/dev_hdd0/game/BLES01227/`, and PS3 firmware in `data/dev_flash/`.
2. Decrypt `EBOOT.BIN` with RPCS3 (`rpcs3 --decrypt data/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN`).
3. Lift and build:
   - **Lift:** `python port/lift.py` and `python port/lift_firmware.py`
   - **Windows:** `.\port\build.ps1` → `port\build\AsurasWrath.exe`
   - **Linux (from Windows):** `.\port\build.ps1 -Target linux` → `port\build-linux\AsurasWrath-x86_64.AppImage`
   - **Linux (native):** `./port/build.sh` → `port/build-linux/AsurasWrath-x86_64.AppImage`

---

<p align="center">
  Built on <a href="https://github.com/sp00nznet/ps3recomp">ps3recomp</a> · Do not redistribute compiled binaries.
</p>
