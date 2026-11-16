import re
import struct
import subprocess
import sys

py, tools, elf, out = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
raw = open(elf, "rb").read()
phoff = struct.unpack(">I", raw[28:32])[0]
phentsize, phnum = struct.unpack(">HH", raw[42:46])
seg_off = seg_va = seg_sz = None
for i in range(phnum):
    o = phoff + i * phentsize
    p_type, p_off, p_va, _, p_fsz, _, p_flags, _ = struct.unpack(">8I", raw[o:o + 32])
    if p_type == 1 and p_flags & 1:
        seg_off, seg_va, seg_sz = p_off, p_va, p_fsz
delta = seg_va - seg_off
text = subprocess.run([py, tools + r"\spu_disasm.py", elf], capture_output=True, text=True, errors="replace").stdout
line_re = re.compile(r"^([0-9A-F]{8}):\s+([0-9A-F]{8})\s+(\S+)\s*(.*)$")
branch = ("br", "bra", "brsl", "brasl", "brz", "brnz", "brhz", "brhnz", "hbrr", "hbra")
with open(out, "w", encoding="ascii") as f:
    for l in text.splitlines():
        m = line_re.match(l)
        if not m:
            continue
        fo = int(m.group(1), 16)
        if not (seg_off <= fo < seg_off + seg_sz):
            continue
        mn, ops = m.group(3), m.group(4)
        if mn in branch:
            ops = re.sub(r"0x([0-9A-F]+)", lambda t: f"0x{int(t.group(1), 16) + delta:X}", ops)
        f.write(f"{fo + delta:05X}  {mn:<9} {ops}\n")
print(f"segment file 0x{seg_off:X} -> LS 0x{seg_va:X} (delta 0x{delta:X}), size 0x{seg_sz:X}")
