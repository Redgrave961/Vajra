import os
import sys

runtime, squashfs, target = sys.argv[1:4]
with open(target, "wb") as out:
    for part in (runtime, squashfs):
        with open(part, "rb") as f:
            out.write(f.read())
os.remove(squashfs)
os.chmod(target, 0o755)