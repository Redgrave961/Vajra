#include "guest.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace {

inline double add_step(double before, double after, double k)
{
    return before + (after - before) * k;
}

inline double scale_step(double before, double after, double k)
{
    const double ratio = after / before;
    if (!std::isfinite(ratio)) return after;
    const double mag = k == 0.5 ? std::sqrt(std::abs(ratio)) : std::pow(std::abs(ratio), k);
    return std::copysign(std::abs(before) * mag, after);
}

template <uint32_t Lanes, bool IsScale>
void particle_step(void (*body)(ppu_context*), ppu_context* ctx, uint32_t field)
{
    const double k = tick_scale();
    if (k == 1)
        return run(body, ctx);
    const ppu_mm m = ppu_mm_take();
    const uint32_t owner = (uint32_t)ctx->gpr[4];
    const uint32_t data = vm_read32_mm(&m, owner + 0x40);
    const uint32_t indices = vm_read32_mm(&m, owner + 0x44);
    const uint32_t stride = vm_read32_mm(&m, owner + 0xD0);
    const uint32_t particles = (uint32_t)std::max((int32_t)vm_read32_mm(&m, owner + 0xD4), 0);
    if (!particles)
        return run(body, ctx);
    static thread_local std::vector<uint32_t> before;
    before.resize((size_t)particles * Lanes);
    for (uint32_t p = 0; p < particles; ++p) {
        const uint32_t base = data + vm_read16_mm(&m, indices + p * 2u) * stride + field;
        for (uint32_t l = 0; l < Lanes; ++l)
            before[p * Lanes + l] = vm_read32_mm(&m, base + l * 4u);
    }
    run(body, ctx);
    for (uint32_t p = 0; p < particles; ++p) {
        const uint32_t base = data + vm_read16_mm(&m, indices + p * 2u) * stride + field;
        for (uint32_t l = 0; l < Lanes; ++l) {
            const uint32_t ea = base + l * 4u;
            const uint32_t prev = before[p * Lanes + l];
            const uint32_t after = vm_read32_mm(&m, ea);
            if (after != prev) {
                const double v = IsScale ? scale_step(from_bits(prev), from_bits(after), k)
                                         : add_step(from_bits(prev), from_bits(after), k);
                vm_write32_mm(&m, ea, to_bits((float)v));
            }
        }
    }
}

void failed_landing(void (*body)(ppu_context*), ppu_context* ctx)
{
    const double k = tick_scale();
    if (k == 1)
        return run(body, ctx);
    static thread_local std::unordered_map<uint32_t, double> counts;
    if (counts.size() >= 4096)
        counts.clear();
    const uint32_t at = (uint32_t)ctx->gpr[3] + 0x3EC;
    const int32_t before = (int32_t)vm_read32(at);
    double& count = counts[at];
    if (std::floor(count) != before)
        count = before;
    const double next = count + k;
    const int32_t whole = (int32_t)next, mark = whole / 50 * 50;
    const int32_t target = whole > 300                    ? whole
                         : mark >= 150 && mark > before   ? mark
                         : whole >= 150 && whole % 50 == 0 ? whole - 1
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
    if (vm_read32((uint32_t)ctx->gpr[3] + 0x60) >> 31)
        particle_step<1, true>(func_0073F6D8_lifted, ctx, 0x2C);
    else
        particle_step<1, false>(func_0073F6D8_lifted, ctx, 0x2C);
}

void func_006E2198(ppu_context* ctx)
{
    if (!(vm_read32((uint32_t)ctx->gpr[3] + 0x88) & 0x40000000))
        return run(func_006E2198_lifted, ctx);
    particle_step<3, false>(func_006E2198_lifted, ctx, (uint32_t)ctx->gpr[5] + 0x18);
}

void func_00469E58(ppu_context* ctx)
{
    failed_landing(func_00469E58_lifted, ctx);
}

void func_00930440(ppu_context* ctx)
{
    failed_landing(func_00930440_lifted, ctx);
}

namespace {

struct WarmupTrack {
    uint32_t psc;
    uint32_t same_time_ticks;
    double last_time;
    double gate_until;
};
thread_local WarmupTrack g_warmup_tracks[512];

void warmup_spawn_gate(const ppu_mm& m, uint32_t psc, double k, float dt)
{
    if (k >= 0.99 || !(dt > 0.0f)) return;

    WarmupTrack& tr = g_warmup_tracks[((psc >> 4) * 2654435761u) >> 23];
    if (tr.psc != psc) {
        tr.psc = psc;
        tr.same_time_ticks = 0;
        tr.last_time = -1.0;
        tr.gate_until = -1.0;
    }

    const double now = from_bits64(vm_fast_read64(m, 0x019FED10));
    if (now == tr.last_time) {
        tr.same_time_ticks++;
        if (tr.same_time_ticks >= 4)
            tr.gate_until = now + (1.0 / 30.0) - 0.0005;
    } else {
        tr.same_time_ticks = 0;
        tr.last_time = now;
    }

    if (now + dt < tr.gate_until) {
        const uint32_t inst_data = vm_fast_read32(m, psc + 0x1E8);
        const uint32_t inst_num = std::min(vm_fast_read32(m, psc + 0x1EC), 8u);
        for (uint32_t i = 0; i < inst_num; ++i) {
            const uint32_t inst = vm_fast_read32(m, inst_data + i * 4u);
            if (!inst) continue;
            const uint32_t act = (uint32_t)std::max((int32_t)vm_fast_read32(m, inst + 0xD4), 0);
            const uint32_t pdata = vm_fast_read32(m, inst + 0x40);
            const uint32_t pind = vm_fast_read32(m, inst + 0x44);
            const uint32_t stride = vm_fast_read32(m, inst + 0xD0);
            if (!act || !pdata || !pind || stride < 0x60) continue;
            for (uint32_t p = 0; p < act; ++p) {
                const uint32_t base = pdata + vm_read16_mm(&m, pind + p * 2u) * stride;
                vm_fast_write32(m, base + 0x50, 0);
                vm_fast_write32(m, base + 0x54, 0);
                vm_fast_write32(m, base + 0x58, 0);
            }
        }
    }
}

}

void func_006F7558(ppu_context* ctx)
{
    const double k = tick_scale();
    const uint32_t psc = (uint32_t)ctx->gpr[3];
    const ppu_mm m = ppu_mm_take();
    const uint32_t system = vm_fast_read32(m, psc + 0x1D8);
    const float dt = (float)ctx->fpr[1];
    if (k == 1 || !system || vm_read8_mm(&m, system + 0x3C) != 1) {
        run(func_006F7558_lifted, ctx);
        if (system) warmup_spawn_gate(m, psc, k, dt);
        return;
    }
    struct Scaled { uint32_t system, delta; };
    static thread_local Scaled active[16];
    static thread_local uint32_t depth;
    const uint32_t current = vm_fast_read32(m, system + 0x44);
    uint32_t original = current;
    for (uint32_t i = depth; i-- > 0;)
        if (active[i].system == system) {
            original = active[i].delta;
            break;
        }
    vm_fast_write32(m, system + 0x44, to_bits((float)(from_bits(original) * k)));
    const bool tracked = depth < std::size(active);
    if (tracked) active[depth++] = { system, original };
    run(func_006F7558_lifted, ctx);
    if (tracked) --depth;
    vm_fast_write32(m, system + 0x44, current);
    warmup_spawn_gate(m, psc, k, dt);
}
