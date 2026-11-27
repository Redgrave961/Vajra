#pragma once

#include "ppu_recomp.h"
#include "vm_page_map.h"

#include <bit>
#include <cstring>
#ifdef __AVX2__
#include <immintrin.h>
#endif

extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
extern "C" void ps3_indirect_call(ppu_context* ctx);
extern "C" const int32_t* ppu_exec_tab;
extern "C" uint32_t ppu_exec_lo, ppu_exec_n;
extern "C" uint64_t ppu_timebase_now(void);
extern "C" void ppu_thread_note_block(uint64_t tid);

double tick_scale();
bool frame_site(uint32_t stack);
void frame_float_multiply(ppu_context* ctx);
bool camera_assist_blocked(uint32_t function);
void note_stick_rotation();
int mouse_route();

inline float from_bits(uint32_t bits) { return std::bit_cast<float>(bits); }
inline uint32_t to_bits(float v) { return std::bit_cast<uint32_t>(v); }
inline double from_bits64(uint64_t bits) { return std::bit_cast<double>(bits); }
inline uint64_t to_bits64(double v) { return std::bit_cast<uint64_t>(v); }
inline double single(double d) { return (double)(float)d; }
inline uint32_t stfs(double d) { return to_bits((float)d); }

inline bool plain_store(const ppu_mm& m, uint32_t ea, uint32_t n)
{
    if (!vm_mm_in(m.st_span, ea, n) || (uint32_t)(ea - m.gcm) < 0x10000u || (uint32_t)(m.gcm - ea) < n) return false;
    if (!n) return true;
    if (const unsigned char* bits = g_spu_coh_bits) {
        const uint32_t p0 = ea >> 12;
        const uint32_t p1 = (ea + n - 1u) >> 12;
        if (__builtin_expect(p0 == p1, 1)) {
            if (__builtin_expect(!(ppu_slow_page[p0] & VM_PAGE_COHERENT), 1)) return true;
            for (uint32_t line = ea >> 7; line <= (ea + n - 1u) >> 7; line++)
                if ((bits[line >> 3] >> (line & 7)) & 1u) return false;
            return true;
        }
        uint32_t p = p0;
#ifdef __AVX2__
        const __m256i coh_mask = _mm256_set1_epi8((char)VM_PAGE_COHERENT);
        while (p + 31u <= p1) {
            const __m256i v = _mm256_loadu_si256((const __m256i*)(ppu_slow_page + p));
            if (__builtin_expect(!_mm256_testz_si256(v, coh_mask), 0)) break;
            p += 32u;
        }
#endif
        for (; p <= p1; ++p) {
            if (__builtin_expect(ppu_slow_page[p] & VM_PAGE_COHERENT, 0)) {
                const uint32_t lo = (p == p0 ? ea : p << 12) >> 7;
                const uint32_t hi = (p == p1 ? ea + n - 1u : (p << 12) + 4095u) >> 7;
                for (uint32_t line = lo; line <= hi; ++line)
                    if ((bits[line >> 3] >> (line & 7)) & 1u) return false;
            }
        }
    }
    return true;
}

inline uint32_t vm_fast_read32(const ppu_mm& m, uint32_t ea)
{
    uint32_t raw;
    std::memcpy(&raw, m.vb + ea, 4);
    return __builtin_bswap32(raw);
}

inline uint64_t vm_fast_read64(const ppu_mm& m, uint32_t ea)
{
    uint64_t raw;
    std::memcpy(&raw, m.vb + ea, 8);
    return __builtin_bswap64(raw);
}

inline void vm_fast_write32(const ppu_mm& m, uint32_t ea, uint32_t v)
{
    if (__builtin_expect((ea & 4095u) <= 4092u && !ppu_slow_page[ea >> 12], 1)) {
        const uint32_t be = __builtin_bswap32(v);
        std::memcpy(m.vb + ea, &be, 4);
        if (__builtin_expect(*(const volatile unsigned char*)&ppu_slow_page[ea >> 12], 0))
            ppu_store_recheck(ea);
        return;
    }
    vm_write32_mm(&m, ea, v);
}

inline float vm_read_f32(uint32_t ea) { return from_bits(vm_read32(ea)); }
inline void vm_write_f32(uint32_t ea, float v) { vm_write32(ea, to_bits(v)); }
inline float vm_read_f32_mm(const ppu_mm& m, uint32_t ea) { return from_bits(vm_fast_read32(m, ea)); }
inline double vm_read_f64_mm(const ppu_mm& m, uint32_t ea) { return from_bits64(vm_fast_read64(m, ea)); }
inline void vm_write_f32_mm(const ppu_mm& m, uint32_t ea, float v) { vm_fast_write32(m, ea, to_bits(v)); }

inline uint64_t vm_timebase_nonzero()
{
    uint64_t t;
    do t = ppu_timebase_now();
    while (!(uint32_t)t);
    return t;
}

inline void drain(ppu_context* ctx)
{
    while (g_trampoline_fn) {
        void (*next)(void*) = g_trampoline_fn;
        g_trampoline_fn = nullptr;
        next(ctx);
    }
}

inline void run(void (*body)(ppu_context*), ppu_context* ctx)
{
    body(ctx);
    drain(ctx);
}

inline void guest_icall(ppu_context* ctx)
{
    const uint32_t d = (uint32_t)ctx->ctr - ppu_exec_lo;
    const uint32_t i = (d >> 2) | (d << 30);
    if (__builtin_expect(i < ppu_exec_n && ppu_exec_tab[i], 1)) {
        const auto fn = (void (*)(ppu_context*))((const char*)ps3_indirect_call + ppu_exec_tab[i]);
        fn(ctx);
        drain(ctx);
        return;
    }
    ps3_indirect_call(ctx);
    drain(ctx);
}

inline void guest_call(ppu_context* ctx, uint32_t opd)
{
    ctx->ctr = vm_read32(opd);
    guest_icall(ctx);
}

struct GuestFrame {
    ppu_context* ctx;
    uint64_t caller;
    uint32_t base;

    GuestFrame(ppu_context* c, uint32_t size) : ctx(c), caller(c->gpr[1]), base((uint32_t)c->gpr[1] - size)
    {
        vm_write64(base, caller);
        ctx->gpr[1] = base;
    }

    ~GuestFrame() { ctx->gpr[1] = caller; }
};
