#include "guest.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace {

double add_step(double before, double after, double k)
{
    return before + (after - before) * k;
}

double scale_step(double before, double after, double k)
{
    const double ratio = after / before;
    return std::isfinite(ratio) ? std::copysign(std::abs(before) * std::pow(std::abs(ratio), k), after) : after;
}

void particle_step(void (*body)(ppu_context*), ppu_context* ctx, uint32_t field, uint32_t lanes, double (*step)(double, double, double))
{
    const double k = tick_scale();
    if (k == 1)
        return run(body, ctx);
    const uint32_t owner = (uint32_t)ctx->gpr[4];
    const uint32_t data = vm_read32(owner + 0x40), indices = vm_read32(owner + 0x44), stride = vm_read32(owner + 0xD0);
    const uint32_t count = (uint32_t)std::max((int32_t)vm_read32(owner + 0xD4), 0) * lanes;
    const auto at = [&](uint32_t i) { return data + vm_read16(indices + i / lanes * 2) * stride + field + i % lanes * 4; };
    static thread_local std::vector<uint32_t> before;
    before.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        before[i] = vm_read32(at(i));
    run(body, ctx);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t ea = at(i), after = vm_read32(ea);
        if (after != before[i])
            vm_write32(ea, to_bits((float)step(from_bits(before[i]), from_bits(after), k)));
    }
}

void failed_landing(void (*body)(ppu_context*), ppu_context* ctx)
{
    const double k = tick_scale();
    if (k == 1)
        return run(body, ctx);
    static thread_local std::unordered_map<uint32_t, double> counts;
    const uint32_t at = (uint32_t)ctx->gpr[3] + 0x3EC;
    const int32_t before = (int32_t)vm_read32(at);
    double& count = counts[at];
    if (std::floor(count) != before)
        count = before;
    const double next = count + k;
    const int32_t whole = (int32_t)next, mark = whole / 25 * 25;
    const int32_t target = whole > 300                    ? whole
                         : mark >= 150 && mark > before   ? mark
                         : whole >= 150 && whole % 25 == 0 ? whole - 1
                                                           : std::max(whole, 1);
    vm_write32(at, target - 1);
    run(body, ctx);
    if ((int32_t)vm_read32(at) != target) {
        count = 0;
        return;
    }
    count = next;
    vm_write32(at, whole);
}

}

void func_0073F6D8(ppu_context* ctx)
{
    particle_step(func_0073F6D8_lifted, ctx, 0x2C, 1, vm_read32((uint32_t)ctx->gpr[3] + 0x60) >> 31 ? scale_step : add_step);
}

void func_006E2198(ppu_context* ctx)
{
    if (!(vm_read32((uint32_t)ctx->gpr[3] + 0x88) & 0x40000000))
        return run(func_006E2198_lifted, ctx);
    particle_step(func_006E2198_lifted, ctx, (uint32_t)ctx->gpr[5] + 0x18, 3, add_step);
}

void func_00469E58(ppu_context* ctx)
{
    failed_landing(func_00469E58_lifted, ctx);
}

void func_00930440(ppu_context* ctx)
{
    failed_landing(func_00930440_lifted, ctx);
}

void func_006F7558(ppu_context* ctx)
{
    const double k = tick_scale();
    const uint32_t system = vm_read32((uint32_t)ctx->gpr[3] + 0x1D8);
    if (k == 1 || !system || vm_read8(system + 0x3C) != 1)
        return run(func_006F7558_lifted, ctx);
    const uint32_t delta = vm_read32(system + 0x44);
    vm_write32(system + 0x44, to_bits((float)(from_bits(delta) * k)));
    run(func_006F7558_lifted, ctx);
    vm_write32(system + 0x44, delta);
}
