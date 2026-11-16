import glob
import os
import re
import sys

sys.path.insert(0, sys.argv[2])
from elf_parser import ELFFile
from ppu_loader import read_vaddr

elf = ELFFile(sys.argv[1])
elf.load()
recompiled = sys.argv[3]
names = sys.argv[4:]

bodies = {}
pat = re.compile(r"^void func_([0-9A-F]{8})\(ppu_context\* ctx\) \{")
for p in glob.glob(os.path.join(recompiled, "ppu_recomp_*.cpp")):
    cur = None
    buf = []
    for line in open(p, encoding="utf-8", errors="replace"):
        m = pat.match(line)
        if m:
            if cur in names:
                bodies[cur] = buf
            cur, buf = m.group(1), []
        elif cur:
            buf.append(line)
    if cur in names:
        bodies[cur] = buf

hi_re = re.compile(r"gpr\[(\d+)\] = \(int64_t\)\(int32_t\)\(\(uint32_t\)(0x[0-9A-F]+) << 16\)")
imm_re = re.compile(r"\(int64_t\)\((-?(?:0x[0-9A-F]+|\d+))\)")


def cstr(addr):
    raw = read_vaddr(elf, addr, 160)
    if not raw:
        return None
    if raw[0] == 0 and len(raw) > 1 and 32 <= raw[1] < 127:
        w = bytes(raw[i + 1] for i in range(0, len(raw) - 1, 2) if raw[i] == 0)
        s = w.split(b"\0")[0]
        return ("L\"" + s.decode("latin1") + "\"") if len(s) >= 4 else None
    s = raw.split(b"\0")[0]
    if len(s) >= 4 and all(32 <= c < 127 for c in s):
        return "\"" + s.decode("latin1") + "\""
    return None


TOC = 0x019AF8F8
toc_re = re.compile(r"vm_read32\(ctx->gpr\[2\] \+ (-?(?:0x[0-9A-F]+|\d+))\)")
call_re = re.compile(r"func_([0-9A-F]{8})\(ctx\)")


def be32(a):
    raw = read_vaddr(elf, a, 4)
    return int.from_bytes(raw, "big") if raw else None


for n in names:
    lines = bodies.get(n)
    print(f"=== func_{n}: {len(lines) if lines else 0} lines")
    if not lines:
        continue
    calls = sorted(set(c for l in lines for c in call_re.findall(l)))
    print(f"   calls: {' '.join(calls[:24])}")
    for l in lines:
        for off in toc_re.findall(l):
            p = be32((TOC + int(off, 0)) & 0xFFFFFFFF)
            if p is not None:
                s = cstr(p) if 0x01606A10 <= p < 0x017AEFB8 else None
                print(f"   toc{int(off, 0):+#x} -> 0x{p:08X} {s or ''}"[:150])
    seen = set()
    for i, l in enumerate(lines):
        m = hi_re.search(l)
        if not m:
            continue
        hi = int(m.group(2), 16) << 16
        for j in range(i, min(i + 6, len(lines))):
            for imm in imm_re.findall(lines[j]):
                a = (hi + int(imm, 0)) & 0xFFFFFFFF
                if 0x01606A10 <= a < 0x017AEFB8 and a not in seen:
                    seen.add(a)
                    s = cstr(a)
                    if s:
                        print(f"   0x{a:08X}  {s[:120]}")
