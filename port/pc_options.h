#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

extern std::atomic<uint64_t> g_pc_options_hooks[64];

void pc_options_boot(const std::filesystem::path& dir);
void pc_options_link(uint32_t function, const std::string& path);
uint32_t pc_options_call_hook(uint32_t hook, uint32_t self);
static inline uint32_t pc_options_call(uint32_t self, uint32_t function)
{
    if (__builtin_expect(!function, 0)) return 0;
    uint32_t i = (function * 0x9E3779B1u) >> 26;
    for (;;) {
        const uint64_t h = g_pc_options_hooks[i].load(std::memory_order_acquire);
        if (__builtin_expect(!h, 1)) return 0;
        if (__builtin_expect((uint32_t)(h >> 32) == function, 0))
            return pc_options_call_hook((uint32_t)h, self);
        i = (i + 1u) & 63u;
    }
}
void pc_options_returned(uint32_t hook, uint32_t result);
void pc_options_movie(uint32_t name);
bool pc_options_controller_screen();
uint32_t pc_options_stick_lit();
void pc_options_mouse_scale(float* yaw, float* pitch);
