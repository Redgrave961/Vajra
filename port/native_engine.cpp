#include "guest.h"
#include "ue3_reflect.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <immintrin.h>

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

inline __m128i bswap16(__m128i v)
{
    return _mm_shuffle_epi8(v, _mm_setr_epi8(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0));
}

inline __m128 load_vec(const ppu_mm& m, uint32_t ea)
{
    return _mm_castsi128_ps(bswap16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(m.vb + (ea & ~15u)))));
}

inline __m128 splat_word(const uint8_t* word)
{
    uint32_t w;
    std::memcpy(&w, word, 4);
    return _mm_castsi128_ps(_mm_set1_epi32((int)__builtin_bswap32(w)));
}

struct ElementBox {
    const ppu_mm& m;
    __m128 center, extent, mask, sign;

    bool hit(uint32_t elem) const
    {
        __m128 a;
        if (vm_mm_ok(&m, elem, 16)) {
            a = _mm_castsi128_ps(bswap16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(m.vb + elem))));
        } else {
            uint8_t line[32];
            std::memcpy(line, m.vb + (elem & ~15u), 16);
            std::memcpy(line + 16, m.vb + ((elem + 11) & ~15u), 16);
            a = _mm_castsi128_ps(bswap16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(line + (elem & 15)))));
        }
        a = _mm_andnot_ps(mask, a);
        const __m128 r = _mm_andnot_ps(mask, splat_word(m.vb + (uint32_t)(elem + 12)));
        const __m128 d = _mm_andnot_ps(sign, _mm_sub_ps(a, center));
        const __m128 s = _mm_add_ps(r, extent);
        return _mm_movemask_ps(_mm_cmpge_ps(s, d)) == 0xF;
    }

    int32_t first(uint32_t data, int32_t index, int32_t count) const
    {
        const __m256 center2 = _mm256_broadcast_ps(&center);
        const __m256 extent2 = _mm256_broadcast_ps(&extent);
        const __m256 mask2 = _mm256_broadcast_ps(&mask);
        const __m256 sign2 = _mm256_broadcast_ps(&sign);
        const __m256i shuf2 = _mm256_setr_epi8(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
                                               15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
        for (; index + 1 < count; index += 2) {
            const uint32_t e0 = data + 36u * (uint32_t)index;
            const uint32_t e1 = e0 + 36u;
            if (index + 6 < count) {
                _mm_prefetch(reinterpret_cast<const char*>(m.vb + e0 + 36u * 4u), _MM_HINT_T0);
                _mm_prefetch(reinterpret_cast<const char*>(m.vb + e0 + 36u * 5u), _MM_HINT_T0);
            }
            if (__builtin_expect(vm_mm_ok(&m, e0, 52), 1)) {
                const __m128i raw0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(m.vb + e0));
                const __m128i raw1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(m.vb + e1));
                const __m256 a = _mm256_andnot_ps(mask2, _mm256_castsi256_ps(_mm256_shuffle_epi8(_mm256_set_m128i(raw1, raw0), shuf2)));
                uint32_t w0, w1;
                std::memcpy(&w0, m.vb + e0 + 12u, 4);
                std::memcpy(&w1, m.vb + e1 + 12u, 4);
                const __m256 r = _mm256_andnot_ps(mask2, _mm256_castsi256_ps(_mm256_set_m128i(_mm_set1_epi32((int)__builtin_bswap32(w1)),
                                                                                              _mm_set1_epi32((int)__builtin_bswap32(w0)))));
                const __m256 d = _mm256_andnot_ps(sign2, _mm256_sub_ps(a, center2));
                const __m256 s = _mm256_add_ps(r, extent2);
                const int mm = _mm256_movemask_ps(_mm256_cmp_ps(s, d, _CMP_GE_OQ));
                if ((mm & 0x0F) == 0x0F) return index;
                if ((mm & 0xF0) == 0xF0) return index + 1;
            } else {
                if (hit(e0)) return index;
                if (hit(e1)) return index + 1;
            }
        }
        if (index < count && hit(data + 36u * (uint32_t)index)) return index;
        return count;
    }
};

ElementBox element_box(const ppu_mm& m, uint32_t self)
{
    return {m, load_vec(m, self), load_vec(m, self + 0x10), load_vec(m, k_box_mask), splat_word(m.vb + k_sign_mask)};
}

}

