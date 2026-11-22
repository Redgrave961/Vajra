#include "guest.h"

#include <cmath>
#include <cstring>
#include <immintrin.h>

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

__m128 vmaddfp(__m128 a, __m128 c, __m128 b)
{
    return _mm_fmadd_ps(a, c, b);
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
    __m128 lo{}, hi{}, center{}, half_scale{};
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
        half_scale = _mm_set1_ps(from_bits(r32(v.half)));
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
        float d = from_bits(zero);
        if (!out) return !(d > r2);
        alignas(16) float t[4];
        _mm_store_ps(t, _mm_sub_ps(point, _mm_blendv_ps(lo_p, hi_p, ge)));
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
        uint32_t kids = r32(n + 0xC);
        if (!kids) return;
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
        const __m128 half = vmaddfp(_mm_shuffle_ps(b, b, 0xFF), half_scale, _mm_set1_ps(-0.0f));
        while (sel) {
            const uint32_t h = 31u - (uint32_t)__builtin_clz(sel);
            sel ^= 1u << h;
            node(kids + 16u * h, _mm_blend_ps(vmaddfp(_mm_load_ps(k_signs[h]), half, b), half, 8));
            if (called) kids = r32(n + 0xC);
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
    const uint32_t qx = stfs(x), qy = stfs(y), qz = stfs(z), qr2 = stfs(r2);
    if (!by_box) {
        vm_write32_mm(&m, octree + 0x20, qx);
        vm_write32_mm(&m, octree + 0x24, qy);
        vm_write32_mm(&m, octree + 0x60, qr2);
        vm_write32_mm(&m, octree + 0x28, qz);
    }
    Search s{m, ctx, by_box ? k_box : k_center, octree, (uint32_t)sp - 0x1E0u,
             _mm_setr_ps(from_bits(qx), from_bits(qy), from_bits(qz), 0.0f), from_bits(qr2)};
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

namespace {

inline bool prim_collides(const ppu_mm& m, uint32_t prim)
{
    if (!(vm_read32_mm(&m, prim + 0x12C) & 0x40000000u)) return false;
    const uint32_t owner = vm_read32_mm(&m, prim + 0x4C);
    return !owner || (vm_read32_mm(&m, owner + 0xB0) & 0x00000010u) != 0;
}

struct LineSearch {
    ppu_mm m;
    ppu_context* ctx;
    uint32_t query;
    uint32_t call_sp;
    uint32_t tag = 0, stack = 0, ignore = 0, flags = 0;
    __m128 lo{}, hi{}, q_start{}, q_inv{}, q_ext{};
    bool called = false;

    uint32_t r32(uint32_t a) const { return vm_read32_mm(&m, a); }
    uint64_t r64(uint32_t a) const { return vm_read64_mm(&m, a); }
    void w32(uint32_t a, uint32_t value) { vm_write32_mm(&m, a, value); }

    __m128 vec4(uint32_t a) const
    {
        if (vm_mm_ok(&m, a, 16))
            return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + a)),
                                                     _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
        return _mm_castsi128_ps(_mm_setr_epi32((int)r32(a), (int)r32(a + 4), (int)r32(a + 8), (int)r32(a + 12)));
    }

    __m128 vec3(uint32_t a) const
    {
        if (vm_mm_ok(&m, a, 16))
            return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + a)),
                                                     _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
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
        stack = r32(query + 0x10);
        ignore = r32(query + 0x3C);
        flags = r32(query + 0x2C);
        lo = vec3(query + 0xA4);
        hi = vec3(query + 0xB0);
        q_start = vec3(query + 0x20);
        q_inv = vec3(query + 0x30);
        q_ext = vec3(query + 0x40);
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

    bool slab_hit(__m128 center, __m128 ext) const
    {
        const __m128 bmin = _mm_sub_ps(_mm_sub_ps(_mm_sub_ps(center, ext), q_ext), q_start);
        const __m128 bmax = _mm_sub_ps(_mm_add_ps(_mm_add_ps(center, ext), q_ext), q_start);
        const __m128 t1 = _mm_mul_ps(bmin, q_inv);
        const __m128 t2 = _mm_mul_ps(bmax, q_inv);
        const __m128 lo_t = _mm_blend_ps(_mm_min_ps(t1, t2), _mm_setzero_ps(), 8);
        const __m128 hi_t = _mm_blend_ps(_mm_max_ps(t1, t2), _mm_set1_ps(1.0f), 8);
        const __m128 m0 = _mm_max_ps(lo_t, _mm_shuffle_ps(lo_t, lo_t, _MM_SHUFFLE(1, 0, 3, 2)));
        const __m128 hmax = _mm_max_ps(m0, _mm_shuffle_ps(m0, m0, _MM_SHUFFLE(2, 3, 0, 1)));
        const __m128 n0 = _mm_min_ps(hi_t, _mm_shuffle_ps(hi_t, hi_t, _MM_SHUFFLE(1, 0, 3, 2)));
        const __m128 hmin = _mm_min_ps(n0, _mm_shuffle_ps(n0, n0, _MM_SHUFFLE(2, 3, 0, 1)));
        return (_mm_movemask_ps(_mm_cmpge_ss(hmin, hmax)) & 1) != 0;
    }

    uint32_t allocate(uint32_t s)
    {
        const uint64_t top_end = r64(s);
        const uint32_t result = ((uint32_t)(top_end >> 32) + 7u) & ~7u;
        if (result + k_result_size <= (uint32_t)top_end) return result;
        ctx->gpr[3] = s;
        ctx->gpr[4] = k_chunk_request;
        call(0x0044620C, func_000B902C);
        return (r32(s) + 7u) & ~7u;
    }

    bool test_prim(uint32_t prim)
    {
        if (r32(prim + 0x58) == tag) return false;
        w32(prim + 0x58, tag);
        const uint32_t actor = r32(prim + 0x4C);
        if (!actor || actor == ignore) return false;
        if (!slab_hit(vec3(prim + 0x5C), vec3(prim + 0x68))) return false;
        if (!(r32(prim + 0x12C) & 0x04000000u)) return false;
        if (!prim_collides(m, prim)) return false;
        for (uint32_t a = ignore; a; a = r32(a + 0x98))
            if (a == actor) return false;
        for (uint32_t a = actor; a; a = r32(a + 0x98))
            if (a == ignore) return false;
        ctx->gpr[3] = actor;
        ctx->gpr[4] = prim;
        ctx->gpr[5] = ignore;
        ctx->gpr[6] = flags;
        ctx->ctr = r32(r32(r32(actor) + 0x1B0));
        call(0x004460B4, ps3_indirect_call);
        if ((int32_t)ctx->gpr[3] == 0) return false;

        const uint32_t hit_ea = call_sp + 0xA0;
        if (vm_mm_st_ok(&m, hit_ea, k_result_size)) {
            uint32_t init[k_result_size / 4] = {};
            init[9] = ~0u;
            init[17] = ~0u;
            memcpy(m.vb + hit_ea, init, k_result_size);
            vm_track_write(hit_ea, k_result_size);
        } else {
            for (uint32_t off = 0; off < k_result_size; off += 4)
                w32(hit_ea + off, (off == 0x24 || off == 0x44) ? 0xFFFFFFFFu : 0u);
        }
        ctx->gpr[3] = prim;
        ctx->gpr[4] = hit_ea;
        ctx->gpr[5] = query + 0x7C;
        ctx->gpr[6] = query + 0x20;
        ctx->gpr[7] = query + 0x40;
        ctx->gpr[8] = flags;
        ctx->ctr = r32(r32(r32(prim) + 0x190));
        call(0x00446138, ps3_indirect_call);
        if ((int32_t)ctx->gpr[3] != 0) return false;

        const uint32_t s = stack;
        const uint32_t res = allocate(s);
        w32(s, res + k_result_size);
        if (res) {
            if (vm_mm_ok(&m, hit_ea, k_result_size) && vm_mm_st_ok(&m, res, k_result_size)) {
                memcpy(m.vb + res, m.vb + hit_ea, k_result_size);
                vm_track_write(res, k_result_size);
            } else {
                for (uint32_t off = 0; off < k_result_size; off += 4) w32(res + off, r32(hit_ea + off));
            }
        }
        w32(res, r32(query + 0xC));
        w32(query + 0xC, res);
        return (flags & 0x200u) != 0;
    }

    bool node(uint32_t n, __m128 b)
    {
        if (const uint32_t kids = peek(n + 0xC)) {
            touch(kids);
            touch(kids + 0x40);
        }
        called = false;
        uint64_t head = r64(n);
        const int32_t count = (int32_t)(uint32_t)head;
        for (int32_t i = 0; i < count; i++) {
            const uint32_t list = (uint32_t)(head >> 32);
            if (i + 6 < count) {
                const uint32_t p = peek(list + 4u * (uint32_t)(i + 6));
                touch(p + 0x4C);
                touch(p + 0x5C);
            }
            if (test_prim(r32(list + 4u * (uint32_t)i))) return true;
            if (called) {
                called = false;
                head = r64(n);
            }
        }
        uint32_t kids = r32(n + 0xC);
        if (!kids) return false;
        const int max_nle = _mm_movemask_ps(_mm_cmpnle_ps(hi, b)), max_gt = _mm_movemask_ps(_mm_cmpgt_ps(hi, b));
        const int min_ngt = _mm_movemask_ps(_mm_cmpngt_ps(lo, b)), min_le = _mm_movemask_ps(_mm_cmple_ps(lo, b));
        uint32_t sel = 0;
        if (max_nle & 1) {
            if (max_nle & 2) sel |= (max_nle & 4 ? 0x80u : 0) | (min_ngt & 4 ? 0x40u : 0);
            if (min_ngt & 2) sel |= (max_gt & 4 ? 0x20u : 0) | (min_le & 4 ? 0x10u : 0);
        }
        if (min_ngt & 1) {
            if (max_nle & 2) sel |= (max_gt & 4 ? 0x08u : 0) | (min_le & 4 ? 0x04u : 0);
            if (min_ngt & 2) sel |= (max_gt & 4 ? 0x02u : 0) | (min_le & 4 ? 0x01u : 0);
        }
        if (!sel) return false;
        const __m128 half = _mm_mul_ps(_mm_shuffle_ps(b, b, 0xFF), _mm_set1_ps(0.5f));
        while (sel) {
            const uint32_t h = 31u - (uint32_t)__builtin_clz(sel);
            sel ^= 1u << h;
            const __m128 child_b = _mm_blend_ps(_mm_fmadd_ps(_mm_load_ps(k_signs[h]), half, b), half, 8);
            if (slab_hit(child_b, half)) {
                if (node(kids + 16u * h, child_b)) return true;
                if (r32(query + 0xC) != 0 && (flags & 0x200u)) return true;
                if (called) {
                    called = false;
                    kids = r32(n + 0xC);
                }
            }
        }
        return false;
    }
};

}

