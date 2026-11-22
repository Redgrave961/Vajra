#include "guest.h"

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
