import re
import sys

rpcs3_path, ours_path = sys.argv[1], sys.argv[2]
mods = ("cellGame", "cellSysutil", "cellSaveData", "sceNp", "sceNpTrophy", "sceNpCommerce2", "cellNetCtl",
        "cellHttp", "cellSsl", "cellSysCache", "cellSysmodule", "cellAudio", "cellPad", "cellFs", "cellSpurs",
        "cellGcmSys", "cellResc", "cellVideoOut", "cellRtc", "cellUserInfo", "cellKb", "cellMouse", "cellOvis",
        "cellPngDec", "cellMsgDialog", "cellHttps")


def rpcs3_calls(path):
    out = []
    pat = re.compile(r"\{PPU\[[^\]]*\] Thread \(([^)]*)\)[^}]*\} (\w+): (\w+)\(")
    for l in open(path, encoding="utf-8", errors="replace"):
        m = pat.search(l)
        if m and m.group(2).startswith(mods) and m.group(3).startswith(("cell", "sce", "_cell")):
            out.append((m.group(1), m.group(3)))
    return out


def our_calls(path):
    out = []
    pat = re.compile(r"^\[(cell\w+|sce\w+)\] (\w+)\(")
    for l in open(path, encoding="utf-8", errors="replace"):
        m = pat.match(l)
        if m:
            out.append(("?", m.group(1) + m.group(2)))
    return out


def norm(n):
    n = re.sub(r"^_", "", n)
    for p in ("cellSysutil", "cellGame", "cellSaveData", "cellGcmSys", "cellGcm", "cellNetCtl", "cellHttps", "cellHttp", "cellSsl",
              "sceNpTrophy", "sceNpCommerce2", "sceNp", "cellSpurs", "cellSysmodule", "cellAudioOut", "cellAudio", "cellPad",
              "cellVideoOut", "cellResc", "cellKb", "cellMouse", "cellMsgDialog", "cellUserInfo", "cellRtc"):
        if n.startswith(p + p):
            n = n[len(p):]
    return n


r = [norm(c) for _, c in rpcs3_calls(rpcs3_path)]
o = [norm(c) for _, c in our_calls(ours_path)]


def dedup(seq):
    res = []
    for x in seq:
        if not res or res[-1] != x:
            res.append(x)
    return res


r, o = dedup(r), dedup(o)
print(f"RPCS3 distinct-in-order calls: {len(r)}, ours: {len(o)}")
seen_o = set(o)
print("--- RPCS3 order (first 90), * = never called by our run")
for x in r[:90]:
    print(("   " if x in seen_o else " * ") + x)
