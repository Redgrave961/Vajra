import bisect
import json
import re
import sys

dump, sp, top, funcs_path = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16), sys.argv[4]
mem = {}
for line in open(dump, encoding="utf-8", errors="replace"):
    m = re.match(r"\s*0x([0-9A-Fa-f]{8})\s+((?:[0-9A-Fa-f]{2} )+)", line)
    if m:
        base = int(m.group(1), 16)
        for i, b in enumerate(re.findall(r"[0-9A-Fa-f]{2}", m.group(2))[:16]):
            mem[base + i] = int(b, 16)
if not mem:
    print(open(dump, encoding="utf-8", errors="replace").read()[:800])
    sys.exit(1)

def rd64(a):
    return int.from_bytes(bytes(mem.get(a + i, 0) for i in range(8)), "big")

starts = sorted(int(f["start"], 16) for f in json.load(open(funcs_path)))

def owner(a):
    i = bisect.bisect_right(starts, a) - 1
    return f"func_{starts[i]:08X}+0x{a - starts[i]:X}" if i >= 0 else "?"

cur = sp
depth = 0
while sp <= cur < top and depth < 64:
    nxt = rd64(cur) & 0xFFFFFFFF
    if not (cur < nxt <= top):
        break
    lr = rd64(nxt + 0x10) & 0xFFFFFFFF
    print(f"  #{depth:02} sp=0x{cur:08X}  lr=0x{lr:08X}  {owner(lr)}")
    cur = nxt
    depth += 1