void func_00445B7C(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t root = (uint32_t)ctx->gpr[3], query = (uint32_t)ctx->gpr[4], bounds_ea = (uint32_t)ctx->gpr[5];
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    vm_write64_mm(&m, sp + 0x10, lr);
    vm_write64_mm(&m, sp - 0x150, sp);
    LineSearch s{m, ctx, query, (uint32_t)sp - 0x150u};
    s.reload();
    s.node(root, s.vec4(bounds_ea));
    ctx->gpr[1] = sp;
    ctx->lr = lr;
}

void func_005B0EB0(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t self = (uint32_t)ctx->gpr[3], result = (uint32_t)ctx->gpr[4];
    const uint32_t end = (uint32_t)ctx->gpr[6], start = (uint32_t)ctx->gpr[7], extent = (uint32_t)ctx->gpr[8];
    if (vm_read32_mm(&m, self + 0x1C) == 0) {
        ctx->gpr[3] = 1;
        return;
    }
    const int32_t num_planes = (int32_t)vm_read32_mm(&m, self + 0x40);
    if (num_planes == 0) {
        ctx->gpr[3] = 1;
        return;
    }
    if (from_bits(vm_read32_mm(&m, extent)) != 0.0f || from_bits(vm_read32_mm(&m, extent + 4)) != 0.0f ||
        from_bits(vm_read32_mm(&m, extent + 8)) != 0.0f) {
        return func_005B0EB0_lifted(ctx);
    }
    const float sx = from_bits(vm_read32_mm(&m, start)), sy = from_bits(vm_read32_mm(&m, start + 4)),
                sz = from_bits(vm_read32_mm(&m, start + 8));
    const float dx = from_bits(vm_read32_mm(&m, end)) - sx;
    const float dy = from_bits(vm_read32_mm(&m, end + 4)) - sy;
    const float dz = from_bits(vm_read32_mm(&m, end + 8)) - sz;
    float t0 = -3.3999999521443642e+38f;
    float t1 = 3.3999999521443642e+38f;
    float nx_hit = 0.0f, ny_hit = 0.0f, nz_hit = 0.0f;
    constexpr double k_eps = 1e-8;
    const uint32_t planes = vm_read32_mm(&m, self + 0x3C);
    const __m128i shuf = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const __m256i shuf2 = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                                           3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    auto step_plane = [&](const float pl[4]) -> bool {
        const float start_dist = pl[0] * sx + pl[1] * sy + pl[2] * sz - pl[3];
        const float dir_dot = pl[0] * dx + pl[1] * dy + pl[2] * dz;
        if (std::fabs((double)dir_dot) < k_eps) return start_dist <= 0.0f;
        const float t = -start_dist / dir_dot;
        if (dir_dot > 0.0f) {
            if (t < t1) t1 = t;
        } else if (t > t0) {
            t0 = t;
            nx_hit = pl[0];
            ny_hit = pl[1];
            nz_hit = pl[2];
        }
        return !(t1 < 0.0f || t1 < t0);
    };
    int32_t i = 0;
    for (; i + 1 < num_planes; i += 2) {
        const uint32_t p = planes + 16u * (uint32_t)i;
        alignas(32) float pl[8];
        if (vm_mm_ok(&m, p, 32)) {
            _mm256_store_ps(pl, _mm256_castsi256_ps(_mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)(m.vb + p)), shuf2)));
            if (!step_plane(pl) || !step_plane(pl + 4)) {
                ctx->gpr[3] = 1;
                return;
            }
            continue;
        }
        for (int k = 0; k < 2; ++k) {
            const uint32_t pk = p + 16u * (uint32_t)k;
            const float q[4] = {from_bits(vm_read32_mm(&m, pk)), from_bits(vm_read32_mm(&m, pk + 4)),
                                from_bits(vm_read32_mm(&m, pk + 8)), from_bits(vm_read32_mm(&m, pk + 12))};
            if (!step_plane(q)) {
                ctx->gpr[3] = 1;
                return;
            }
        }
    }
    if (i < num_planes) {
        const uint32_t p = planes + 16u * (uint32_t)i;
        alignas(16) float pl[4];
        if (vm_mm_ok(&m, p, 16)) {
            _mm_store_ps(pl, _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(m.vb + p)), shuf)));
        } else {
            pl[0] = from_bits(vm_read32_mm(&m, p));
            pl[1] = from_bits(vm_read32_mm(&m, p + 4));
            pl[2] = from_bits(vm_read32_mm(&m, p + 8));
            pl[3] = from_bits(vm_read32_mm(&m, p + 12));
        }
        if (!step_plane(pl)) {
            ctx->gpr[3] = 1;
            return;
        }
    }
    const float best = from_bits(vm_read32_mm(&m, result + 0x20));
    if (t0 > best) {
        ctx->gpr[3] = 1;
        return;
    }
    if (t0 < 0.0f) {
        vm_write32_mm(&m, result + 0x20, 0);
        const float len2 = dx * dx + dy * dy + dz * dz;
        float ix = dx, iy = dy, iz = dz;
        if (len2 > 0.0f) {
            const float inv = 1.0f / std::sqrt(len2);
            ix = dx * inv;
            iy = dy * inv;
            iz = dz * inv;
        }
        if ((double)(ix * ix + iy * iy + iz * iz) >= k_eps) {
            vm_write32_mm(&m, result + 0x14, to_bits(-ix));
            vm_write32_mm(&m, result + 0x18, to_bits(-iy));
            vm_write32_mm(&m, result + 0x1C, to_bits(-iz));
        } else {
            vm_write32_mm(&m, result + 0x14, 0);
            vm_write32_mm(&m, result + 0x18, 0);
            vm_write32_mm(&m, result + 0x1C, 0x3F800000u);
        }
    } else {
        vm_write32_mm(&m, result + 0x20, to_bits(t0));
        vm_write32_mm(&m, result + 0x14, to_bits(nx_hit));
        vm_write32_mm(&m, result + 0x18, to_bits(ny_hit));
        vm_write32_mm(&m, result + 0x1C, to_bits(nz_hit));
    }
    ctx->gpr[3] = 0;
}
