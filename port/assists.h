#pragma once

#include <atomic>
#include <cstdint>
#include <string>

enum AssistFlag : uint32_t {
    ASSIST_GOD_MODE      = 1u << 0,
    ASSIST_INF_BURST     = 1u << 1,
    ASSIST_INF_UNLIMITED = 1u << 2,
    ASSIST_ONE_HIT_KILL  = 1u << 3,
    ASSIST_DAMAGE_MULT   = 1u << 4,
    ASSIST_AUTO_QTE      = 1u << 5,
    ASSIST_HOLD_MASH     = 1u << 6,
    ASSIST_EXT_QTE_TIMER = 1u << 7,
    ASSIST_UNLOCK_EPISODES = 1u << 8,
    ASSIST_UNLOCK_GAUGES = 1u << 9,
    ASSIST_UNLOCK_EXTRAS = 1u << 10,
};

enum AssistAction : uint32_t {
    ASSIST_ACT_NONE = 0,
    ASSIST_ACT_POST = 1u << 0,
    ASSIST_ACT_SKIP = 1u << 1,
};

extern std::atomic<uint64_t> g_assists_hooks[256];

void assists_set_flag(AssistFlag flag, bool on);
void assists_set_damage_mult(uint32_t mult);
void assists_set_qte_timer_pct(uint32_t pct);
void assists_sync_casual();

void assists_link(uint32_t function, const std::string& path);

uint32_t assists_pre_hook(uint32_t hk, uint32_t self, uint32_t function, uint32_t locals, uint32_t result);
void assists_post_slow(uint32_t token, uint32_t self, uint32_t function, uint32_t locals, uint32_t result);

static inline uint32_t assists_pre(uint32_t self, uint32_t function, uint32_t locals, uint32_t result)
{
    if (__builtin_expect(!function, 0))
        return 0;
    uint32_t i = (function * 0x9E3779B1u) >> 24;
    for (;;) {
        const uint64_t h = g_assists_hooks[i].load(std::memory_order_acquire);
        if (__builtin_expect(!h, 1))
            return 0;
        if (__builtin_expect((uint32_t)(h >> 32) == function, 0))
            return assists_pre_hook((uint32_t)h, self, function, locals, result);
        i = (i + 1u) & 255u;
    }
}

static inline void assists_post(uint32_t token, uint32_t self, uint32_t function, uint32_t locals, uint32_t result)
{
    if (__builtin_expect((token & ASSIST_ACT_POST) != 0, 0))
        assists_post_slow(token, self, function, locals, result);
}
