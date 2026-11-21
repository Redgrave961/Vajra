#include "button_prompts.h"
#include "vk/rsx_vk.h"
#include "../ps3recomp/libs/input/cellPad.h"

#include <cstring>

namespace {

constexpr uint64_t k_sheet_hash = 0xE4976F12660F1CA3ull;
constexpr uint64_t k_half = 16384;

uint32_t variant()
{
    return (uint32_t)cellPad_xbox_layout();
}

int apply(uint32_t xbox, uint32_t gcm_format, uint32_t width, uint32_t height, const uint8_t* guest, uint64_t guest_size,
          uint8_t* staging)
{
    if (gcm_format != 0x88 || width != 256 || height != 128 || guest_size < 2 * k_half) return 0;
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint64_t i = 0; i < 2 * k_half; i++) h = (h ^ guest[i]) * 0x100000001B3ull;
    if (h != k_sheet_hash) return 0;
    if (xbox) std::memcpy(staging + k_half, guest, k_half);
    return 1;
}

constexpr rsx_vk_texture_patch k_patch{ variant, apply };

}

void use_button_prompts()
{
    rsx_vk_set_texture_patch(&k_patch);
}
