import os
import sys

needle = bytes.fromhex(sys.argv[1].replace(" ", ""))
for root in sys.argv[2:]:
    for dirpath, _, files in os.walk(root):
        for f in files:
            p = os.path.join(dirpath, f)
            try:
                if os.path.getsize(p) > 512 << 20:
                    continue
                b = open(p, "rb").read()
            except OSError:
                continue
            k = b.find(needle)
            hits = []
            while k >= 0 and len(hits) < 4:
                hits.append(k)
                k = b.find(needle, k + 1)
            if hits:
                print("%s: %s" % (p, " ".join("0x%X" % h for h in hits)))
