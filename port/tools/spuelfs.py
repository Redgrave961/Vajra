import struct
import sys

d = open(sys.argv[1], "rb").read()
i = 0
while True:
    i = d.find(b"\x7fELF\x01\x02\x01", i)
    if i < 0:
        break
    m, = struct.unpack(">H", d[i + 18:i + 20])
    e, phoff = struct.unpack(">II", d[i + 24:i + 32])
    phn, = struct.unpack(">H", d[i + 44:i + 46])
    segs = []
    for k in range(phn):
        o = i + phoff + k * 32
        if o + 32 > len(d):
            break
        segs.append(struct.unpack(">8I", d[o:o + 32]))
    end = max((s[1] + s[4] for s in segs if s[0] == 1), default=0)
    print(f"ELF32 @0x{i:X} machine {m} entry 0x{e:X} span 0x{end:X} " +
          " ".join(f"[t{s[0]} off 0x{s[1]:X} va 0x{s[2]:X} fsz 0x{s[4]:X} msz 0x{s[5]:X} f{s[6]}]" for s in segs))
    i += 4
