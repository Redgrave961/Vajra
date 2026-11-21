import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys

port = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(port)
tools = os.path.join(root, "ps3recomp", "tools")

ap = argparse.ArgumentParser()
ap.add_argument("--elf", default=os.path.join(root, "data", "dev_bdvd", "PS3_GAME", "USRDIR", "EBOOT.elf"))
ap.add_argument("--chunks", type=int, default=200)
args = ap.parse_args()

meta = os.path.join(port, "meta")
out = os.path.join(port, "recompiled")
os.makedirs(meta, exist_ok=True)
wrap = os.path.join(meta, "wrap.json")
overrides = set()
for path in glob.glob(os.path.join(port, "*.cpp")):
    with open(path, encoding="utf-8") as f:
        overrides.update("0x" + m for m in re.findall(r"^void func_([0-9A-F]{8})\(ppu_context\* ctx\)", f.read(), re.M))
with open(wrap, "w", encoding="ascii") as f:
    json.dump(sorted(overrides), f)

subprocess.run([sys.executable, os.path.join(tools, "ppu_loader.py"), args.elf, "--output", meta], check=True)
name = os.path.splitext(os.path.basename(args.elf))[0]
shutil.rmtree(out, ignore_errors=True)
os.makedirs(out)
subprocess.run([sys.executable, os.path.join(tools, "ppu_lifter.py"), args.elf,
                "--functions", os.path.join(meta, f"{name}.functions.json"),
                "--hle-stubs", os.path.join(meta, f"{name}.imports.json"),
                "--wrap", wrap,
                "--jobs", str(os.cpu_count()), "--chunks", str(args.chunks), "--output", out], check=True)
