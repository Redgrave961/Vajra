#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

extern std::atomic<uint64_t> g_pc_options_hook_mask;

void pc_options_boot(const std::filesystem::path& dir);
void pc_options_link(uint32_t function, const std::string& path);
uint32_t pc_options_call_slow(uint32_t self, uint32_t function);
static inline uint32_t pc_options_call(uint32_t self, uint32_t function)
{
    const uint64_t bit = 1ull << ((function * 0x9E3779B1u) >> 26);
    if (__builtin_expect(!function || !(g_pc_options_hook_mask.load(std::memory_order_relaxed) & bit), 1)) return 0;
    return pc_options_call_slow(self, function);
}
void pc_options_returned(uint32_t hook, uint32_t result);
bool pc_options_controller_screen();
uint32_t pc_options_stick_lit();
void pc_options_mouse_scale(float* yaw, float* pitch);
