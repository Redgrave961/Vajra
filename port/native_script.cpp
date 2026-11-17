#include "ppu_recomp.h"

namespace {

uint32_t probe(const ppu_mm& m, uint32_t level, uint32_t index, uint64_t name)
{
    const uint32_t size = vm_read32_mm(&m, level + 0xB8);
    if (!size) return 0;
    const uint32_t buckets = vm_read32_mm(&m, level + 0xB4);
    const uint32_t table = buckets ? buckets : level + 0xB0;
    const uint32_t elements = vm_read32_mm(&m, level + 0x80);
    for (uint32_t i = vm_read32_mm(&m, table + 4u * (index & (size - 1u))); (int32_t)i != -1;) {
        const uint32_t element = elements + 20u * i;
        if (vm_read64_mm(&m, element) == name) return vm_read32_mm(&m, element + 8);
        i = vm_read32_mm(&m, element + 0xC);
    }
    return 0;
}

uint32_t walk(const ppu_mm& m, uint32_t level, uint32_t index, uint64_t name)
{
    for (; level; level = vm_read32_mm(&m, level + 0x40))
        if (const uint32_t found = probe(m, level, index, name)) return found;
    return 0;
}

}

void func_000D7644(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t object = (uint32_t)ctx->gpr[3];
    const uint64_t name = ctx->gpr[4];
    vm_write64_mm(&m, ctx->gpr[1] + 0x38, name);
    const uint32_t index = (uint32_t)(name >> 32);
    uint32_t found = 0;
    const uint32_t frame = vm_read32_mm(&m, object + 0x14);
    const uint32_t state = frame ? vm_read32_mm(&m, frame + 0x28) : 0;
    if (state && (int32_t)ctx->gpr[5] == 0) found = walk(m, state, index, name);
    if (!found) found = walk(m, vm_read32_mm(&m, object + 0x34), index, name);
    ctx->gpr[3] = found;
}
