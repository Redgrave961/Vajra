import json
import os
import sys

src, out, name = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(out, exist_ok=True)
with open(os.path.join(src, f"{name}_exports.json"), encoding="utf-8") as f:
    exp = json.load(f)
with open(os.path.join(src, f"{name}_imports.json"), encoding="utf-8") as f:
    imp = json.load(f)
with open(os.path.join(src, f"{name}_image.bin"), "rb") as f:
    image = f.read()
base = int(exp["base"], 0)

stubs = [{"library": lib, "nid": nid, "stub": slot}
         for lib, fns in imp.items() for nid, slot in fns.items()]
with open(os.path.join(out, f"{name}_stubs.json"), "w", encoding="utf-8") as f:
    json.dump(stubs, f, indent=1)

exports = sorted({int(nid, 0): int(opd, 0) - base
                  for fns in exp["exports"].values() for nid, opd in fns.items()}.items())

with open(os.path.join(out, f"{name}_module.c"), "w", encoding="ascii") as f:
    f.write('#include "prx_loader.h"\n\n')
    f.write(f"static const uint8_t {name}_image[{len(image)}] = {{\n")
    for i in range(0, len(image), 24):
        f.write("".join(f"0x{b:02X}," for b in image[i:i + 24]) + "\n")
    f.write("};\n\n")
    f.write(f"static const prx_export {name}_exports[{len(exports)}] = {{\n")
    for nid, va in exports:
        f.write(f"    {{0x{nid:08X}u, 0x{va:08X}u}},\n")
    f.write("};\n\n")
    f.write(f"extern const prx_func_entry {name}_function_table[];\n")
    f.write(f"extern const uint64_t {name}_function_table_count;\n\n")
    f.write(f"const prx_module* {name}_module(void)\n{{\n")
    f.write("    static prx_module m;\n")
    f.write(f'    m.name = "{name}";\n')
    f.write(f"    m.base = 0x{base:08X}u;\n")
    f.write(f"    m.image = {name}_image;\n")
    f.write(f"    m.image_size = {len(image)}u;\n")
    f.write(f"    m.funcs = {name}_function_table;\n")
    f.write(f"    m.func_count = {name}_function_table_count;\n")
    f.write(f"    m.exports = {name}_exports;\n")
    f.write(f"    m.export_count = {len(exports)}u;\n")
    f.write("    return &m;\n}\n")
print(f"{name}: {len(stubs)} import stubs, {len(exports)} exports, image {len(image)} bytes, base 0x{base:08X}")
