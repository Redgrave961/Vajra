import re
import sys
from collections import Counter

def norm(s):
    return re.sub(r"\d+", "N", re.sub(r"0x[0-9A-Fa-f]+", "#", s))[:140]

for path in sys.argv[1:]:
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    print(f"=== {path}: {len(lines)} lines")
    c = Counter(norm(l) for l in lines)
    print("--- most frequent")
    for k, n in c.most_common(10):
        print(f"{n:8}x {k}")
    print("--- first occurrence of each distinct line, after the first 40 (up to 70)")
    seen = set()
    shown = 0
    for l in lines[40:]:
        k = norm(l)
        if k in seen:
            continue
        seen.add(k)
        print("   " + l[:200])
        shown += 1
        if shown >= 70:
            break
    print("--- last 12 lines")
    for l in lines[-12:]:
        print("   " + l[:200])
