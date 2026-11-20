#include "guest.h"
#include "ppu_vmx.h"

#include <cmath>
#include <cstring>

namespace {

constexpr uint32_t k_tag = 0x019C2E9C;
constexpr uint32_t k_zero = 0x001CBBD0;
constexpr uint32_t k_root_bounds = 0x01A61F10;
constexpr uint32_t k_result_size = 0x4C;
constexpr uint32_t k_chunk_request = 0x54;

struct Variant {
    uint32_t half;
    uint32_t alloc_return;
    uint32_t recheck_return;
    bool by_center;
};
constexpr Variant k_box{0x001CBBD4, 0x00447D10, 0, false};
constexpr Variant k_center{0x001123C0, 0x004477BC, 0x004476B4, true};

alignas(16) constexpr float k_signs[8][4] = {{-1, -1, -1, 0}, {-1, -1, 1, 0}, {-1, 1, -1, 0}, {-1, 1, 1, 0},
                                              {1, -1, -1, 0},  {1, -1, 1, 0},  {1, 1, -1, 0},  {1, 1, 1, 0}};

float fmadds(float t, float acc) { return (float)std::fma((double)t, (double)t, (double)acc); }

__m128 vmaddfp(__m128 a, __m128 c, __m128 b, uint32_t vscr)
{
    return vmx_flushv(vmx_fixv(_mm_fmadd_ps(vmx_flushv(a, vscr), vmx_flushv(c, vscr), vmx_flushv(b, vscr))), vscr);
}

struct Search {
    ppu_mm m;
    ppu_context* ctx;
    const Variant& v;
    uint32_t octree;
    uint32_t call_sp;
    __m128 point;
    float r2;
    uint32_t tag = 0, zero = 0, stack = 0;
    __m128 lo{}, hi{}, center{};
    float center_r2 = 0;
    bool called = false;

    uint32_t r32(uint32_t a) const { return vm_read32_mm(&m, a); }
    uint64_t r64(uint32_t a) const { return vm_read64_mm(&m, a); }
    void w32(uint32_t a, uint32_t value) { vm_write32_mm(&m, a, value); }

