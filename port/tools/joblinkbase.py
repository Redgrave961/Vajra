import re
import sys

sys.path.insert(0, sys.argv[1])
import lift_spurs_jobs as L
from spu_disasm import disassemble_spu

segs = L.load_segments(sys.argv[2])


def raw_at(ea, n):
    for va, d in segs:
        if va <= ea < va + len(d):
            return d[ea - va:ea - va + n]
    return b""


op = re.compile(r"\$r(\d+), 0x([0-9A-F]+)$")
for ea in (int(x, 16) for x in sys.argv[3:]):
    ins = disassemble_spu(raw_at(ea, 0x8000), base_addr=0)
    print("EA 0x%08X" % ea)
    shown = 0
    for k, i in enumerate(ins):
        if i.mnemonic != "brsl":
            continue
        m = op.match(i.operands)
        if not m or m.group(1) == "0":
            continue
        link = i.addr + 4
        for j in ins[max(0, k - 4):k + 4]:
            n = op.match(j.operands) if j.mnemonic == "ila" else None
            if n:
                print("   brsl $r%s at +0x%X (link +0x%X), ila $r%s,0x%X -> link base 0x%X"
                      % (m.group(1), i.addr, link, n.group(1), int(n.group(2), 16), int(n.group(2), 16) - link))
                shown += 1
        if shown >= 3:
            break
