import sys

src, dst, sym = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, "rb").read()
with open(dst, "w", encoding="ascii") as out:
    out.write(f"static const unsigned char {sym}[] = {{\n")
    for i in range(0, len(data), 24):
        chunk = ",".join(str(b) for b in data[i : i + 24])
        out.write(f"  {chunk},\n")
    out.write("};\n")