    __m128 vec3(uint32_t a) const
    {
        if (vm_mm_ok(&m, a, 16))
            return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + a)), _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
        return _mm_castsi128_ps(_mm_setr_epi32((int)r32(a), (int)r32(a + 4), (int)r32(a + 8), 0));
    }

    uint32_t peek(uint32_t a) const
    {
        uint32_t x = 0;
        if (vm_mm_ok(&m, a, 4)) memcpy(&x, m.vb + a, 4);
        return __builtin_bswap32(x);
    }

    void touch(uint32_t a) const { _mm_prefetch((const char*)m.vb + a, _MM_HINT_T0); }

    void reload()
    {
        tag = r32(k_tag);
        stack = r32(octree + 0x10);
        lo = vec3(octree + 0xA4);
        hi = vec3(octree + 0xB0);
        if (v.by_center) {
            center = vec3(octree + 0x20);
            center_r2 = from_bits(r32(octree + 0x60));
        } else {
            zero = r32(k_zero);
        }
    }

    void call(uint32_t lr, void (*fn)(ppu_context*))
    {
        const uint64_t sp = ctx->gpr[1];
        ctx->gpr[1] = call_sp;
        ctx->lr = lr;
        run(fn, ctx);
        ctx->gpr[1] = sp;
        reload();
        called = true;
    }

    bool box_hit(uint32_t prim) const
    {
        const __m128 c = vec3(prim + 0x5C), e = vec3(prim + 0x68);
        const __m128 lo_p = _mm_sub_ps(c, e), hi_p = _mm_add_ps(c, e);
        const __m128 ge = _mm_cmpge_ps(point, lo_p);
        const int out = (~_mm_movemask_ps(ge) | _mm_movemask_ps(_mm_cmpgt_ps(point, hi_p))) & 7;
        alignas(16) float t[4];
        _mm_store_ps(t, _mm_sub_ps(point, _mm_blendv_ps(lo_p, hi_p, ge)));
        float d = from_bits(zero);
        if (out & 1) d = t[0] * t[0];
        if (out & 2) d = fmadds(t[1], d);
        if (out & 4) d = fmadds(t[2], d);
        return !(d > r2);
    }

    bool center_hit(uint32_t prim) const
    {
        alignas(16) float t[4];
        _mm_store_ps(t, _mm_sub_ps(vec3(prim + 0x5C), center));
        const float d = fmadds(t[2], fmadds(t[0], t[1] * t[1]));
        return !(d >= center_r2);
    }

    bool recheck(uint32_t actor, uint32_t prim)
    {
        ctx->gpr[3] = actor;
        ctx->gpr[4] = prim;
        ctx->ctr = r32(r32(r32(actor) + 0x328));
        call(v.recheck_return, ps3_indirect_call);
        return (int32_t)ctx->gpr[3] != 0;
    }

    uint32_t allocate(uint32_t s)
    {
        const uint64_t top_end = r64(s);
        const uint32_t result = ((uint32_t)(top_end >> 32) + 7u) & ~7u;
        if (result + k_result_size <= (uint32_t)top_end) return result;
        ctx->gpr[3] = s;
        ctx->gpr[4] = k_chunk_request;
        call(v.alloc_return, func_000B902C);
        return (r32(s) + 7u) & ~7u;
    }

    void add(uint32_t actor, uint32_t prim)
    {
        const uint32_t fill = v.by_center ? 0 : zero, s = stack;
        const uint32_t result = allocate(s);
        w32(s, result + k_result_size);
        if (result && vm_mm_st_ok(&m, result, 4) && vm_mm_st_ok(&m, result + k_result_size - 4, 4)) {
            const uint32_t f = __builtin_bswap32(fill);
            const uint32_t record[k_result_size / 4] = {__builtin_bswap32(r32(octree + 0xC)), __builtin_bswap32(actor), f, f, f, f, f, f, f, ~0u, 0, 0,
                                                        __builtin_bswap32(prim), 0, 0, 0, 0, ~0u, 0};
            memcpy(m.vb + result, record, k_result_size);
            vm_track_write(result, k_result_size);
            w32(octree + 0xC, result);
            w32(actor + 0x18C, tag);
            return;
        }
        if (result) {
            w32(result + 4, 0);
            for (uint32_t k = 0x08; k <= 0x20; k += 4) w32(result + k, fill);
            w32(result + 0x24, 0xFFFFFFFFu);
            for (uint32_t k = 0x28; k <= 0x40; k += 4) w32(result + k, 0);
            w32(result + 0x44, 0xFFFFFFFFu);
            w32(result + 0x48, 0);
        }
        w32(result + 4, actor);
        w32(result + 0x30, prim);
        w32(result, r32(octree + 0xC));
        w32(octree + 0xC, result);
        w32(actor + 0x18C, tag);
    }

    void primitive(uint32_t prim)
    {
        if (r32(prim + 0x58) == tag) return;
        w32(prim + 0x58, tag);
        const uint32_t actor = r32(prim + 0x4C);
        if (!actor) return;
        if (r32(actor + 0x18C) == tag && (!v.by_center || !recheck(actor, prim))) return;
        if (v.by_center ? center_hit(prim) : box_hit(prim)) add(actor, prim);
    }

    void prefetch(uint32_t list, int32_t i, int32_t count) const
    {
        if (i + 8 < count) {
            const uint32_t prim = peek(list + 4u * (uint32_t)(i + 8));
            touch(prim + 0x4C);
            touch(prim + 0x73);
        }
        if (i + 4 < count) touch(peek(peek(list + 4u * (uint32_t)(i + 4)) + 0x4C) + 0x18C);
    }

    uint64_t header(uint32_t n)
    {
        called = false;
        return r64(n);
    }

    void node(uint32_t n, __m128 b)
    {
        if (const uint32_t kids = peek(n + 0xC)) {
            touch(kids);
            touch(kids + 0x40);
        }
        uint64_t head = header(n);
        for (int32_t i = 0; i < (int32_t)(uint32_t)head; i++) {
            const uint32_t list = (uint32_t)(head >> 32);
            prefetch(list, i, (int32_t)(uint32_t)head);
            primitive(r32(list + 4u * (uint32_t)i));
            if (called) head = header(n);
        }
        if (!r32(n + 0xC)) return;
        const int max_nle = _mm_movemask_ps(_mm_cmpnle_ps(hi, b)), max_gt = _mm_movemask_ps(_mm_cmpgt_ps(hi, b));
        const int min_ngt = _mm_movemask_ps(_mm_cmpngt_ps(lo, b)), min_le = _mm_movemask_ps(_mm_cmple_ps(lo, b));
        uint32_t sel = 0;
        if (max_nle & 1) {
            if (max_nle & 2) sel |= (max_nle & 4 ? 0x80u : 0) | (min_ngt & 4 ? 0x40u : 0);
            if (min_ngt & 2) sel |= (max_nle & 4 ? 0x20u : 0) | (min_le & 4 ? 0x10u : 0);
        }
        if (min_ngt & 1) {
            if (max_nle & 2) sel |= (max_gt & 4 ? 0x08u : 0) | (min_le & 4 ? 0x04u : 0);
            if (min_ngt & 2) sel |= (max_gt & 4 ? 0x02u : 0) | (min_ngt & 4 ? 0x01u : 0);
        }
        if (!sel) return;
        const __m128 half_scale = _mm_set1_ps(from_bits(r32(v.half)));
        while (sel) {
            const uint32_t h = 31u - (uint32_t)__builtin_clz(sel);
            sel ^= 1u << h;
            const uint32_t vscr = ctx->vscr;
            const __m128 half = vmaddfp(_mm_shuffle_ps(b, b, 0xFF), half_scale, _mm_set1_ps(-0.0f), vscr);
            node(r32(n + 0xC) + 16u * h, _mm_blend_ps(vmaddfp(_mm_load_ps(k_signs[h]), half, b, vscr), half, 8));
        }
    }
};

