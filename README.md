<p align="center">
  <img src="assets/banner.jpg" alt="Vajra — Asura's Wrath PC Recompilation" width="100%" />
</p>

<h1 align="center">
  <img src="assets/vajra.png" alt="Vajra Icon" width="44" align="absmiddle" />
  &nbsp;Vajra
</h1>

<p align="center">
  <strong>An unofficial native PC port of <em>Asura's Wrath</em> (PS3)</strong><br/>
  Windows &amp; Linux · Vulkan
</p>

---

**Vajra** is a static recompilation of the PlayStation 3 version of *Asura's Wrath* (`BLES01227`, update `1.02`) to native x86-64 PC, powered by [ps3recomp](https://github.com/sp00nznet/ps3recomp). It brings the full game to Windows and Linux with native performance, modern display support, full keyboard & mouse controls, and quality-of-life enhancements — all configurable directly from the in-game options menu.

## Features

- **High Framerate Support** — Play beyond the original 30 FPS cap with smooth, display-matched frame rate limits or completely uncapped framerates.
- **High-Resolution Rendering** — Scale past the original 720p up to 1080p, 1440p, 4K, and beyond, with properly scaled effects, shadows, bloom, and depth of field.
- **In-Game PC Options Menu** — Adjust resolution, display mode (Windowed, Borderless, Exclusive Fullscreen), frame rate cap, controller prompt style, keybindings, and mouse settings right inside the game's native menu.
- **Full Keyboard & Mouse + Controller Support** — Seamless hot-switching between Keyboard & Mouse, PlayStation, Xbox, and Nintendo button prompts, full custom keybinds, mouse sensitivity/invert controls, and tailored KBM QTE prompts.
- **Trophies** — Full local trophy support with in-game icons and PS3-style unlock notifications.
- **Windows & Linux** — Runs natively on Windows and Linux (portable AppImage supporting Wayland and X11, including SteamOS / Steam Deck).

## Requirements

- A legally dumped copy of *Asura's Wrath* for PlayStation 3 (`BLES01227`) and official update `1.02`
- PS3 firmware (`dev_flash`) and [RPCS3](https://rpcs3.net/) to decrypt `EBOOT.BIN`
- A Vulkan-capable GPU on Windows or Linux (x86-64)

## Setup & Building

1. Place your dumped disc files in `data/dev_bdvd/`, update `1.02` in `data/dev_hdd0/game/BLES01227/`, and PS3 firmware in `data/dev_flash/`.
2. Decrypt `EBOOT.BIN` with RPCS3:
   ```bash
   rpcs3 --decrypt data/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN
   ```
3. Lift the game and firmware modules:
   ```bash
   python port/lift.py
   python port/lift_firmware.py
   ```
4. Build:
   - **Windows:** `.\port\build.ps1` → `port\build\AsurasWrath.exe`
   - **Linux (from Windows):** `.\port\build.ps1 -Target linux` → `port\build-linux\AsurasWrath-x86_64.AppImage`
   - **Linux (native):** `./port/build.sh` → `port/build-linux/AsurasWrath-x86_64.AppImage`

## Disclaimer

This project is **not** affiliated with or endorsed by Capcom or CyberConnect2. No copyrighted game code, binaries, or assets are included in this repository — you must provide your own legally obtained copy of the game, and compiled binaries must not be redistributed.

## License

Vajra is licensed under the [MIT License](LICENSE). See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for third-party component attributions.
