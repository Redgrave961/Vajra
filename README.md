<p align="center">
  <img src="assets/banner.jpg" alt="Vajra: Asura's Wrath PC Recompilation" width="100%" />
</p>

<p align="center">
  <strong>An unofficial native PC port of <em>Asura's Wrath</em> (PS3)</strong><br/>
  Windows &amp; Linux · Vulkan
</p>

---

**Vajra** is a static recompilation of the PlayStation 3 version of *Asura's Wrath* (`BLES01227`, update `1.02`) to native x86-64 PC, powered by [ps3recomp](https://github.com/sp00nznet/ps3recomp). It brings the full game to Windows and Linux with native performance, modern display and audio support, full keyboard & mouse controls, and quality-of-life enhancements, all configurable directly from the in-game options menu.

## Features

- **High Framerate Support:** Play beyond the original 30 FPS cap with smooth, display-matched frame rate limits or completely uncapped framerates.
- **High-Resolution Rendering & Anti-Aliasing:** Scale past the original 720p up to 1080p, 1440p, 4K, and 8K with properly scaled shadows and effects, 16x anisotropic filtering, SMAA 1x Ultra, and linear-space supersampling downscaling.
- **Native 5.1 & 7.1 Surround Sound:** Automatic stereo, 5.1, and 7.1 LPCM output matching your OS speaker layout on Windows (WASAPI) and Linux (SDL3).
- **In-Game PC Options Menu:** Adjust resolution, display mode (Windowed, Borderless, Exclusive Fullscreen), frame rate cap, SMAA, post-processing (Motion Blur, Depth of Field, Bloom), keybindings, and mouse settings right inside the game's native menu.
- **Full Keyboard & Mouse + Controller Support:** Automatic hot-switching between Keyboard & Mouse, PlayStation, Xbox, and Nintendo button prompts, full custom keybinds, mouse sensitivity/invert controls, controller rumble, motion sensors, and tailored KBM QTE prompts.
- **Trophies & Rich Presence:** Local PS3 trophy support with in-game icons and PS3-style unlock notifications, Discord Rich Presence (showing your current Episode and Part), and Recomp Achievements integration.
- **One-Click Setup:** Reads your `BLES01227` disc folder or decrypted ISO and optional DLC `.pkg`/`.rap` files directly, downloads the official `1.02` update and PS3 firmware from Sony automatically, and decrypts `EBOOT.BIN` on first launch (no RPCS3 required). Includes `recomplauncher.json` metadata for launcher integration.
- **Windows & Linux:** Runs natively on Windows (`Vajra.exe`) and Linux (`Vajra-x86_64.AppImage`, Wayland & X11, including SteamOS / Steam Deck).

## Getting Started

1. Obtain your own legally dumped copy of *Asura's Wrath* (`BLES01227` disc folder or decrypted `.iso`), plus any optional DLC `.pkg` + `.rap` files.
2. Download the latest release and launch `Vajra.exe` on Windows or `Vajra-x86_64.AppImage` on Linux.
3. On first launch, drop or select your game folder/ISO and click **Download** for the official `1.02` update and PS3 firmware. Vajra handles decryption and setup automatically.

### Command-Line Options

When running standalone, Vajra stores saves, settings (`asura_settings.ini`), and caches (`cache/`) next to the executable (set `VAJRA_LOG=1` to write `asura.log`). You can also pass optional flags (used by RecompLauncher and custom shortcuts):

- `--data <path>` (or `RECOMPLAUNCHER_DATA`): store all writable data (saves, settings, logs, caches, user files) in `<path>`.
- `--game <path>`: path to your `BLES01227` disc folder or decrypted `.iso`.
- `--update <path>`: optional path to a local `1.02` update `.pkg`.
- `--dlc <path>`: optional DLC `.pkg` or `.rap` file (may be passed multiple times).
- `--prepare`: run windowless setup (downloads the `1.02` update and PS3 firmware if missing and verifies files).

## Building from Source

1. Place your dumped `BLES01227` disc files in `data/dev_bdvd/`, update `1.02` in `data/dev_hdd0/game/BLES01227/`, and decrypted `EBOOT.elf` alongside `EBOOT.BIN`.
2. Lift the game and firmware modules:
   ```bash
   python port/lift.py
   python port/lift_firmware.py
   ```
3. Build:
   - **Windows:** `.\port\build.ps1` -> `port\build\Vajra.exe` (+ `recomplauncher.json`)
   - **Linux (from Windows):** `.\port\build.ps1 -Target linux` -> `port\build-linux\Vajra-x86_64.AppImage` (with embedded `/recomplauncher.json`)
   - **Linux (native):** `./port/build.sh` -> `port/build-linux/Vajra-x86_64.AppImage`

## Disclaimer

This project is **not** affiliated with or endorsed by Capcom or CyberConnect2. No copyrighted game code, binaries, or assets are included in this repository. You must provide your own legally obtained copy of the game.

## License

Vajra is licensed under the [MIT License](LICENSE). See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for third-party component attributions.
