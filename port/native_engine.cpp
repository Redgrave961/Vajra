#include "guest.h"
#include "ppu_vmx.h"

#include <cstring>

namespace {

constexpr uint32_t k_copy_flags = 0x019EEC98;
constexpr uint32_t k_box_mask = 0x01AA3810;
constexpr uint32_t k_sign_mask = 0x00106CE0;

void call(ppu_context* ctx, uint64_t sp, uint64_t lr, void (*fn)(ppu_context*))
{
    ctx->gpr[1] = sp;
    ctx->lr = lr;
    run(fn, ctx);
}

void frame(const ppu_mm& m, uint64_t sp, uint32_t size, uint64_t lr)
{
    vm_write64_mm(&m, sp - size, sp);
    vm_write64_mm(&m, sp + 0x10, lr);
}

bool plain_copy(const ppu_mm& m, uint32_t prop, uint64_t owner, uint64_t graph)
{
    return !(vm_read64_mm(&m, prop + 0x48) & 0x400000) || !(uint32_t)owner || (vm_read32_mm(&m, k_copy_flags) & 1) ||
           ((uint32_t)graph && !vm_read32_mm(&m, graph + 0x18));
}

struct alignas(16) Vec {
    uint8_t b[16];
};

Vec load(const ppu_mm& m, uint32_t ea)
{
    Vec v;
    std::memcpy(v.b, m.vb + (ea & ~15u), 16);
    vmx_rev(v.b, v.b);
    return v;
}

Vec splat(const uint8_t* word)
{
    Vec v;
    for (int i = 0; i < 16; i += 4) std::memcpy(v.b + i, word, 4);
    vmx_rev(v.b, v.b);
    return v;
}

Vec andc(const Vec& a, const Vec& c)
{
    Vec r;
    for (int i = 0; i < 16; i++) r.b[i] = a.b[i] & (uint8_t)~c.b[i];
    return r;
}

struct ElementBox {
    const ppu_mm& m;
    uint32_t vscr;
    Vec center, extent, mask, sign;

    bool hit(uint32_t elem) const
    {
        uint8_t line[32];
        std::memcpy(line, m.vb + (elem & ~15u), 16);
        std::memcpy(line + 16, m.vb + ((elem + 11) & ~15u), 16);
        Vec a, d, s, ge;
        std::memcpy(a.b, line + (elem & 15), 16);
        vmx_rev(a.b, a.b);
        a = andc(a, mask);
        const Vec r = andc(splat(m.vb + (uint32_t)(elem + 12)), mask);
        vmx_vsubfp(&d, &a, &center, vscr);
        d = andc(d, sign);
        vmx_vaddfp(&s, &r, &extent, vscr);
        return vmx_vcmpgefp(&ge, &s, &d) & 8;
    }

    int32_t first(uint32_t data, int32_t index, int32_t count) const
    {
        for (; index < count; index++)
            if (hit(data + 36u * (uint32_t)index)) break;
        return index;
    }
};

ElementBox element_box(const ppu_mm& m, const ppu_context* ctx, uint32_t self)
{
    return {m, ctx->vscr, load(m, self), load(m, self + 0x10), load(m, k_box_mask), splat(m.vb + k_sign_mask)};
}

}

void func_00F297C8(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t dst = (uint32_t)ctx->gpr[3], src = (uint32_t)ctx->gpr[4];
    const uint64_t n = ctx->gpr[5];
    if (n >= 0x80000000u || !vm_mm_ok(&m, src, (uint32_t)n) || !plain_store(m, dst, (uint32_t)n) ||
        ((uint64_t)src < dst + n && (uint64_t)dst < src + n))
        return func_00F297C8_lifted(ctx);
    std::memcpy(m.vb + dst, m.vb + src, n);
    ctx->gpr[3] = dst;
}

void func_00F35E2C(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t dst = (uint32_t)ctx->gpr[3];
    const uint64_t n = ctx->gpr[5];
    const uint8_t c = (uint8_t)ctx->gpr[4];
    if (n >= 0x80000000u || !plain_store(m, dst, (uint32_t)n)) return func_00F35E2C_lifted(ctx);
    std::memset(m.vb + dst, c, n);
}

void func_000E5170(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    if (!plain_copy(m, (uint32_t)ctx->gpr[3], ctx->gpr[7], ctx->gpr[8])) return func_000E5170_lifted(ctx);
    const uint32_t dst = (uint32_t)ctx->gpr[4], value = vm_read32_mm(&m, ctx->gpr[5]);
    vm_write32_mm(&m, dst, value);
}

void func_000E5548(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr, prop = ctx->gpr[3], root = ctx->gpr[6], owner = ctx->gpr[7],
                   graph = ctx->gpr[8];
    const uint32_t dst = (uint32_t)ctx->gpr[4], src = (uint32_t)ctx->gpr[5];
    for (int32_t i = 0; i < (int32_t)vm_read32_mm(&m, prop + 0x40); i++) {
        const uint32_t off = vm_read32_mm(&m, prop + 0x44) * (uint32_t)i;
        if (plain_copy(m, (uint32_t)prop, owner, graph)) {
            vm_write32_mm(&m, dst + off, vm_read32_mm(&m, src + off));
            continue;
        }
        frame(m, sp, 0xB0, lr);
        ctx->gpr[3] = prop;
        ctx->gpr[4] = dst + off;
        ctx->gpr[5] = src + off;
        ctx->gpr[6] = root;
        ctx->gpr[7] = owner;
        ctx->gpr[8] = graph;
        call(ctx, sp - 0xB0, 0x000E55F0, func_000E5170_lifted);
    }
    ctx->gpr[1] = sp;
    ctx->lr = lr;
}

void func_0104E850(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr, it = ctx->gpr[3];
    const uint32_t self = (uint32_t)it;
    bool framed = false;
    for (;;) {
        const uint32_t node = vm_read32_mm(&m, self + 0x1940);
        int32_t index = (int32_t)vm_read32_mm(&m, self + 0x1944);
        if (index >= 0) {
            const int32_t count = (int32_t)vm_read32_mm(&m, node + 4);
            if (index < count) {
                const int32_t hit = element_box(m, ctx, self).first(vm_read32_mm(&m, node), index, count);
                if (hit < count) {
                    if (hit != index) vm_write32_mm(&m, self + 0x1944, (uint32_t)hit);
                    break;
                }
                index = count;
            }
        }
        if (!framed) frame(m, sp, 0xC0, lr);
        framed = true;
        ctx->gpr[3] = it + 0x20;
        call(ctx, sp - 0xC0, 0x0104E8C8, func_01040424);
        if (!vm_read32_mm(&m, self + 0x20)) {
            vm_write32_mm(&m, self + 0x1940, node);
            vm_write32_mm(&m, self + 0x1944, (uint32_t)index);
            break;
        }
        ctx->gpr[3] = it;
        call(ctx, sp - 0xC0, 0x0104E8DC, func_0104E4D8);
        vm_write32_mm(&m, self + 0x1940, vm_read32_mm(&m, self + 0x20));
        vm_write32_mm(&m, self + 0x1944, 0);
    }
    ctx->gpr[1] = sp;
    ctx->lr = lr;
}
