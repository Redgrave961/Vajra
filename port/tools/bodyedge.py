import sys

sys.path.insert(0, sys.argv[1])
from spu_disasm import disassemble_spu

img = open(sys.argv[2], "rb").read()
for spec in sys.argv[3:]:
    off, size, lsa, lo, hi = (int(x, 16) for x in spec.split(":"))
    body = img[off:off + size]
    print(f"== body +0x{off:05X}, LS 0x{lo:X}..0x{hi:X}")
    for i in disassemble_spu(body, base_addr=lsa):
        if lo <= i.addr < hi:
            w = int.from_bytes(body[i.addr - lsa:i.addr - lsa + 4], "big")
            print(f"  {i.addr:05X}  {w:08X}  {i.mnemonic:<9} {i.operands}")
