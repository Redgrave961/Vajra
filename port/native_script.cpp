#include "ue3_reflect.h"

void func_000D7644(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint8_t* vb = m.vb;
    const uint32_t object = (uint32_t)ctx->gpr[3];
    const uint64_t name = ctx->gpr[4];
    const uint64_t name_be = __builtin_bswap64(name);
    vm_write64_mm(&m, ctx->gpr[1] + 0x38, name);
    const uint32_t index = (uint32_t)(name >> 32);
    uint32_t found = 0;
    const uint32_t frame = vm_fast_read32(m, object + 0x14);
    const uint32_t state = frame ? vm_fast_read32(m, frame + 0x28) : 0;
    if (state && (int32_t)ctx->gpr[5] == 0) found = ue3::find_function_walk(vb, state, index, name_be);
    if (!found) found = ue3::find_function_walk(vb, vm_fast_read32(m, object + 0x34), index, name_be);
    ctx->gpr[3] = found;
}
