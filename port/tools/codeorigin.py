import re
import struct
import sys


def segments(path):
    b = open(path, "rb").read()
    if b[:4] != b"\x7fELF" or b[4] != 2:
        return b, [(0, 0, len(b))]
    phoff = struct.unpack(">Q", b[0x20:0x28])[0]
    phnum = struct.unpack(">H", b[0x38:0x3A])[0]
    segs = []
    for i in range(phnum):
        p = phoff + 56 * i
        typ, = struct.unpack(">I", b[p:p + 4])
        off, va = struct.unpack(">QQ", b[p + 8:p + 24])
        fsz, = struct.unpack(">Q", b[p + 32:p + 40])
        if typ == 1:
            segs.append((off, va, fsz))
    return b, segs


def main():
    log, images = sys.argv[1], sys.argv[2:]
    loaded = []
    for spec in images:
        path, _, base = spec.partition("@")
        b, segs = segments(path)
        loaded.append((path, b, segs, int(base, 0) if base else None))
    pat = re.compile(r"img=0x([0-9A-F]+) ls=0x([0-9A-F]+)\s+(\d+)\s+code((?: [0-9A-F]{8}){8})")
    seen = {}
    for line in open(log, encoding="utf-8", errors="replace"):
        m = pat.search(line)
        if not m:
            continue
        ls, calls, sig = int(m.group(2), 16), int(m.group(3)), bytes.fromhex(m.group(4).replace(" ", ""))
        seen.setdefault((ls, sig), 0)
        seen[(ls, sig)] = max(seen[(ls, sig)], calls)
    for (ls, sig), calls in sorted(seen.items(), key=lambda kv: -kv[1]):
        hits = []
        for path, b, segs, base in loaded:
            k = b.find(sig)
            while k >= 0 and len(hits) < 4:
                for off, va, fsz in segs:
                    if off <= k < off + fsz:
                        ea = (base + k) if base is not None else va + (k - off)
                        hits.append(f"{path.split(chr(92))[-1]} EA 0x{ea:08X}")
                k = b.find(sig, k + 1)
        print(f"LS 0x{ls:05X} calls/window {calls:>7}  {' | '.join(hits) if hits else 'NOT FOUND in the given images'}")


if __name__ == "__main__":
    main()