void search(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t octree = (uint32_t)ctx->gpr[3], stack = (uint32_t)ctx->gpr[4], point = (uint32_t)ctx->gpr[5];
    const bool by_box = (int32_t)ctx->gpr[7] != 0;
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    const double radius = ctx->fpr[1];
    vm_write64_mm(&m, sp + 0x10, lr);
    vm_write64_mm(&m, sp - 0xC0, sp);
    vm_write64_mm(&m, sp - 0x1E0, sp - 0xC0);
    const uint32_t tag = vm_read32_mm(&m, k_tag) + 1;
    vm_write32_mm(&m, octree + 0xC, 0);
    vm_write32_mm(&m, octree + 0x10, stack);
    vm_write32_mm(&m, k_tag, tag);
    const double x = from_bits(vm_read32_mm(&m, point)), y = from_bits(vm_read32_mm(&m, point + 4)), z = from_bits(vm_read32_mm(&m, point + 8));
    const uint32_t box[6] = {stfs(single(x - radius)), stfs(single(y - radius)), stfs(single(z - radius)),
                             stfs(single(x + radius)), stfs(single(y + radius)), stfs(single(z + radius))};
    for (int i = 0; i < 6; i++) vm_write32_mm(&m, octree + 0xA4 + 4 * i, box[i]);
    vm_write8_mm(&m, octree + 0xBC, 1);
    const double r2 = single(radius * radius);
    const double qx = from_bits(vm_read32_mm(&m, point)), qy = from_bits(vm_read32_mm(&m, point + 4)), qz = from_bits(vm_read32_mm(&m, point + 8));
    if (!by_box) {
        vm_write32_mm(&m, octree + 0x20, stfs(qx));
        vm_write32_mm(&m, octree + 0x24, stfs(qy));
        vm_write32_mm(&m, octree + 0x60, stfs(r2));
        vm_write32_mm(&m, octree + 0x28, stfs(qz));
    }
    Search s{m, ctx, by_box ? k_box : k_center, octree, (uint32_t)sp - 0x1E0u,
             _mm_setr_ps(from_bits(stfs(qx)), from_bits(stfs(qy)), from_bits(stfs(qz)), 0.0f), from_bits(stfs(r2))};
    s.reload();
    s.node(vm_read32_mm(&m, octree + 4),
           _mm_setr_ps(from_bits(vm_read32_mm(&m, k_root_bounds)), from_bits(vm_read32_mm(&m, k_root_bounds + 4)),
                       from_bits(vm_read32_mm(&m, k_root_bounds + 8)), from_bits(vm_read32_mm(&m, k_root_bounds + 12))));
    ctx->gpr[1] = sp;
    ctx->lr = lr;
    ctx->gpr[3] = vm_read32_mm(&m, octree + 0xC);
}

}

void func_0044C160(ppu_context* ctx)
{
    search(ctx);
}