void func_00F3F180(ppu_context* ctx)
{
    const uint32_t u = vm_read32((uint32_t)ctx->gpr[3]);
    const uint32_t exp = u & 0x7F800000u;
    if (exp == 0x7F800000u) {
        ctx->gpr[3] = (u & 0x007FFFFFu) ? 2u : 1u;
    } else if ((u & 0x7FFFFFFFu) == 0u) {
        ctx->gpr[3] = 0u;
    } else {
        ctx->gpr[3] = (uint64_t)(int64_t)-1;
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
    vm_track_write(dst, n);
    ctx->gpr[3] = dst;
}

void func_0136F9A8_lifted(ppu_context* ctx);

void func_0136F9A8(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t dst = (uint32_t)ctx->gpr[3];
    if (__builtin_expect(!plain_store(m, dst, 0x9Cu), 0))
        return func_0136F9A8_lifted(ctx);
    std::memset(m.vb + dst, 0, 0x74u);
    std::memset(m.vb + dst + 0x84u, 0, 0x18u);
    if (__builtin_expect(ppu_slow_page[dst >> 12] || ((dst ^ (dst + 0x9Bu)) >> 12), 0))
        vm_track_write(dst, 0x9Cu);
    ctx->gpr[4] = ctx->gpr[3] + 0x70u;
    ctx->gpr[5] = 0;
    ctx->gpr[6] = 2;
    ctx->ctr = 0;
}

void func_00F35E2C(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t dst = (uint32_t)ctx->gpr[3];
    const uint64_t n = ctx->gpr[5];
    const uint8_t c = (uint8_t)ctx->gpr[4];
    if (n >= 0x80000000u || !plain_store(m, dst, (uint32_t)n)) return func_00F35E2C_lifted(ctx);
    std::memset(m.vb + dst, c, n);
    vm_track_write(dst, n);
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
    const int32_t count = (int32_t)vm_read32_mm(&m, prop + 0x40);
    if (count > 0 && plain_copy(m, (uint32_t)prop, owner, graph)) {
        const uint32_t stride = vm_read32_mm(&m, prop + 0x44);
        if (stride == 4u) {
            const uint32_t bytes = (uint32_t)count * 4u;
            if (__builtin_expect(vm_mm_ok(&m, src, bytes) && plain_store(m, dst, bytes), 1)) {
                std::memmove(m.vb + dst, m.vb + src, bytes);
                if (__builtin_expect(ppu_slow_page[dst >> 12] || ((dst ^ (dst + bytes - 1u)) >> 12), 0))
                    vm_track_write(dst, bytes);
                return;
            }
        }
        for (int32_t i = 0; i < count; i++) {
            const uint32_t off = stride * (uint32_t)i;
            vm_write32_mm(&m, dst + off, vm_read32_mm(&m, src + off));
        }
        return;
    }
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
    const ElementBox box = element_box(m, self);
    bool framed = false;
    for (;;) {
        const uint32_t node = vm_read32_mm(&m, self + 0x1940);
        int32_t index = (int32_t)vm_read32_mm(&m, self + 0x1944);
        if (index >= 0) {
            const int32_t count = (int32_t)vm_read32_mm(&m, node + 4);
            if (index < count) {
                const int32_t hit = box.first(vm_read32_mm(&m, node), index, count);
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

namespace {

inline uint32_t fast32(const ppu_mm& m, uint32_t a) { return vm_fast_read32(m, a); }

inline uint16_t fast16(const ppu_mm& m, uint32_t a)
{
    if (__builtin_expect(vm_mm_ok(&m, a, 2), 1)) {
        uint16_t x;
        std::memcpy(&x, m.vb + a, 2);
        return __builtin_bswap16(x);
    }
    return vm_read16_mm(&m, a);
}

inline uint8_t fast8(const ppu_mm& m, uint32_t a)
{
    if (__builtin_expect(vm_mm_ok(&m, a, 1), 1)) return m.vb[a];
    return vm_read8_mm(&m, a);
}

inline bool gcm_reserve(const ppu_mm& m, ppu_context* ctx, uint32_t gcm_ctx, uint32_t words, uint32_t lr_site, uint32_t& cur)
{
    cur = fast32(m, gcm_ctx + 8u);
    const uint32_t end = fast32(m, gcm_ctx + 4u);
    if (__builtin_expect(cur + words * 4u <= end, 1)) return true;
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    frame(m, sp, 0xB0, lr);
    ctx->gpr[3] = gcm_ctx;
    ctx->gpr[4] = words;
    const uint32_t cb = fast32(m, gcm_ctx + 0xCu);
    ctx->ctr = fast32(m, cb);
    ctx->gpr[2] = fast32(m, cb + 4u);
    call(ctx, sp - 0xB0, lr_site, guest_icall);
    ctx->gpr[2] = 0x019AF8F8ULL;
    ctx->gpr[1] = sp;
    ctx->lr = lr;
    if ((int32_t)ctx->gpr[3] != 0) return false;
    cur = vm_read32_mm(&m, gcm_ctx + 8u);
    return true;
}

inline void gcm_put_words(const ppu_mm& m, uint32_t ea, const uint32_t* w, uint32_t n)
{
    const uint32_t bytes = n * 4u;
    if (__builtin_expect(plain_store(m, ea, bytes) && !vm_watched(ea, bytes), 1)) {
        uint32_t* dst = reinterpret_cast<uint32_t*>(m.vb + ea);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t v = __builtin_bswap32(w[i]);
            std::memcpy(dst + i, &v, 4);
        }
        vm_track_write(ea, bytes);
        return;
    }
    uint32_t tmp[16];
    for (uint32_t i = 0; i < n; ++i) tmp[i] = __builtin_bswap32(w[i]);
    ppu_store_block(ea, tmp, bytes);
}

inline bool registered(const ppu_mm& m, uint32_t obj) { return ue3::uobject_registered(m, obj); }

struct PropCache {
    uint32_t s, s_outer, f, f_outer, f_cls;
    uint64_t s_name, want;
};
thread_local PropCache g_prop_cache[512];

}

void func_001B7848(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t prim = (uint32_t)ctx->gpr[3];
    if ((fast32(m, prim + 0x12Cu) & 0x40000000u) == 0) {
        ctx->gpr[3] = 0;
        return;
    }
    const uint32_t actor = fast32(m, prim + 0x4Cu);
    ctx->gpr[3] = (!actor || (fast32(m, actor + 0xB0u) & 0x00000010u) != 0) ? 1u : 0u;
}

void func_00F6399C(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    const uint64_t want = ctx->gpr[4];
    const uint32_t s0 = (uint32_t)ctx->gpr[3];
    uint64_t s_name = 0;
    uint32_t s_outer = 0;
    if (s0) {
        s_name = ((uint64_t)fast32(m, s0 + 0x2Cu) << 32) | fast32(m, s0 + 0x30u);
        s_outer = fast32(m, s0 + 0x28u);
        const uint32_t pi = ((s0 >> 4) ^ ((uint32_t)(want >> 32) * 0x9E3779B1u >> 23)) & 511u;
        const PropCache& pc = g_prop_cache[pi];
        if (__builtin_expect(pc.s == s0 && pc.want == want && pc.f != 0 &&
                             pc.s_name == s_name && pc.s_outer == s_outer &&
                             registered(m, pc.f) &&
                             (((uint64_t)fast32(m, pc.f + 0x2Cu) << 32) | fast32(m, pc.f + 0x30u)) == want &&
                             fast32(m, pc.f + 0x28u) == pc.f_outer &&
                             fast32(m, pc.f + 0x34u) == pc.f_cls, 1)) {
            ctx->gpr[3] = pc.f;
            return;
        }
    }
    uint32_t s = s0;
    uint32_t f = s ? fast32(m, s + 0x44u) : 0u;
    bool framed = false;

    auto next_valid = [&]() {
        while (s) {
            while (f) {
                const uint32_t cls = fast32(m, f + 0x34u);
                if ((fast32(m, cls + 0xC0u) & 0x00008000u) != 0) return;
                f = fast32(m, f + 0x3Cu);
            }
            if (!framed) {
                frame(m, sp, 0xA0, lr);
                framed = true;
            }
            ctx->gpr[3] = s;
            ctx->ctr = fast32(m, fast32(m, fast32(m, s) + 0x130u));
            call(ctx, sp - 0xA0, 0x00F63A28, guest_icall);
            s = (uint32_t)ctx->gpr[3];
            if (s) f = fast32(m, s + 0x44u);
        }
    };

    next_valid();
    while (f) {
        uint64_t name;
        if (__builtin_expect((int32_t)fast32(m, f + 0x20u) != -1, 1)) {
            name = ((uint64_t)fast32(m, f + 0x2Cu) << 32) | fast32(m, f + 0x30u);
        } else {
            if (!framed) {
                frame(m, sp, 0xA0, lr);
                framed = true;
            }
            const uint32_t tmp_ea = (uint32_t)(sp - 0xA0) + 0x70u;
            ctx->gpr[3] = tmp_ea;
            ctx->gpr[4] = 0x01691EECu;
            ctx->gpr[5] = 1;
            ctx->gpr[6] = 1;
            call(ctx, sp - 0xA0, 0x00F63A70, func_000A4148);
            name = ((uint64_t)vm_read32_mm(&m, tmp_ea) << 32) | vm_read32_mm(&m, tmp_ea + 4u);
        }
        if (name == want) {
            if (s0 && registered(m, f)) {
                const uint32_t pi = ((s0 >> 4) ^ ((uint32_t)(want >> 32) * 0x9E3779B1u >> 23)) & 511u;
                g_prop_cache[pi] = { s0, s_outer, f, fast32(m, f + 0x28u), fast32(m, f + 0x34u), s_name, want };
            }
            ctx->gpr[1] = sp;
            ctx->lr = lr;
            ctx->gpr[3] = f;
            return;
        }
        f = fast32(m, f + 0x3Cu);
        next_valid();
    }
    ctx->gpr[1] = sp;
    ctx->lr = lr;
    ctx->gpr[3] = 0;
}

void func_005BCE0C(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    const uint32_t dst = (uint32_t)ctx->gpr[3];
    const uint32_t obj = fast32(m, (uint32_t)ctx->gpr[4] + 0x60u);
    frame(m, sp, 0x100, lr);
    const uint32_t call_sp = (uint32_t)(sp - 0x100);
    const uint32_t tmp_b0 = call_sp + 0xB0u;
    const uint32_t tmp_70 = call_sp + 0x70u;
    ctx->gpr[3] = tmp_b0;
    ctx->gpr[4] = obj;
    ctx->ctr = fast32(m, fast32(m, fast32(m, obj) + 0x28u));
    call(ctx, call_sp, 0x005BCE4C, guest_icall);
    ctx->gpr[3] = tmp_70;
    ctx->gpr[4] = tmp_b0;
    call(ctx, call_sp, 0x005BCE58, func_005AEF20);
    if (__builtin_expect(vm_mm_ok(&m, tmp_70, 64) && plain_store(m, dst, 64), 1)) {
        std::memcpy(m.vb + dst, m.vb + tmp_70, 64);
        vm_track_write(dst, 64);
    } else {
        ppu_store_block(dst, m.vb + tmp_70, 64);
    }
    ctx->gpr[1] = sp;
    ctx->lr = lr;
}

void func_00CF08F0(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t gcm_ctx = (uint32_t)ctx->gpr[3];
    uint32_t load_idx = (uint32_t)ctx->gpr[4];
    const uint32_t total_floats = ((uint32_t)ctx->gpr[5]) << 2;
    uint32_t src = (uint32_t)ctx->gpr[6];
    const uint32_t rem = total_floats & 31u;
    const uint32_t blocks = total_floats >> 5;
    const uint32_t words = blocks * 34u + (rem ? rem + 2u : 0u);
    uint32_t cur;
    if (!gcm_reserve(m, ctx, gcm_ctx, words & ~1u, 0x00CF0994, cur)) return;

    const uint32_t total_bytes = words * 4u;
    const uint32_t src_bytes = total_floats * 4u;
    const bool fast = total_bytes > 0 && plain_store(m, cur, total_bytes) && !vm_watched(cur, total_bytes) && vm_mm_ok(&m, src, src_bytes);
    const uint32_t cur0 = cur;
    for (uint32_t b = 0; b < blocks; ++b) {
        const uint32_t hdr[2] = { __builtin_bswap32(0x00841EFCu), __builtin_bswap32(load_idx) };
        if (__builtin_expect(fast, 1)) {
            std::memcpy(m.vb + cur, hdr, 8);
            std::memcpy(m.vb + cur + 8u, m.vb + src, 128);
        } else {
            ppu_store_block(cur, hdr, 8);
            ppu_store_block(cur + 8u, m.vb + src, 128);
        }
        cur += 0x88u;
        src += 128u;
        load_idx += 8u;
    }
    if (rem) {
        const uint32_t hdr[2] = { __builtin_bswap32(((rem + 1u) << 18) | 0x1EFCu), __builtin_bswap32(load_idx) };
        const uint32_t rem_bytes = (rem & ~3u) * 4u;
        if (__builtin_expect(fast, 1)) {
            std::memcpy(m.vb + cur, hdr, 8);
            if (rem_bytes) std::memcpy(m.vb + cur + 8u, m.vb + src, rem_bytes);
        } else {
            ppu_store_block(cur, hdr, 8);
            if (rem_bytes) ppu_store_block(cur + 8u, m.vb + src, rem_bytes);
        }
        cur += 8u + rem_bytes;
    }
    if (__builtin_expect(fast, 1)) vm_track_write(cur0, total_bytes);
    vm_write32_mm(&m, gcm_ctx + 8u, cur);
}

void func_00CF0BD0(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t gcm_ctx = (uint32_t)ctx->gpr[3];
    const uint32_t idx = (uint32_t)ctx->gpr[4] & 0xFFu;
    const uint32_t freq = (uint32_t)ctx->gpr[5];
    const uint32_t stride = (uint32_t)ctx->gpr[6];
    const uint32_t size = (uint32_t)ctx->gpr[7];
    const uint32_t type = (uint32_t)ctx->gpr[8];
    const uint32_t loc = (uint32_t)ctx->gpr[9];
    const uint32_t off = (uint32_t)ctx->gpr[10];
    uint32_t cur;
    if (!gcm_reserve(m, ctx, gcm_ctx, 4u, 0x00CF0CD8, cur)) return;
    const uint32_t w[4] = {
        0x00040000u | (0x1740u + (idx << 2)),
        ((freq & 0xFFFFu) << 16) | ((stride & 0xFFu) << 8) | ((size & 0xFFu) << 4) | (type & 0xFFu),
        0x00040000u | (0x1680u + (idx << 2)),
        ((loc & 1u) << 31) | off
    };
    gcm_put_words(m, cur, w, 4u);
    vm_write32_mm(&m, gcm_ctx + 8u, cur + 0x10u);
}

void func_00CF3758(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t gcm_ctx = (uint32_t)ctx->gpr[3];
    const uint32_t idx = (uint32_t)ctx->gpr[4] & 0xFFu;
    const uint32_t tex = (uint32_t)ctx->gpr[5];
    const uint32_t border = (uint32_t)ctx->gpr[6];
    uint32_t cur;
    if (!gcm_reserve(m, ctx, gcm_ctx, 9u, 0x00CF38D4, cur)) return;
    const uint8_t fmt = fast8(m, tex);
    const uint8_t mip = fast8(m, tex + 1u);
    const uint8_t dim = fast8(m, tex + 2u);
    const uint8_t cube = fast8(m, tex + 3u);
    const uint32_t remap = fast32(m, tex + 4u);
    const uint16_t width = fast16(m, tex + 8u);
    const uint16_t height = fast16(m, tex + 0xAu);
    const uint16_t depth = fast16(m, tex + 0xCu);
    const uint8_t loc = fast8(m, tex + 0xEu);
    const uint32_t pitch = fast32(m, tex + 0x10u);
    const uint32_t offset = fast32(m, tex + 0x14u);
    const uint32_t idx5 = idx << 5;
    const uint32_t w[9] = {
        0x00080000u | (0x1A00u + idx5),
        offset,
        ((border & 0xFFu) << 3) | ((uint32_t)fmt << 8) | ((uint32_t)mip << 16) | ((uint32_t)dim << 4) | ((uint32_t)cube << 2) | ((uint32_t)loc + 1u),
        0x00040000u | (0x1A18u + idx5),
        ((uint32_t)width << 16) | height,
        0x00040000u | (0x1840u + (idx << 2)),
        ((uint32_t)depth << 20) | pitch,
        0x00040000u | (0x1A10u + idx5),
        remap
    };
    gcm_put_words(m, cur, w, 9u);
    vm_write32_mm(&m, gcm_ctx + 8u, cur + 0x24u);
}

void func_00CEF93C(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t gcm_ctx = (uint32_t)ctx->gpr[3];
    const uint32_t idx = (uint32_t)ctx->gpr[4] & 0xFFu;
    const uint32_t en = (uint32_t)ctx->gpr[5];
    const uint32_t minlod = (uint32_t)ctx->gpr[6];
    const uint32_t maxlod = (uint32_t)ctx->gpr[7];
    const uint32_t maxaniso = (uint32_t)ctx->gpr[8];
    uint32_t cur;
    if (!gcm_reserve(m, ctx, gcm_ctx, 2u, 0x00CEFA0C, cur)) return;
    const uint32_t w[2] = {
        0x00040000u | (0x1A0Cu + (idx << 5)),
        ((en & 1u) << 31) | ((minlod & 0xFFFu) << 19) | ((maxlod & 0xFFFu) << 7) | ((maxaniso & 0xFFu) << 4)
    };
    gcm_put_words(m, cur, w, 2u);
    vm_write32_mm(&m, gcm_ctx + 8u, cur + 8u);
}

void func_00CEFE7C(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t gcm_ctx = (uint32_t)ctx->gpr[3];
    const uint32_t idx = (uint32_t)ctx->gpr[4] & 0xFFu;
    const uint32_t bias = (uint32_t)ctx->gpr[5];
    const uint32_t minf = (uint32_t)ctx->gpr[6];
    const uint32_t magf = (uint32_t)ctx->gpr[7];
    const uint32_t conv = (uint32_t)ctx->gpr[8];
    uint32_t cur;
    if (!gcm_reserve(m, ctx, gcm_ctx, 2u, 0x00CEFF4C, cur)) return;
    const uint32_t w[2] = {
        0x00040000u | (0x1A14u + (idx << 5)),
        (bias & 0x1FFFu) | ((minf & 0xFFu) << 16) | ((magf & 0xFFu) << 24) | ((conv & 0xFFu) << 13)
    };
    gcm_put_words(m, cur, w, 2u);
    vm_write32_mm(&m, gcm_ctx + 8u, cur + 8u);
}

extern "C" uint64_t ppu_timebase_now(void);

namespace {

inline void archive_copy_advance(const ppu_mm& m, ppu_context* ctx, uint32_t self, uint32_t dst, uint32_t src,
                                 int32_t pos, int32_t len, uint32_t pos_off, uint32_t fallback_lr)
{
    const uint32_t ulen = (uint32_t)len;
    if (__builtin_expect(vm_mm_ok(&m, src, ulen) && plain_store(m, dst, ulen) && !vm_watched(dst, ulen), 1)) {
        std::memmove(m.vb + dst, m.vb + src, ulen);
        vm_track_write(dst, ulen);
    } else {
        ctx->gpr[3] = dst;
        ctx->gpr[4] = src;
        ctx->gpr[5] = ulen;
        ctx->lr = fallback_lr;
        run(func_00F297C8, ctx);
    }
    vm_fast_write32(m, self + pos_off, (uint32_t)(pos + len));
}

inline void update_attached_component(const ppu_mm& m, ppu_context* ctx, uint32_t call_sp, uint32_t actor,
                                      uint32_t scene, uint32_t mat_ea, uint32_t comp, uint32_t flags,
                                      uint32_t lr_detach, uint32_t lr_reattach, uint32_t lr_xform, uint32_t lr_post)
{
    if (flags & 0x20000000u) {
        ctx->gpr[3] = comp;
        ctx->gpr[4] = 1u;
        ctx->ctr = fast32(m, fast32(m, fast32(m, comp) + 0x134u));
        call(ctx, call_sp, lr_detach, guest_icall);
        vm_write32_mm(&m, comp + 0x48u, 0u);
        vm_write32_mm(&m, comp + 0x4Cu, 0u);
        ctx->gpr[3] = comp;
        ctx->gpr[4] = scene;
        ctx->gpr[5] = actor;
        ctx->gpr[6] = mat_ea;
        call(ctx, call_sp, lr_reattach, func_002294F4);
    } else if (flags & 0x10000000u) {
        vm_write32_mm(&m, comp + 0x50u, flags & ~0x10000000u);
        ctx->gpr[3] = comp;
        ctx->gpr[4] = mat_ea;
        ctx->ctr = fast32(m, fast32(m, fast32(m, comp) + 0x128u));
        call(ctx, call_sp, lr_xform, guest_icall);
        if (fast32(m, comp + 0x50u) & 0x80000000u) {
            ctx->gpr[3] = comp;
            ctx->ctr = fast32(m, fast32(m, fast32(m, comp) + 0x130u));
            call(ctx, call_sp, lr_post, guest_icall);
        }
    }
}

}

void func_00F60120(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t self = (uint32_t)ctx->gpr[3];
    const uint32_t dst = (uint32_t)ctx->gpr[4];
    const int32_t len = (int32_t)ctx->gpr[5];
    if (len == 0 || fast32(m, self + 0x34u) != 0) return;
    const uint32_t arr = fast32(m, self + 0x88u);
    const int32_t pos = (int32_t)fast32(m, self + 0x84u);
    const int32_t size = (int32_t)fast32(m, arr + 4u);
    if (pos + len > size) {
        vm_write32_mm(&m, self + 0x34u, 1u);
        return;
    }
    const uint32_t src = fast32(m, arr) + (uint32_t)pos;
    archive_copy_advance(m, ctx, self, dst, src, pos, len, 0x84u, 0x00F601A4);
}

void func_00035338(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t self = (uint32_t)ctx->gpr[3];
    const uint32_t dst = (uint32_t)ctx->gpr[4];
    const int32_t len = (int32_t)ctx->gpr[5];
    const int32_t pos = (int32_t)fast32(m, self + 0x98u);
    const int32_t buf_pos = (int32_t)fast32(m, self + 0x9Cu);
    const int32_t buf_end = (int32_t)fast32(m, self + 0xA4u);
    if (__builtin_expect(pos >= buf_pos && pos + len <= buf_end && fast32(m, self + 0xB4u) == 0, 1)) {
        const uint32_t src = fast32(m, self + 0xACu) + (uint32_t)(pos - buf_pos);
        archive_copy_advance(m, ctx, self, dst, src, pos, len, 0x98u, 0x00035474);
        return;
    }

    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    frame(m, sp, 0xA0, lr);
    const uint32_t call_sp = (uint32_t)(sp - 0xA0);
    const float sleep_s = from_bits(fast32(m, 0x0002E050u));
    int waited = 0;
    if (pos < buf_pos || pos + len > buf_end) {
        while ((int32_t)ppu_timebase_now() == 0) {}
        waited = 1;
        for (;;) {
            ctx->gpr[3] = self;
            ctx->gpr[4] = (int64_t)(int32_t)vm_read32_mm(&m, self + 0x98u);
            ctx->gpr[5] = (int64_t)len;
            ctx->ctr = fast32(m, fast32(m, fast32(m, self) + 0x44u));
            call(ctx, call_sp, 0x00035428, guest_icall);
            if ((int32_t)ctx->gpr[3] != 0) break;
            if (vm_read32_mm(&m, 0x019EECD8u) != 0) call(ctx, call_sp, 0x00035444, func_0089B1D8);
            ctx->fpr[1] = sleep_s;
            call(ctx, call_sp, 0x0003544C, func_008A9458);
        }
    }
    while (vm_read32_mm(&m, self + 0xB4u) != 0) {
        if (vm_read32_mm(&m, 0x019EECD8u) != 0) call(ctx, call_sp, 0x000353B4, func_0089B1D8);
        if (!waited) {
            while ((int32_t)ppu_timebase_now() == 0) {}
            waited = 1;
        }
        ctx->fpr[1] = sleep_s;
        call(ctx, call_sp, 0x000353E0, func_008A9458);
    }
    const int32_t cur_pos = (int32_t)vm_read32_mm(&m, self + 0x98u);
    const int32_t cur_bpos = (int32_t)vm_read32_mm(&m, self + 0x9Cu);
    ctx->gpr[3] = dst;
    ctx->gpr[4] = vm_read32_mm(&m, self + 0xACu) + (uint32_t)(cur_pos - cur_bpos);
    ctx->gpr[5] = (uint32_t)len;
    call(ctx, call_sp, 0x00035474, func_00F297C8);
    vm_write32_mm(&m, self + 0x98u, (uint32_t)(cur_pos + len));
    ctx->gpr[1] = sp;
    ctx->lr = lr;
}

void func_0009C6D0(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t inner = fast32(m, (uint32_t)ctx->gpr[3] + 0x498u);
    ctx->gpr[3] = inner;
    ctx->ctr = fast32(m, fast32(m, fast32(m, inner) + 8u));
    guest_icall(ctx);
}

void func_0009C8C0(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint64_t sp = ctx->gpr[1], lr = ctx->lr;
    const uint32_t self = (uint32_t)ctx->gpr[3];
    const uint32_t out_name = (uint32_t)ctx->gpr[4];
    const uint32_t ar = self + 0x16Cu;
    frame(m, sp, 0xA0, lr);
    const uint32_t call_sp = (uint32_t)(sp - 0xA0);
    const uint32_t tmp_idx = call_sp + 0x70u;
    ctx->gpr[3] = ar;
    ctx->gpr[4] = tmp_idx;
    ctx->gpr[5] = 4u;
    ctx->ctr = fast32(m, fast32(m, fast32(m, ar) + 8u));
    call(ctx, call_sp, 0x0009C908, guest_icall);
    int32_t idx = (int32_t)fast32(m, tmp_idx);
    const int32_t map_num = (int32_t)fast32(m, self + 0xE8u);
    if (__builtin_expect(idx < 0 || idx >= map_num, 0)) {
        ctx->gpr[3] = fast32(m, 0x019B1B84u);
        ctx->gpr[4] = 0x0160C518u;
        ctx->gpr[5] = (int64_t)idx;
        ctx->gpr[6] = (int64_t)map_num;
        call(ctx, call_sp, 0x0009C9F8, func_0009DB34);
        idx = (int32_t)fast32(m, tmp_idx);
    }
    const uint32_t map_data = fast32(m, self + 0xE4u);
    const uint32_t entry_ea = map_data + ((uint32_t)idx << 3);
    uint64_t entry;
    if (__builtin_expect(vm_mm_ok(&m, entry_ea, 8), 1)) {
        std::memcpy(&entry, m.vb + entry_ea, 8);
        entry = __builtin_bswap64(entry);
    } else {
        entry = vm_read64_mm(&m, entry_ea);
    }
    if (entry == 0) {
        ctx->gpr[3] = ar;
        ctx->gpr[4] = call_sp + 0x74u;
        ctx->gpr[5] = 4u;
        ctx->ctr = fast32(m, fast32(m, fast32(m, ar) + 8u));
        call(ctx, call_sp, 0x0009C960, guest_icall);
        if (__builtin_expect(plain_store(m, out_name, 8), 1)) {
            std::memset(m.vb + out_name, 0, 8);
            vm_track_write(out_name, 8);
        } else {
            vm_write32_mm(&m, out_name, 0u);
            vm_write32_mm(&m, out_name + 4u, 0u);
        }
    } else {
        const uint32_t tmp_num = call_sp + 0x78u;
        ctx->gpr[3] = ar;
        ctx->gpr[4] = tmp_num;
        ctx->gpr[5] = 4u;
        ctx->ctr = fast32(m, fast32(m, fast32(m, ar) + 8u));
        call(ctx, call_sp, 0x0009C9AC, guest_icall);
        const uint32_t name_idx = fast32(m, fast32(m, self + 0xE4u) + (fast32(m, tmp_idx) << 3));
        const uint32_t number = fast32(m, tmp_num);
        if (__builtin_expect(plain_store(m, out_name, 8), 1)) {
            const uint32_t pair[2] = { __builtin_bswap32(name_idx), __builtin_bswap32(number) };
            std::memcpy(m.vb + out_name, pair, 8);
            vm_track_write(out_name, 8);
        } else {
            vm_write32_mm(&m, out_name + 4u, number);
            vm_write32_mm(&m, out_name, name_idx);
        }
    }
    ctx->gpr[3] = ar;
    ctx->gpr[1] = sp;
    ctx->lr = lr;
}

void func_00CEFF60(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t gcm_ctx = (uint32_t)ctx->gpr[3];
    const uint32_t idx = (uint32_t)ctx->gpr[4] & 0xFFu;
    const uint32_t wraps = (uint32_t)ctx->gpr[5];
    const uint32_t wrapt = (uint32_t)ctx->gpr[6];
    const uint32_t wrapr = (uint32_t)ctx->gpr[7];
    const uint32_t remap = (uint32_t)ctx->gpr[8];
    const uint32_t zfunc = (uint32_t)ctx->gpr[9];
    const uint32_t gamma = (uint32_t)ctx->gpr[10];
    uint32_t cur;
    if (!gcm_reserve(m, ctx, gcm_ctx, 2u, 0x00CF0058, cur)) return;
    const uint32_t w[2] = {
        0x00040000u | (0x1A08u + (idx << 5)),
        (wraps & 0xFFu) | ((wrapt & 0xFFu) << 8) | ((wrapr & 0xFFu) << 16) | ((remap & 0xFFu) << 12) | ((zfunc & 0xFu) << 28) | ((gamma & 0xFFu) << 20)
    };
    gcm_put_words(m, cur, w, 2u);
    vm_write32_mm(&m, gcm_ctx + 8u, cur + 8u);
}

void func_002392C8(ppu_context* ctx)
{
    const ppu_mm m = ppu_mm_take();
    const uint32_t actor = (uint32_t)ctx->gpr[3];
    const uint32_t b_collision_only = (uint32_t)ctx->gpr[4];
    const uint64_t sp = ctx->gpr[1];
    const uint64_t lr = ctx->lr;
    const uint32_t call_sp = (uint32_t)(sp - 0x1B0u);
    const uint32_t mat_ea = call_sp + 0x70u;
    frame(m, sp, 0x1B0u, lr);

    const int32_t pitch = (int32_t)fast32(m, actor + 0x60u);
    const int32_t yaw   = (int32_t)fast32(m, actor + 0x64u);
    const int32_t roll  = (int32_t)fast32(m, actor + 0x68u);
    const uint32_t sin_tab = 0x019EECDCu;
    auto sin_lookup = [&](int32_t angle) -> float {
        return from_bits(fast32(m, sin_tab + (((uint32_t)(angle >> 2) & 0x3FFFu) << 2)));
    };
    const float sp_f = sin_lookup(pitch);
    const float cp_f = sin_lookup(pitch + 0x4000);
    const float sy_f = sin_lookup(yaw);
    const float cy_f = sin_lookup(yaw + 0x4000);
    const float sr_f = sin_lookup(roll);
    const float cr_f = sin_lookup(roll + 0x4000);

    const float ds = vm_read_f32_mm(m, actor + 0x6Cu);
    const float sx = vm_read_f32_mm(m, actor + 0x70u) * ds;
    const float sy = vm_read_f32_mm(m, actor + 0x74u) * ds;
    const float sz = vm_read_f32_mm(m, actor + 0x78u) * ds;
    const float px = vm_read_f32_mm(m, actor + 0x7Cu);
    const float py = vm_read_f32_mm(m, actor + 0x80u);
    const float pz = vm_read_f32_mm(m, actor + 0x84u);
    const float lx = vm_read_f32_mm(m, actor + 0x54u);
    const float ly = vm_read_f32_mm(m, actor + 0x58u);
    const float lz = vm_read_f32_mm(m, actor + 0x5Cu);

    const float m00 = cp_f * cy_f * sx;
    const float m01 = cp_f * sy_f * sx;
    const float m02 = sp_f * sx;
    const float m10 = (sr_f * sp_f * cy_f - cr_f * sy_f) * sy;
    const float m11 = (sr_f * sp_f * sy_f + cr_f * cy_f) * sy;
    const float m12 = -(sr_f * cp_f) * sy;
    const float m20 = -(cr_f * sp_f * cy_f + sr_f * sy_f) * sz;
    const float m21 = (cy_f * sr_f - cr_f * sp_f * sy_f) * sz;
    const float m22 = cr_f * cp_f * sz;
    const float m30 = lx - (px * m00 + py * m10 + pz * m20);
    const float m31 = ly - (px * m01 + py * m11 + pz * m21);
    const float m32 = lz - (px * m02 + py * m12 + pz * m22);

    const float mat_f[16] = {
        m00, m01, m02, 0.0f,
        m10, m11, m12, 0.0f,
        m20, m21, m22, 0.0f,
        m30, m31, m32, 1.0f,
    };
    uint32_t mat_be[16];
    for (int i = 0; i < 16; i++) mat_be[i] = __builtin_bswap32(to_bits(mat_f[i]));
    if (__builtin_expect(plain_store(m, mat_ea, 64), 1)) {
        std::memcpy(m.vb + mat_ea, mat_be, 64);
        vm_track_write(mat_ea, 64);
    } else {
        for (int i = 0; i < 16; i++) vm_write32_mm(&m, mat_ea + (uint32_t)i * 4u, to_bits(mat_f[i]));
    }

    if (b_collision_only == 0) {
        for (int32_t i = 0; i < (int32_t)fast32(m, actor + 0x40u); i++) {
            const uint32_t data = fast32(m, actor + 0x3Cu);
            const uint32_t comp = fast32(m, data + (uint32_t)i * 4u);
            if (!comp) continue;
            const uint32_t gworld = fast32(m, 0x019CCE80u);
            const uint32_t scene = fast32(m, gworld + 0x40u);
            const uint32_t flags = fast32(m, comp + 0x50u);
            if ((flags & 0x80000000u) == 0) {
                ctx->gpr[3] = comp;
                ctx->gpr[4] = scene;
                ctx->gpr[5] = actor;
                ctx->gpr[6] = mat_ea;
                call(ctx, call_sp, 0x0023979C, func_002294F4);
            } else {
                update_attached_component(m, ctx, call_sp, actor, scene, mat_ea, comp, flags,
                                          0x002397E8, 0x00239804, 0x0023983C, 0x00239864);
            }
            ctx->gpr[3] = comp;
            ctx->ctr = fast32(m, fast32(m, fast32(m, comp) + 0x140u));
            call(ctx, call_sp, 0x002397B4, guest_icall);
        }
    } else {
        for (uint32_t i = 0; i < fast32(m, actor + 0x40u); i++) {
            const uint32_t data = fast32(m, actor + 0x3Cu);
            const uint32_t comp = fast32(m, data + i * 4u);
            if (!comp) continue;
            uint32_t prim_cls = fast32(m, 0x019C2E98u);
            if (!prim_cls) {
                ctx->gpr[3] = 0x01691EA8u;
                call(ctx, call_sp, 0x002399D4, func_001B74B0);
                vm_write32_mm(&m, 0x019C2E98u, (uint32_t)ctx->gpr[3]);
                call(ctx, call_sp, 0x002399E0, func_001B7400);
                prim_cls = fast32(m, 0x019C2E98u);
            }
            if (prim_cls) {
                uint32_t cls = fast32(m, comp + 0x34u);
                while (cls && cls != prim_cls) cls = fast32(m, cls + 0x40u);
                if (!cls) continue;
            }
            const uint32_t flags = fast32(m, comp + 0x50u);
            if ((flags & 0x80000000u) == 0) continue;
            if (comp != fast32(m, actor + 0x188u) && (fast32(m, comp + 0x12Cu) & 0x20000000u) == 0) continue;
            const uint32_t gworld = fast32(m, 0x019CCE80u);
            const uint32_t scene = fast32(m, gworld + 0x40u);
            update_attached_component(m, ctx, call_sp, actor, scene, mat_ea, comp, flags,
                                      0x002399AC, 0x002399C8, 0x00239964, 0x0023998C);
        }
    }
    ctx->gpr[1] = sp;
    ctx->lr = lr;
}

namespace {

constexpr uint32_t k_shadow_border = 0x016171A8;
constexpr uint32_t k_max_shadow_x = 0x019C2F6C;
constexpr uint32_t k_max_dom_shadow = 0x019C2F74;
constexpr uint32_t k_current_time = 0x019FED10;

struct ShadowLevel {
    uint32_t key;
    int32_t level;
    double seen;
};
thread_local ShadowLevel t_shadow_levels[256];

uint32_t stable_shadow_resolution(uint32_t key, uint32_t desired, uint32_t cap)
{
    const int32_t wanted = (int32_t)std::ceil(4.0 * std::log2((double)desired) - 1e-9);
    ShadowLevel& slot = t_shadow_levels[(key * 0x9E3779B1u) >> 24];
    const double now = from_bits64(vm_read64(k_current_time));
    int32_t level = wanted;
    if (slot.key == key && now >= slot.seen && now - slot.seen < 1.0 && (wanted == slot.level || wanted == slot.level - 1))
        level = slot.level;
    slot = { key, level, now };
    return std::min(cap, (uint32_t)std::lround(std::exp2(level / 4.0)));
}

}

void func_006A1488(ppu_context* ctx)
{
    const uint32_t desired = (uint32_t)ctx->gpr[9];
    if (desired >= 16u && desired == (uint32_t)ctx->gpr[10]) {
        const uint32_t border = 2u * vm_read32(k_shadow_border);
        const uint32_t max_res = std::min(vm_read32(k_max_shadow_x), vm_read32(k_max_dom_shadow));
        if (max_res > border) {
            const uint32_t key = ((uint32_t)ctx->gpr[5] << 1) | ((uint32_t)ctx->gpr[8] & 1u);
            ctx->gpr[9] = ctx->gpr[10] = stable_shadow_resolution(key, desired, max_res - border);
        }
    }
    func_006A1488_lifted(ctx);
}
