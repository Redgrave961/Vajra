# Asura's Wrath recomp

Static recompilation of Asura's Wrath (PS3, BLES01227, update 1.02) on [ps3recomp](https://github.com/sp00nznet/ps3recomp) (`ps3recomp/`, a subtree of upstream de4f090 plus local fixes). No game code or data is in this repository.

## Requirements

- Your own BLES01227 disc image and the official 1.02 update
- RPCS3 with PS3 firmware installed (decryption, `dev_flash`)
- clang-cl, lld-link, CMake, Ninja, Python 3.10+

## Layout

```
data/dev_bdvd/   disc: PS3_GAME/, PS3_DISC.SFB, PS3_GAME/USRDIR/EBOOT.elf (decrypted)
data/dev_hdd0/   game/BLES01227/ (1.02 update), saves
data/dev_hdd1/   system cache
port/            build, lift and run scripts
ps3recomp/       toolkit
```

## Steps

1. Copy the disc's `PS3_GAME` and `PS3_DISC.SFB` to `data/dev_bdvd`, the 1.02 update to `data/dev_hdd0/game/BLES01227`.
2. `rpcs3 --decrypt data/dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN` (writes `EBOOT.elf` beside it).
3. `python -m venv venv`, `venv\Scripts\pip install -r ps3recomp/requirements.txt`
4. `port\lift.ps1`, then `port\build.ps1` (ThinLTO, `-O3`, `-march=native`).
5. `port\run.bat` (`RPCS3_DIR` defaults to `E:\RPCS3`).
