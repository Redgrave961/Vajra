#include "assists.h"
#include "guest.h"
#include "pc_options.h"
#include "presence.h"
#include "rl_achievements.h"
#include "ue3_reflect.h"
#include "rsx_core.h"
#include "vk/rsx_vk.h"
#include "../ps3recomp/libs/input/pad_bindings.h"
#include "../ps3recomp/libs/system/cellSaveData.h"
#include "../ps3recomp/libs/system/cellSysutil.h"

#include <SDL3/SDL_events.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

extern "C" void ppu_fs_set_open_callback(void (*cb)(const char*));

namespace {

using ue3::Elements;
using ue3::Member;
using ue3::elements;
using ue3::k_object_class;
using ue3::k_struct_super;
using ue3::object_name_eq;

#include "option_menu_tags.inc"

constexpr uint32_t k_memory_file_vtable = 0x01751910;
constexpr uint32_t k_set_return_value = 0xCC;
constexpr uint32_t k_value_number = 3;
constexpr uint32_t k_value_string = 4;
constexpr uint32_t k_value_size = 0x10;
constexpr uint32_t k_proxy_blur[] = { 0x14, 0x18, 0x1C };
constexpr uint32_t k_proxy_bloom_scale = 0x38;
constexpr uint32_t k_proxy_motion_blur = 0x110;
constexpr uint32_t k_sys_settings = 0x01A56AB4;
constexpr uint32_t k_sys_shadow_filter_quality_bias = k_sys_settings + 0x384;
constexpr uint32_t k_sys_min_shadow_res = k_sys_settings + 0x388;
constexpr uint32_t k_sys_min_preshadow_res = k_sys_settings + 0x38C;
constexpr uint32_t k_sys_max_shadow_res = k_sys_settings + 0x390;
constexpr uint32_t k_sys_max_dom_shadow_res = k_sys_settings + 0x394;
constexpr uint32_t k_sys_shadow_texels_per_pixel = k_sys_settings + 0x398;
constexpr uint32_t k_sys_preshadow_res_factor = k_sys_settings + 0x39C;
constexpr uint32_t k_sys_shadow_filter_radius = k_sys_settings + 0x3B8;
constexpr uint32_t k_sys_shadow_depth_bias = k_sys_settings + 0x3BC;
constexpr uint32_t k_sys_shadow_fade_res = k_sys_settings + 0x3E4;
constexpr uint32_t k_sys_preshadow_fade_res = k_sys_settings + 0x3E8;
constexpr uint32_t k_rhi_max_shadow_x = 0x019C2F6C;
constexpr uint32_t k_rhi_max_shadow_y = 0x019C2F70;
constexpr uint32_t k_rhi_max_dom_shadow = 0x019C2F74;
constexpr uint32_t k_shadow_res = 4096;
constexpr uint32_t k_sys_tex_lod_base = 0x01A56B58;
constexpr uint32_t k_sys_tex_lod_count = 28;
constexpr uint32_t k_sys_tex_lod_stride = 24;
constexpr uint32_t k_tex_mips = 0xC4;
constexpr uint32_t k_bulk_flags = 0x04;
constexpr uint32_t k_bulk_elements = 0x08;
constexpr uint32_t k_bulk_offset = 0x0C;
constexpr uint32_t k_bulk_size_on_disk = 0x10;
constexpr uint32_t k_bulk_data = 0x24;
constexpr uint32_t k_bulk_separate_file = 0x01;
constexpr uint32_t k_bulk_unused = 0x20;
constexpr uint32_t k_use_vsync = 0x01A56E04;
constexpr uint32_t k_key_config_types = 3;
constexpr uint8_t k_dialog_idle = 3;
constexpr uint8_t k_dialog_passive = 3;
constexpr uint8_t k_start_button = 4;
constexpr uint8_t k_start_command = 5;
constexpr uint32_t k_start_judged = 1u << 8;
constexpr char k_push_button_event[] = "BCSeqEvent_PushButton";
constexpr uint8_t k_left_rotation_first = 36, k_left_rotation_last = 39;
constexpr char k_start_word[] = "START", k_enter_word[] = "ENTER";
constexpr uint32_t k_view_user_data = 0xD0;
constexpr uint32_t k_user_data_player = 0xD4;
constexpr uint32_t k_value_managed = 0x40;
constexpr uint32_t k_gfx_engine = 0x019CF64C;
constexpr uint32_t k_gfx_set_viewport = 0x68;
constexpr uint32_t k_gfx_get_viewport = 0x6C;
constexpr uint32_t k_gcm_ctx_ptr = 0x01B42038;
constexpr uint32_t k_bink_fullscreen_lr = 0x001547C0;
enum : uint32_t { hook_save_param = 1, hook_scene_manager, hook_stick_rotation, hook_judge_update, hook_judge_destroyed };
constexpr std::pair<const char*, uint32_t> k_hooks[] = {
    { "BCGame.BCGameEngine.GetSaveParam", hook_save_param },
    { "BCGame.BCHUD.GetSceneManager", hook_scene_manager },
    { "BCGame.BCGFxMovieScene.GetSceneManager", hook_scene_manager },
    { "BCGame.BCInputJudgeSystem_StickRotation.Update", hook_stick_rotation },
    { "BCGame.BCInputJudgeSystem.Update", hook_judge_update },
    { "BCGame.BCInputJudgeSystem.Destroyed", hook_judge_destroyed },
};
constexpr char k_prefix[] = "PCOpt_";
constexpr char k_bind_prefix[] = "bind_";
constexpr float k_mouse_units_per_step = 3.f;

struct Choice {
    std::string token;
    std::string label;
    uint32_t value;
    bool preferred = false;
};

using Choices = std::vector<Choice>;

const Choices k_display = {
    { "windowed", "Windowed", RSX_VK_WINDOWED },
    { "borderless", "Borderless", RSX_VK_BORDERLESS },
    { "fullscreen", "Fullscreen", RSX_VK_FULLSCREEN },
};
const Choices k_switch = {
    { "on", "", 1 },
    { "off", "", 0 },
};
const Choices k_damage_mult = {
    { "1x", "1x", 1 },
    { "2x", "2x", 2 },
    { "4x", "4x", 4 },
    { "8x", "8x", 8 },
    { "16x", "16x", 16 },
};
const Choices k_qte_timer = {
    { "off", "Off", 100 },
    { "1.5x", "1.5x", 150 },
    { "2x", "2x", 200 },
    { "3x", "3x", 300 },
};
const Choices k_language = {
    { "auto", "System Default", 0xFFFFFFFFu },
    { "en", "English", CELL_SYSUTIL_LANG_ENGLISH_US },
    { "ja", "Japanese", CELL_SYSUTIL_LANG_JAPANESE },
    { "fr", "French", CELL_SYSUTIL_LANG_FRENCH },
    { "es", "Spanish", CELL_SYSUTIL_LANG_SPANISH },
    { "de", "German", CELL_SYSUTIL_LANG_GERMAN },
    { "it", "Italian", CELL_SYSUTIL_LANG_ITALIAN },
};

constexpr uint32_t k_max_scale = 12;

std::vector<rsx_vk_mode> g_modes;
Choices g_res_choices;
Choices g_fps_choices;
Choices g_upscaler_choices;
int g_upscaler_caps_cached = -1;
bool g_modes_valid = false;
bool g_res_choices_valid = false;
bool g_fps_choices_valid = false;

const std::vector<rsx_vk_mode>& display_modes()
{
    if (!g_modes_valid) {
        for (;;) {
            const uint32_t n = rsx_vk_display_modes(nullptr, 0);
            g_modes.resize(n);
            if (rsx_vk_display_modes(g_modes.data(), n) == n) break;
        }
        g_modes_valid = !g_modes.empty();
        g_res_choices_valid = false;
        g_fps_choices_valid = false;
    }
    return g_modes;
}

constexpr uint32_t k_resolution_heights[] = { 720, 900, 1080, 1440, 1800, 2160, 2880, 4320 };
constexpr std::pair<uint32_t, uint32_t> k_ultrawide_presets[] = {
    { 2560, 1080 }, { 3440, 1440 }, { 3840, 1600 }, { 5120, 2160 },
    { 3840, 1200 },
    { 3840, 1080 }, { 5120, 1440 }, { 7680, 2160 },
    { 5760, 1200 },
    { 5760, 1080 }, { 7680, 1440 }, { 11520, 2160 },
    { 7680, 1080 }, { 10320, 1440 },
};

const char* aspect_tag(uint32_t w, uint32_t h)
{
    if (!h) return "";
    const double r = (double)w / (double)h;
    if (r <= 16.0 / 9.0 + 0.02) return "";
    if (std::fabs(r - 21.0 / 9.0) < 0.08) return " (21:9)";
    if (std::fabs(r - 32.0 / 10.0) < 0.08) return " (32:10)";
    if (std::fabs(r - 32.0 / 9.0) < 0.08) return " (32:9)";
    if (std::fabs(r - 48.0 / 10.0) < 0.08) return " (48:10)";
    if (std::fabs(r - 48.0 / 9.0) < 0.08) return " (48:9)";
    if (r >= 6.8 && r <= 7.3) return " (63:9)";
    return "";
}

constexpr uint32_t pack_resolution(uint32_t w, uint32_t h) { return (w << 16) | (h & 0xFFFFu); }
constexpr uint32_t res_width(uint32_t packed) { return packed >> 16; }
constexpr uint32_t res_height(uint32_t packed) { return packed & 0xFFFFu; }

bool valid_resolution(uint32_t w, uint32_t h)
{
    return w >= 1280u && h >= 720u && w <= 1280u * k_max_scale && h <= 720u * k_max_scale && (uint64_t)w * 9u >= (uint64_t)h * 16u;
}

bool same_aspect(uint32_t aw, uint32_t ah, uint32_t bw, uint32_t bh)
{
    return ah && bh && std::fabs((double)aw / (double)ah - (double)bw / (double)bh) < 0.02;
}

std::optional<uint32_t> parse_resolution(const std::string& token)
{
    uint32_t w = 0, h = 0;
    char tail = 0;
    if (std::sscanf(token.c_str(), "%ux%u%c", &w, &h, &tail) != 2 || !valid_resolution(w, h)) return std::nullopt;
    return pack_resolution(w, h);
}

std::optional<uint32_t> parse_frame_rate(const std::string& token)
{
    if (token == "unlimited") return 0u;
    const uint32_t hz = (uint32_t)std::strtoul(token.c_str(), nullptr, 10);
    return hz ? std::optional<uint32_t>(hz) : std::nullopt;
}

const Choices& display_choices() { return k_display; }
const Choices& switch_choices() { return k_switch; }
const Choices& damage_mult_choices() { return k_damage_mult; }
const Choices& qte_timer_choices() { return k_qte_timer; }
const Choices& language_choices() { return k_language; }

constexpr uint32_t k_sensitivity_steps = 20;

const Choices& sensitivity_choices()
{
    static const Choices list = [] {
        Choices out;
        out.reserve(k_sensitivity_steps);
        for (uint32_t n = 1; n <= k_sensitivity_steps; n++) out.push_back({ std::to_string(n), std::to_string(n), n });
        return out;
    }();
    return list;
}

const Choices& resolution_choices()
{
    const std::vector<rsx_vk_mode>& modes = display_modes();
    if (!g_res_choices_valid) {
        g_res_choices.clear();
        if (!modes.empty()) {
            rsx_vk_mode native{};
            if (!rsx_vk_desktop_mode(&native)) {
                native = *std::max_element(modes.begin(), modes.end(), [](const rsx_vk_mode& a, const rsx_vk_mode& b) {
                    return (uint64_t)a.width * a.height < (uint64_t)b.width * b.height;
                });
            }
            std::vector<uint32_t> list;
            const auto add = [&](uint32_t w, uint32_t h) {
                if (valid_resolution(w, h)) list.push_back(pack_resolution(w, h));
            };
            for (const uint32_t h : k_resolution_heights) add(h * 16u / 9u, h);
            const auto add_matching_presets = [&](uint32_t aw, uint32_t ah) {
                for (const auto& [pw, ph] : k_ultrawide_presets)
                    if (same_aspect(pw, ph, aw, ah)) add(pw, ph);
            };
            for (const rsx_vk_mode& m : modes) {
                if ((uint64_t)m.width * 9u > (uint64_t)m.height * 16u) {
                    add(m.width, m.height);
                    add_matching_presets(m.width, m.height);
                }
            }
            const bool native_uw = (uint64_t)native.width * 9u > (uint64_t)native.height * 16u;
            add(native.width, native.height);
            if (native_uw) {
                add_matching_presets(native.width, native.height);
                const auto even = [](double v) { return ((uint32_t)std::lround(v) + 1u) & ~1u; };
                constexpr std::pair<uint32_t, uint32_t> k_scales[] = { { 3, 4 }, { 5, 4 }, { 3, 2 }, { 2, 1 } };
                for (const auto& [num, den] : k_scales)
                    add(even((double)native.width * num / den), even((double)native.height * num / den));
            }
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
            const auto bucket = [&](uint32_t p) -> int {
                const uint32_t w = res_width(p), h = res_height(p);
                if (same_aspect(w, h, native.width, native.height)) return 0;
                if ((uint64_t)w * 9u == (uint64_t)h * 16u) return 1;
                return 2;
            };
            std::sort(list.begin(), list.end(), [&](uint32_t a, uint32_t b) {
                const int ba = bucket(a), bb = bucket(b);
                if (ba != bb) return ba < bb;
                const uint32_t aw = res_width(a), ah = res_height(a), bw = res_width(b), bh = res_height(b);
                if (ba == 2) {
                    const double ra = (double)aw / ah, rb = (double)bw / bh;
                    if (std::fabs(ra - rb) >= 0.02) return ra < rb;
                }
                return ah != bh ? ah < bh : aw < bw;
            });
            const uint32_t exact_native = pack_resolution(native.width, native.height);
            const auto distance = [&](uint32_t p) -> uint64_t {
                if (p == exact_native) return 0u;
                const uint32_t h = res_height(p);
                const uint64_t dh = h > native.height ? h - native.height : native.height - h;
                return (uint64_t)bucket(p) * 100000ull + dh + 1u;
            };
            const uint32_t preferred = *std::min_element(list.begin(), list.end(), [&](uint32_t a, uint32_t b) { return distance(a) < distance(b); });
            g_res_choices.reserve(list.size());
            for (const uint32_t p : list) {
                const uint32_t rw = res_width(p), rh = res_height(p);
                const std::string w = std::to_string(rw), h = std::to_string(rh);
                g_res_choices.push_back({ w + "x" + h, w + " x " + h + aspect_tag(rw, rh), p, p == preferred });
            }
        }
        g_res_choices_valid = g_modes_valid;
    }
    return g_res_choices;
}

const Choices& frame_rate_choices()
{
    const std::vector<rsx_vk_mode>& modes = display_modes();
    if (!g_fps_choices_valid) {
        g_fps_choices.clear();
        if (!modes.empty()) {
            std::vector<uint32_t> rates;
            for (const rsx_vk_mode& m : modes)
                if (const uint32_t hz = (uint32_t)std::lround(m.refresh); hz && hz != 30) rates.push_back(hz);
            std::sort(rates.begin(), rates.end());
            rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
            g_fps_choices.reserve(rates.size() + 2);
            g_fps_choices.push_back({ "30", "30", 30 });
            for (const uint32_t hz : rates) g_fps_choices.push_back({ std::to_string(hz), std::to_string(hz), hz });
            g_fps_choices.push_back({ "unlimited", "Unlimited", 0 });
        }
        g_fps_choices_valid = g_modes_valid;
    }
    return g_fps_choices;
}

constexpr uint32_t k_aa_smaa = 0x100u;

const Choices& aa_choices()
{
    const int caps = rsx_vk_upscaler_caps();
    if (caps != g_upscaler_caps_cached) {
        g_upscaler_choices.clear();
        g_upscaler_choices.push_back({ "off", "Off", 0u });
        g_upscaler_choices.push_back({ "on", "SMAA", k_aa_smaa });
        if (caps & RSX_VK_UPSCALE_CAP_FSR) {
            const char* ver = rsx_vk_upscaler_fsr_version();
            const std::string fsr = (ver && *ver) ? ("FSR " + std::string(ver)) : std::string("FSR 3.1");
            g_upscaler_choices.push_back({ "fsr_native", fsr + " Native AA", RSX_VK_UPSCALE_FSR_NATIVE });
            g_upscaler_choices.push_back({ "fsr_quality", fsr + " Quality", RSX_VK_UPSCALE_FSR_QUALITY });
            g_upscaler_choices.push_back({ "fsr_balanced", fsr + " Balanced", RSX_VK_UPSCALE_FSR_BALANCED });
            g_upscaler_choices.push_back({ "fsr_performance", fsr + " Performance", RSX_VK_UPSCALE_FSR_PERFORMANCE });
        }
        if (caps & RSX_VK_UPSCALE_CAP_DLSS) {
            g_upscaler_choices.push_back({ "dlaa", "NVIDIA DLAA", RSX_VK_UPSCALE_DLAA });
            g_upscaler_choices.push_back({ "dlss_quality", "DLSS Quality", RSX_VK_UPSCALE_DLSS_QUALITY });
            g_upscaler_choices.push_back({ "dlss_balanced", "DLSS Balanced", RSX_VK_UPSCALE_DLSS_BALANCED });
            g_upscaler_choices.push_back({ "dlss_performance", "DLSS Performance", RSX_VK_UPSCALE_DLSS_PERFORMANCE });
        }
        g_upscaler_caps_cached = caps;
    }
    return g_upscaler_choices;
}

std::optional<uint32_t> parse_aa(const std::string& token)
{
    if (token == "off") return 0u;
    if (token == "on" || token == "smaa") return k_aa_smaa;
    if (token == "fsr_native") return RSX_VK_UPSCALE_FSR_NATIVE;
    if (token == "fsr_quality") return RSX_VK_UPSCALE_FSR_QUALITY;
    if (token == "fsr_balanced") return RSX_VK_UPSCALE_FSR_BALANCED;
    if (token == "fsr_performance") return RSX_VK_UPSCALE_FSR_PERFORMANCE;
    if (token == "dlaa") return RSX_VK_UPSCALE_DLAA;
    if (token == "dlss_quality") return RSX_VK_UPSCALE_DLSS_QUALITY;
    if (token == "dlss_balanced") return RSX_VK_UPSCALE_DLSS_BALANCED;
    if (token == "dlss_performance") return RSX_VK_UPSCALE_DLSS_PERFORMANCE;
    return std::nullopt;
}

const Choices& fsr_sharpness_choices()
{
    static const Choices list = [] {
        Choices c;
        c.reserve(101);
        for (uint32_t v = 0; v <= 100u; ++v)
            c.push_back({ std::to_string(v), v ? std::to_string(v) + "%" : "Off", v });
        return c;
    }();
    return list;
}

std::optional<uint32_t> parse_fsr_sharpness(const std::string& token)
{
    if (token.empty()) return std::nullopt;
    char* end = nullptr;
    const unsigned long v = std::strtoul(token.c_str(), &end, 10);
    if (!end || *end != '\0' || v > 100ul) return std::nullopt;
    return (uint32_t)v;
}

bool g_settings_parsed;
std::atomic<bool> g_motion_blur{ true };
std::atomic<bool> g_depth_of_field{ true };
std::atomic<bool> g_bloom{ true };
std::atomic<float> g_mouse_units{ 30.f };
std::atomic<bool> g_invert_mouse{ false };
std::atomic<float> g_aspect_scale{ 1.f };
std::atomic<bool> g_gfx_aspect_dirty{ false };
std::atomic<uint32_t> g_upscaler_mode{ RSX_VK_UPSCALE_OFF };
std::atomic<uint32_t> g_fsr_sharpness{ 0u };
std::atomic<uint32_t> g_eff_w{ 1280u };
std::atomic<uint32_t> g_eff_h{ 720u };
std::mutex g_res_lock;
uint32_t g_base_res = pack_resolution(1280u, 720u);
uint32_t g_win_w = 0, g_win_h = 0;
uint32_t g_cur_display = RSX_VK_BORDERLESS;

void apply_effective_resolution_locked()
{
    uint32_t w = res_width(g_base_res), h = res_height(g_base_res);
    if (g_cur_display == RSX_VK_WINDOWED && g_win_w && g_win_h) {
        const double win_ar = (double)g_win_w / (double)g_win_h;
        const double eff_ar = win_ar > 16.0 / 9.0 + 0.01 ? win_ar : 16.0 / 9.0;
        w = std::clamp(((uint32_t)std::lround((double)h * eff_ar) + 1u) & ~1u, 1280u, 1280u * k_max_scale);
    }
    g_eff_w.store(w, std::memory_order_relaxed);
    g_eff_h.store(h, std::memory_order_relaxed);
    rsx_vk_set_resolution_scale(w, 1280u, h, 720u);
    const float aspect = ((float)w * 9.f) / ((float)h * 16.f);
    const float clamped = aspect > 1.0001f ? aspect : 1.f;
    if (g_aspect_scale.exchange(clamped, std::memory_order_relaxed) != clamped)
        g_gfx_aspect_dirty.store(true, std::memory_order_release);
}

void window_resized(uint32_t w, uint32_t h)
{
    std::lock_guard<std::mutex> hold(g_res_lock);
    g_win_w = w;
    g_win_h = h;
    if (g_cur_display == RSX_VK_WINDOWED) apply_effective_resolution_locked();
}

void apply_display(uint32_t value)
{
    uint32_t rw = 0, rh = 0;
    {
        std::lock_guard<std::mutex> hold(g_res_lock);
        g_cur_display = value;
        if (value == RSX_VK_WINDOWED) {
            rw = res_width(g_base_res);
            rh = res_height(g_base_res);
            g_win_w = rw;
            g_win_h = rh;
        }
        apply_effective_resolution_locked();
    }
    rsx_vk_set_display((rsx_vk_display)value);
    if (rw && rh) rsx_vk_resize_window(rw, rh);
}
void apply_resolution(uint32_t value)
{
    uint32_t rw = 0, rh = 0;
    {
        std::lock_guard<std::mutex> hold(g_res_lock);
        g_base_res = value;
        if (g_cur_display == RSX_VK_WINDOWED) {
            rw = res_width(value);
            rh = res_height(value);
            g_win_w = rw;
            g_win_h = rh;
        }
        apply_effective_resolution_locked();
    }
    if (rw && rh) rsx_vk_resize_window(rw, rh);
}
void apply_aa(uint32_t value)
{
    if (value == k_aa_smaa) {
        g_upscaler_mode.store(RSX_VK_UPSCALE_OFF, std::memory_order_relaxed);
        rsx_vk_set_upscaler(RSX_VK_UPSCALE_OFF);
        rsx_vk_set_aa(RSX_VK_AA_SMAA);
    } else {
        g_upscaler_mode.store(value, std::memory_order_relaxed);
        rsx_vk_set_aa(RSX_VK_AA_OFF);
        rsx_vk_set_upscaler((int)value);
    }
}
void apply_fsr_sharpness(uint32_t value) { g_fsr_sharpness.store(std::min(value, 100u), std::memory_order_relaxed); }
void apply_frame_rate(uint32_t value)
{
    const bool console = value == 30;
    rsx_vk_set_frame_rate(!console, console ? 0 : value);
    if (g_settings_parsed) vm_write32(k_use_vsync, console);
}
void apply_motion_blur(uint32_t value) { g_motion_blur.store(value != 0, std::memory_order_relaxed); }
void apply_depth_of_field(uint32_t value) { g_depth_of_field.store(value != 0, std::memory_order_relaxed); }
void apply_bloom(uint32_t value) { g_bloom.store(value != 0, std::memory_order_relaxed); }
void apply_language(uint32_t value) { ps3_sysutil_set_language((int32_t)value); }
void apply_sensitivity(uint32_t value) { g_mouse_units.store(value * k_mouse_units_per_step, std::memory_order_relaxed); }
void apply_invert_mouse(uint32_t value) { g_invert_mouse.store(value != 0, std::memory_order_relaxed); }
void apply_god_mode(uint32_t value) { assists_set_flag(ASSIST_GOD_MODE, value != 0); }
void apply_inf_burst(uint32_t value) { assists_set_flag(ASSIST_INF_BURST, value != 0); }
void apply_inf_unlimited(uint32_t value) { assists_set_flag(ASSIST_INF_UNLIMITED, value != 0); }
void apply_one_hit_kill(uint32_t value) { assists_set_flag(ASSIST_ONE_HIT_KILL, value != 0); }
void apply_damage_mult(uint32_t value) { assists_set_damage_mult(value); }
void apply_auto_qte(uint32_t value) { assists_set_flag(ASSIST_AUTO_QTE, value != 0); }
void apply_hold_mash(uint32_t value) { assists_set_flag(ASSIST_HOLD_MASH, value != 0); }
void apply_qte_timer(uint32_t value) { assists_set_qte_timer_pct(value); }
void apply_unlock_episodes(uint32_t value) { assists_set_flag(ASSIST_UNLOCK_EPISODES, value != 0); }
void apply_unlock_gauges(uint32_t value) { assists_set_flag(ASSIST_UNLOCK_GAUGES, value != 0); }
void apply_unlock_extras(uint32_t value) { assists_set_flag(ASSIST_UNLOCK_EXTRAS, value != 0); }

struct Option {
    const char* key;
    const char* title;
    const char* help;
    const Choices& (*choices)();
    const char* initial;
    std::optional<uint32_t> (*parse)(const std::string& token);
    void (*apply)(uint32_t value);
};

constexpr Option k_options[] = {
    { "display", "Display Mode", "Windowed, borderless window, or exclusive fullscreen at the display's native mode.", display_choices, "borderless", nullptr, apply_display },
    { "resolution", "Resolution", "Internal rendering resolution.", resolution_choices, nullptr, parse_resolution, apply_resolution },
    { "frame_rate", "Frame Rate", "Frame rate limit. 30 uses the console's original pacing.", frame_rate_choices, "unlimited", parse_frame_rate, apply_frame_rate },
    { "anti_aliasing", "Anti-Aliasing / Upscaling", "SMAA, AMD FSR, or NVIDIA DLSS / DLAA anti-aliasing and upscaling.", aa_choices, "on", parse_aa, apply_aa },
    { "rcas_sharpness", "RCAS Sharpness", "Robust Contrast-Adaptive Sharpening strength (0% disables RCAS).", fsr_sharpness_choices, "0", parse_fsr_sharpness, apply_fsr_sharpness },
    { "motion_blur", "Motion Blur", "Blur from camera and object motion.", switch_choices, "on", nullptr, apply_motion_blur },
    { "depth_of_field", "Depth of Field", "Blur outside the camera's focus, mainly in cutscenes.", switch_choices, "on", nullptr, apply_depth_of_field },
    { "bloom", "Bloom", "Glow around bright lights.", switch_choices, "on", nullptr, apply_bloom },
    { "language", "Language", "Text and menu language. Takes effect after restarting the game.", language_choices, "auto", nullptr, apply_language },
    { "god_mode", "God Mode", "Invincibility and full health.", switch_choices, "off", nullptr, apply_god_mode },
    { "inf_burst", "Infinite Burst", "Any Burst gauge gain fills the gauge completely. Spending it works normally, so scripted Burst scenes play as intended.", switch_choices, "off", nullptr, apply_inf_burst },
    { "inf_unlimited", "Infinite Unlimited", "Any Unlimited gauge gain fills the gauge completely, and Unlimited mode is not force-ended.", switch_choices, "off", nullptr, apply_inf_unlimited },
    { "one_hit_kill", "One-Hit Kills", "Defeat enemies in a single hit.", switch_choices, "off", nullptr, apply_one_hit_kill },
    { "damage_mult", "Damage Multiplier", "Multiply damage dealt to enemies.", damage_mult_choices, "1x", nullptr, apply_damage_mult },
    { "auto_qte", "Automatic Perfect QTEs", "Instantly complete button, stick, and mash QTEs, and hit timed Synchronic Impacts with Excellent timing.", switch_choices, "off", nullptr, apply_auto_qte },
    { "hold_mash", "Hold Instead of Mash", "Hold the prompted button instead of rapidly mashing.", switch_choices, "off", nullptr, apply_hold_mash },
    { "ext_qte_timer", "Extended QTE Timers", "Slow QTE countdowns and widen Synchronic timing windows.", qte_timer_choices, "off", nullptr, apply_qte_timer },
    { "unlock_episodes", "Unlock All Episodes", "Access all episodes without modifying your save file.", switch_choices, "off", nullptr, apply_unlock_episodes },
    { "unlock_gauges", "Unlock All Gauges", "Access all gauges without modifying your save file.", switch_choices, "off", nullptr, apply_unlock_gauges },
    { "unlock_extras", "Unlock All Extras", "Access all Art, CG, Movies, and Interludes without modifying your save.", switch_choices, "off", nullptr, apply_unlock_extras },
    { "mouse_sensitivity", "Sensitivity", "Mouse: How fast the camera turns with the mouse.", sensitivity_choices, "10", nullptr, apply_sensitivity },
    { "invert_mouse_y", "Invert Y", "Mouse: Moving the mouse forward looks down.", switch_choices, "off", nullptr, apply_invert_mouse },
};

constexpr uint32_t option_row(std::string_view key)
{
    for (uint32_t i = 0; i < (uint32_t)std::size(k_options); ++i)
        if (key == k_options[i].key) return i;
    return (uint32_t)std::size(k_options);
}

constexpr uint32_t row_display = option_row("display");
constexpr uint32_t row_frame_rate = option_row("frame_rate");
constexpr uint32_t row_language = option_row("language");
constexpr uint32_t row_cheats = option_row("god_mode");
constexpr uint32_t row_mouse = option_row("mouse_sensitivity");
static_assert(row_display < std::size(k_options) && row_frame_rate < std::size(k_options) &&
              row_language < std::size(k_options) && row_cheats < std::size(k_options) &&
              row_mouse < std::size(k_options));

const char* const k_action_keys[PAD_ACTION_COUNT] = {
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3", "start", "select",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "move_forward", "move_back", "move_left", "move_right",
    "look_up", "look_down", "look_left", "look_right", "menu_confirm", "menu_back", "menu_prev", "menu_next", "menu_extra",
};

struct Binding {
    int action;
    const char* title;
    const char* help;
};

const Binding k_bindings[] = {
    { PAD_SQUARE, "Attack", "Combat: Asura's basic melee attack." },
    { PAD_TRIANGLE, "Heavy Attack / Counter", "Combat: Strong hit that overheats for a while; also catches missiles." },
    { PAD_CIRCLE, "Shoot", "Combat: Fire at range; hold for rapid fire." },
    { PAD_L1, "Lock On", "Combat: Lock on to an enemy." },
    { PAD_LOOK_RIGHT, "Switch Target Right", "Combat: Switch to the next target while locked on." },
    { PAD_LOOK_LEFT, "Switch Target Left", "Combat: Switch to the previous target while locked on." },
    { PAD_R1, "Evade", "Combat: Dash out of the way of enemy attacks." },
    { PAD_CROSS, "Jump", "Combat: Jump, air dash with a direction, and recover after a hit." },
    { PAD_R2, "Burst", "Special: Unleash Burst when the Burst Gauge is full." },
    { PAD_L2, "Unlimited Mode", "Special: Activate Unlimited Mode when its gauge is full." },
    { PAD_R3, "Reset Camera", "Special: Reset the free camera." },
    { PAD_MOVE_UP, "Forward", "Movement: Move forward, or aim the reticle up." },
    { PAD_MOVE_DOWN, "Back", "Movement: Move back, or aim the reticle down." },
    { PAD_MOVE_LEFT, "Left", "Movement: Move left, or aim the reticle left." },
    { PAD_MOVE_RIGHT, "Right", "Movement: Move right, or aim the reticle right." },
    { PAD_START, "Pause", "Menus & System: Open the pause menu." },
    { PAD_SELECT, "Skip Movie", "Menus & System: Skip the current movie." },
    { PAD_MENU_CONFIRM, "Confirm", "Menus & System: Confirm in menus. Jump confirms too." },
    { PAD_MENU_BACK, "Back", "Menus & System: Go back in menus. Shoot goes back too." },
    { PAD_MENU_PREV, "Previous Tab", "Menus & System: Previous page or tab in menus." },
    { PAD_MENU_NEXT, "Next Tab", "Menus & System: Next page or tab in menus." },
    { PAD_MENU_EXTRA, "Zoom / Select All", "Menus & System: Zoom artwork or toggle all items in Collection." },
    { PAD_UP, "Menu Up", "Menus & System: Move the menu cursor up." },
    { PAD_DOWN, "Menu Down", "Menus & System: Move the menu cursor down." },
    { PAD_LEFT, "Menu Left", "Menus & System: Move the menu cursor left." },
    { PAD_RIGHT, "Menu Right", "Menus & System: Move the menu cursor right." },
};

constexpr uint32_t k_rows = (uint32_t)std::size(k_options);
constexpr uint32_t k_reset_row = k_rows + (uint32_t)std::size(k_bindings);
constexpr uint32_t k_all_rows = k_reset_row + 1;
constexpr char k_bind_hint[] = "  Left/Right picks a key, Confirm rebinds";
constexpr char k_capture_hint[] = "Esc to cancel, Delete to clear";
constexpr char k_separator[] = "  /  ";

bool binding_row(uint32_t row)
{
    return row >= k_rows && row < k_reset_row;
}

struct Page {
    const char* title;
    const char* help;
    uint32_t first;
    uint32_t end;
};

const Page k_pages[] = {
    { "Graphics", "Display, resolution, frame rate and image effects.", 0, row_language },
    { "Keyboard & Mouse", "Key and mouse button bindings, mouse sensitivity and inversion.", row_mouse, k_all_rows },
    { "Cheats", "Gameplay assists, QTE accessibility, and non-destructive unlocks.", row_cheats, row_mouse },
    { "Language", "Text and menu language. Takes effect the next time the game starts.", row_language, row_cheats },
};

std::atomic<int> g_capture{ -1 };
uint32_t g_slot[std::size(k_bindings)];
int g_swap_row = -1;
std::string g_swap_text;

std::mutex g_hooks_lock;
std::atomic<uint64_t> g_save_param{ 0 };
std::atomic<uint32_t> g_stick_lit{ 0 };
std::atomic<uint64_t> g_spin_ms{ 0 };
std::atomic<uint8_t> g_spin_cmd{ 0 };
std::atomic<uint64_t> g_scene_manager{ 0 };
std::atomic<uint64_t> g_prompt{ 0 };
std::atomic<uint64_t> g_hud{ 0 };
std::atomic<bool> g_controller_screen{ false };
std::string g_saved[k_rows];
std::optional<uint32_t> g_applied[k_rows];
std::mutex g_settings_lock;
std::filesystem::path g_settings_path;

void load()
{
    std::ifstream f(g_settings_path);
    if (!f) return;
    bool legacy_perfect_qte = false;
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        if (key == "perfect_qte" && val == "on") legacy_perfect_qte = true;
        for (uint32_t r = 0; r < k_rows; r++)
            if (key == k_options[r].key) g_saved[r] = val;
        if (key.rfind(k_bind_prefix, 0) != 0) continue;
        const std::string act = key.substr(sizeof k_bind_prefix - 1);
        for (int a = 0; a < PAD_ACTION_COUNT; a++) {
            if (act != k_action_keys[a]) continue;
            const char* at = val.c_str();
            for (int s = 0; s < PAD_BINDING_SLOTS; s++, at += *at == ',') {
                char* end = nullptr;
                pad_binding_set(a, s, (int)std::strtol(at, &end, 10));
                at = end;
            }
        }
    }
    if (legacy_perfect_qte) {
        for (uint32_t r = 0; r < k_rows; r++)
            if (!std::strcmp(k_options[r].key, "auto_qte") && g_saved[r].empty()) g_saved[r] = "on";
    }
}

std::optional<uint32_t> selected(uint32_t row, const Choices& list)
{
    const char* initial = k_options[row].initial;
    std::optional<uint32_t> fallback;
    for (uint32_t c = 0; c < list.size(); c++) {
        if (list[c].token == g_saved[row]) return c;
        if (initial ? list[c].token == initial : list[c].preferred) fallback = c;
    }
    return fallback;
}

std::optional<uint32_t> value(uint32_t row)
{
    const Choices& list = k_options[row].choices();
    if (const auto c = selected(row, list)) return list[*c].value;
    if (k_options[row].parse) return k_options[row].parse(g_saved[row]);
    return std::nullopt;
}

void sync(uint32_t row)
{
    const auto v = value(row);
    if (!v || g_applied[row] == v) return;
    g_applied[row] = v;
    k_options[row].apply(*v);
}

void save()
{
    std::ofstream f(g_settings_path, std::ios::binary | std::ios::trunc);
    if (!f) return;
    for (uint32_t r = 0; r < k_rows; r++) {
        std::string token = g_saved[r];
        if (token.empty()) {
            const Choices& list = k_options[r].choices();
            if (const auto c = selected(r, list)) token = list[*c].token;
        }
        if (!token.empty()) f << k_options[r].key << '=' << token << '\n';
    }
    for (int a = 0; a < PAD_ACTION_COUNT; a++) {
        f << k_bind_prefix << k_action_keys[a] << '=';
        for (int s = 0; s < PAD_BINDING_SLOTS; s++) {
            if (s) f << ',';
            f << pad_binding(a, s);
        }
        f << '\n';
    }
}

void set(uint32_t row, uint32_t index)
{
    if (binding_row(row) && index < PAD_BINDING_SLOTS && g_slot[row - k_rows] != index) {
        g_slot[row - k_rows] = index;
        g_swap_row = -1;
    }
    std::lock_guard<std::mutex> hold(g_settings_lock);
    if (row >= k_rows) return;
    const Choices& list = k_options[row].choices();
    if (index >= list.size() || selected(row, list) == index) return;
    g_saved[row] = list[index].token;
    sync(row);
    save();
}

void reset(const Page& page)
{
    std::lock_guard<std::mutex> hold(g_settings_lock);
    for (uint32_t r = page.first; r < page.end && r < k_rows; r++) {
        g_saved[r] = k_options[r].initial ? k_options[r].initial : "";
        sync(r);
    }
    if (page.end > k_rows) pad_bindings_reset();
    save();
}

bool capturing(uint32_t row)
{
    return pad_capturing() && g_capture.load(std::memory_order_relaxed) == (int)row;
}

uint32_t decide(uint32_t row)
{
    g_swap_row = -1;
    if (row == k_reset_row) {
        reset(k_pages[1]);
        return 2;
    }
    if (!binding_row(row)) return 0;
    g_capture.store((int)row, std::memory_order_relaxed);
    pad_capture(k_bindings[row - k_rows].action, (int)g_slot[row - k_rows]);
    return 1;
}

std::string action_title(int action)
{
    for (const Binding& b : k_bindings)
        if (b.action == action) return b.title;
    return k_action_keys[action];
}

uint32_t capture(bool cancel)
{
    if (cancel) pad_capture(-1, 0);
    if (pad_capturing()) return 1;
    const int row = g_capture.exchange(-1, std::memory_order_relaxed);
    if (row < 0) return 0;
    int input;
    if (const int other = pad_last_swap(&input); other >= 0) {
        g_swap_row = row;
        g_swap_text = "Swapped: " + action_title(other) + " is now " + (input ? pad_input_name(input) : "unbound");
    }
    std::lock_guard<std::mutex> hold(g_settings_lock);
    save();
    return 0;
}

std::string binding_label(uint32_t row, uint32_t slot)
{
    if (capturing(row)) return "Press a key or mouse button...";
    std::string text;
    for (uint32_t s = 0; s < PAD_BINDING_SLOTS; s++) {
        const int input = pad_binding(k_bindings[row - k_rows].action, (int)s);
        const std::string name = input ? pad_input_name(input) : "None";
        text += (s ? k_separator : "") + (s == slot ? "[" + name + "]" : name);
    }
    return text;
}

const char* title(uint32_t row)
{
    return row < k_rows ? k_options[row].title : binding_row(row) ? k_bindings[row - k_rows].title : "Restore Default Controls";
}

std::string help(uint32_t row)
{
    if (row < k_rows) return k_options[row].help;
    if (!binding_row(row)) return "Restore the default keyboard and mouse controls.";
    if (capturing(row)) return k_capture_hint;
    if ((int)row == g_swap_row) return g_swap_text;
    return std::string(k_bindings[row - k_rows].help) + k_bind_hint;
}

void display_toggled(rsx_vk_display display)
{
    {
        std::lock_guard<std::mutex> hold(g_res_lock);
        g_cur_display = (uint32_t)display;
        apply_effective_resolution_locked();
    }
    std::lock_guard<std::mutex> hold(g_settings_lock);
    for (const Choice& c : k_display)
        if (c.value == (uint32_t)display) g_saved[row_display] = c.token;
    g_applied[row_display] = (uint32_t)display;
    save();
}

void display_modes_changed()
{
    std::lock_guard<std::mutex> hold(g_settings_lock);
    g_modes_valid = false;
    g_res_choices_valid = false;
    g_fps_choices_valid = false;
    for (uint32_t r = 0; r < k_rows; r++) sync(r);
}

uint32_t count(uint32_t row)
{
    if (row >= k_rows) return binding_row(row) ? PAD_BINDING_SLOTS : 1;
    std::lock_guard<std::mutex> hold(g_settings_lock);
    const Choices& list = k_options[row].choices();
    return list.empty() || list[0].label.empty() ? 0 : (uint32_t)list.size();
}

std::string label(uint32_t row, uint32_t index)
{
    if (row == k_reset_row) return "Reset";
    if (row >= k_rows) return binding_label(row, index);
    std::lock_guard<std::mutex> hold(g_settings_lock);
    const Choices& list = k_options[row].choices();
    return index < list.size() ? list[index].label : std::string();
}

uint32_t current(uint32_t row)
{
    if (row >= k_rows) return binding_row(row) ? g_slot[row - k_rows] : 0;
    std::lock_guard<std::mutex> hold(g_settings_lock);
    return selected(row, k_options[row].choices()).value_or(0);
}

double argument(const ppu_context* ctx, uint32_t i)
{
    if (i >= (uint32_t)ctx->gpr[7]) return 0;
    const uint32_t at = (uint32_t)ctx->gpr[6] + i * k_value_size;
    if ((vm_read32(at + 4) & 0xF) != k_value_number) return 0;
    return from_bits64(vm_read64(at + 8));
}

void reply(ppu_context* ctx, uint64_t number, const char* text)
{
    const uint32_t view = (uint32_t)ctx->gpr[4];
    const uint32_t length = text ? (uint32_t)std::strlen(text) + 1u : 0u;
    const GuestFrame frame(ctx, 0x80u + ((length + 15u) & ~15u));
    const uint32_t result = frame.base + 0x70, chars = result + k_value_size;
    if (text) ppu_store_block(chars, text, length);
    vm_write32(result, 0);
    vm_write32(result + 4, text ? k_value_string : k_value_number);
    vm_write64(result + 8, text ? (uint64_t)chars << 32 : number);
    ctx->gpr[3] = view;
    ctx->gpr[4] = result;
    guest_call(ctx, vm_read32(vm_read32(view) + k_set_return_value));
}

void reply_number(ppu_context* ctx, double v)
{
    reply(ctx, to_bits64(v), nullptr);
}

Member m_current_scene{ "CurrentScene" }, m_pause_menu{ "PauseMenu" }, m_common{ "Common" }, m_movie_open{ "bMovieIsOpen" },
    m_dialog_state{ "DialogState" }, m_dialog_type{ "dialogType" }, m_system_param{ "SystemParam" }, m_key_config{ "KeyConfigType" },
    m_judging{ "bJudge" }, m_command{ "CommandType" }, m_title_menu{ "TitleMenu" }, m_enable_input{ "bEnableInput" },
    m_judge_info{ "JudgeInfo" }, m_request{ "Request" }, m_buttons{ "Buttons" }, m_input_info{ "InputInfo" }, m_enable_judge{ "bEnableJudge" },
    m_paused{ "bPause" }, m_shown{ "bShow" }, m_succeeded{ "bSuccess" },
    m_player_owner{ "PlayerOwner" }, m_player_pawn{ "BCPlayerPawn" }, m_generated_events{ "GeneratedEvents" }, m_enabled{ "bEnabled" },
    m_trigger_count{ "TriggerCount" }, m_max_trigger_count{ "MaxTriggerCount" }, m_input_buttons{ "InputButtons" };
std::atomic<uint32_t> g_push_button_class{ 0 };
std::atomic<uint32_t> g_texture2d_class{ 0 };

bool is_texture2d(uint32_t object)
{
    const uint32_t cls = object ? vm_read32(object + k_object_class) : 0;
    if (!cls) return false;
    const uint32_t cached = g_texture2d_class.load(std::memory_order_relaxed);
    if (cached && cls == cached) return true;
    for (uint32_t c = cls; c; c = vm_read32(c + k_struct_super)) {
        if (cached ? (c == cached) : object_name_eq(c, "Texture2D")) {
            if (!cached) g_texture2d_class.store(c, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

bool mip_has_data(uint32_t mip)
{
    if (!mip) return false;
    const uint32_t flags = vm_read32(mip + k_bulk_flags);
    if (flags & k_bulk_unused) return false;
    if ((int32_t)vm_read32(mip + k_bulk_elements) <= 0) return false;
    const int32_t size_on_disk = (int32_t)vm_read32(mip + k_bulk_size_on_disk);
    if (flags & k_bulk_separate_file)
        return size_on_disk > 0 && vm_read32(mip + k_bulk_offset) != 0xFFFFFFFFu;
    return vm_read32(mip + k_bulk_data) != 0 || size_on_disk > 0;
}

bool push_button_event(uint32_t event)
{
    const uint32_t cls = vm_read32(event + k_object_class);
    if (cls == g_push_button_class.load(std::memory_order_relaxed)) return true;
    if (!object_name_eq(cls, k_push_button_event)) return false;
    g_push_button_class.store(cls, std::memory_order_relaxed);
    return true;
}

bool waits_for_start(uint32_t hud)
{
    const uint32_t player = hud ? m_player_owner.obj(hud) : 0;
    const uint32_t pawn = player ? m_player_pawn.obj(player) : 0;
    const uint32_t events = pawn ? m_generated_events.at(pawn, vm_read32(pawn + k_object_class)) : 0;
    for (uint32_t i = 0, n = events ? vm_read32(events + 4) : 0; i < n; i++) {
        const uint32_t event = vm_read32(vm_read32(events) + i * 4);
        if (!event || !push_button_event(event) || !m_enabled.flag(event)) continue;
        const uint32_t limit = m_max_trigger_count.obj(event);
        if (limit && m_trigger_count.obj(event) >= limit) continue;
        const uint32_t buttons = m_input_buttons.at(event, vm_read32(event + k_object_class));
        for (uint32_t b = 0, count = buttons ? vm_read32(buttons + 4) : 0; b < count; b++)
            if (vm_read8(vm_read32(buttons) + b) == k_start_button) return true;
    }
    return false;
}

uint32_t stick_bits(uint8_t command)
{
    constexpr uint8_t k_directions[4] = { 1, 4, 2, 8 };
    return command >= 17 && command <= 24 ? (uint32_t)k_directions[(command - 17) & 3] << ((command - 17) & 4) : command == 42 ? 4 | 8 : 0;
}

uint32_t stick_lit(uint32_t system)
{
    uint32_t lit = 0;
    const Elements judges = elements(m_judge_info, system, vm_read32(system + k_object_class));
    for (uint32_t j = 0; j < judges.count; j++) {
        const uint32_t judge = judges.data + j * judges.stride;
        uint32_t req_type = 0;
        const uint32_t request = m_request.at(judge, judges.type, &req_type);
        if (!request || !m_enable_judge.sflag(judge, judges.type) || m_paused.sflag(judge, judges.type)) continue;
        const Elements buttons = elements(m_buttons, request, req_type);
        const Elements inputs = elements(m_input_info, judge, judges.type);
        for (uint32_t b = 0; b < buttons.count && b < inputs.count; b++) {
            const uint32_t input = inputs.data + b * inputs.stride;
            if (m_succeeded.sflag(input, inputs.type)) continue;
            const uint8_t command = vm_read8(buttons.data + b * buttons.stride);
            if (command == k_start_command) lit |= k_start_judged;
            else if (m_shown.sflag(input, inputs.type)) lit |= stick_bits(command);
        }
    }
    return lit;
}

uint32_t live(const std::atomic<uint64_t>& slot)
{
    const uint64_t key = slot.load(std::memory_order_relaxed);
    const uint32_t object = (uint32_t)(key >> 32);
    return key && vm_read32(object + k_object_class) == (uint32_t)key ? object : 0;
}

void remember(std::atomic<uint64_t>& slot, uint32_t object)
{
    if (object) slot.store((uint64_t)object << 32 | vm_read32(object + k_object_class), std::memory_order_relaxed);
}

int face_layout()
{
    const uint32_t save = live(g_save_param);
    if (!save) return 0;
    uint32_t sys_type = 0;
    const uint32_t system = m_system_param.at(save, vm_read32(save + k_object_class), &sys_type);
    const uint32_t key = system ? m_key_config.at(system, sys_type) : 0;
    const uint32_t type = key ? vm_read8(key) : 0;
    return type < k_key_config_types ? (int)type : 0;
}

int ui_context()
{
    static std::atomic<uint64_t> cached{ 0 };
    const uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const uint64_t prev = cached.load(std::memory_order_relaxed);
    if (prev && now - (prev >> 8) < 8u) return (int)(prev & 0xFFu);

    int res = 0;
    const uint32_t scenes = live(g_scene_manager);
    if (scenes) {
        if (m_current_scene.obj(scenes)) res = PAD_CONTEXT_UI;
        else if (const uint32_t pause = m_pause_menu.obj(scenes); pause && m_movie_open.flag(pause)) res = PAD_CONTEXT_UI;
        else if (const uint32_t title = m_title_menu.obj(scenes); title && m_movie_open.flag(title) && m_enable_input.flag(title)) res = PAD_CONTEXT_UI;
        else if (const uint32_t common = m_common.obj(scenes); common && m_dialog_state.byte(common) != k_dialog_idle && m_dialog_type.byte(common) != k_dialog_passive) res = PAD_CONTEXT_UI;
        else if (const uint32_t prompt = live(g_prompt); prompt && m_movie_open.flag(prompt)) res = PAD_CONTEXT_UI;
        else if ((g_stick_lit.load(std::memory_order_relaxed) & k_start_judged) || waits_for_start(live(g_hud))) res = PAD_CONTEXT_START;
    }
    cached.store((now << 8) | (uint64_t)(res & 0xFF), std::memory_order_relaxed);
    return res;
}

uint32_t movie_player(ppu_context* ctx)
{
    uint64_t saved[5];
    std::copy_n(&ctx->gpr[3], std::size(saved), saved);
    const uint64_t link = ctx->lr;
    const uint32_t view = (uint32_t)ctx->gpr[4];
    ctx->gpr[3] = view;
    guest_call(ctx, vm_read32(vm_read32(view) + k_view_user_data));
    const uint32_t movie = (uint32_t)ctx->gpr[3];
    std::copy_n(saved, std::size(saved), &ctx->gpr[3]);
    ctx->lr = link;
    return movie ? vm_read32(movie + k_user_data_player) : 0;
}

std::string text_argument(const ppu_context* ctx, uint32_t i)
{
    std::string text;
    if (i >= (uint32_t)ctx->gpr[7]) return text;
    const uint32_t at = (uint32_t)ctx->gpr[6] + i * k_value_size, type = vm_read32(at + 4);
    if ((type & 0xF) != k_value_string) return text;
    uint32_t chars = vm_read32(at + 8);
    if (type & k_value_managed) chars = vm_read32(chars);
    for (; const char c = (char)vm_read8(chars); ++chars) text += c;
    return text;
}

std::string start_text(std::string text)
{
    if (const size_t at = text.find(k_start_word); pad_kbm_last() && at != std::string::npos)
        text.replace(at, sizeof k_start_word - 1, k_enter_word);
    return text;
}

void quit_game()
{
    static std::atomic<bool> quitting{ false };
    if (quitting.exchange(true, std::memory_order_acq_rel)) return;
    ppu_fs_set_open_callback(nullptr);
    presence_shutdown();
    rl_ach_shutdown();
    SDL_Event e{};
    e.type = SDL_EVENT_QUIT;
    if (SDL_PushEvent(&e)) std::this_thread::sleep_for(std::chrono::seconds(10));
    ps3_savedata_shutdown();
    std::fflush(nullptr);
    std::_Exit(0);
}

void refresh_gfx_viewports(ppu_context* ctx)
{
    const uint32_t engine = vm_read32(k_gfx_engine);
    if (!engine) return;
    const uint32_t data = vm_read32(engine + 0x14u);
    const uint32_t count = vm_read32(engine + 0x18u);
    if (!data || !count || count > 64u) return;
    uint64_t saved_gpr[8];
    double saved_fpr[5];
    std::copy_n(&ctx->gpr[3], 8, saved_gpr);
    std::copy_n(&ctx->fpr[1], 5, saved_fpr);
    const uint64_t saved_lr = ctx->lr;
    {
        const GuestFrame frame(ctx, 0xC0u);
        const uint32_t vp = frame.base + 0x70u;
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t movie = vm_read32(data + i * 4u);
            const uint32_t view = movie ? vm_read32(movie + 0x34u) : 0u;
            const uint32_t vtbl = view ? vm_read32(view) : 0u;
            if (!vtbl) continue;
            ctx->gpr[3] = view;
            ctx->gpr[4] = vp;
            guest_call(ctx, vm_read32(vtbl + k_gfx_get_viewport));
            ctx->gpr[3] = view;
            ctx->gpr[4] = vp;
            guest_call(ctx, vm_read32(vtbl + k_gfx_set_viewport));
        }
    }
    std::copy_n(saved_gpr, 8, &ctx->gpr[3]);
    std::copy_n(saved_fpr, 5, &ctx->fpr[1]);
    ctx->lr = saved_lr;
}

void set_gcm_viewport_x(ppu_context* ctx, float scale_x)
{
    const uint32_t gcm = vm_read32(k_gcm_ctx_ptr);
    if (!gcm) return;
    uint64_t saved_gpr[8];
    double saved_fpr[5];
    std::copy_n(&ctx->gpr[3], 8, saved_gpr);
    std::copy_n(&ctx->fpr[1], 5, saved_fpr);
    const uint64_t saved_lr = ctx->lr;
    {
        const GuestFrame frame(ctx, 0xB0u);
        const uint32_t sc = frame.base + 0x80u;
        const uint32_t of = frame.base + 0x90u;
        vm_write_f32(sc + 0x00u, scale_x);
        vm_write_f32(sc + 0x04u, -360.f);
        vm_write_f32(sc + 0x08u, 1.f);
        vm_write_f32(sc + 0x0Cu, 0.f);
        vm_write_f32(of + 0x00u, 640.f);
        vm_write_f32(of + 0x04u, 360.f);
        vm_write_f32(of + 0x08u, 0.f);
        vm_write_f32(of + 0x0Cu, 0.f);
        vm_write64(frame.base + 0x70u, of);
        ctx->gpr[3] = gcm;
        ctx->gpr[4] = 0;
        ctx->gpr[5] = 0;
        ctx->gpr[6] = 1280;
        ctx->gpr[7] = 720;
        ctx->gpr[10] = sc;
        ctx->fpr[1] = 0.0;
        ctx->fpr[2] = 1.0;
        run(func_00CF1D34, ctx);
    }
    std::copy_n(saved_gpr, 8, &ctx->gpr[3]);
    std::copy_n(saved_fpr, 5, &ctx->fpr[1]);
    ctx->lr = saved_lr;
}

bool external_call(ppu_context* ctx)
{
    char name[24] = {};
    const uint32_t at = (uint32_t)ctx->gpr[5];
    for (uint32_t i = 0; i + 1 < sizeof name && (name[i] = (char)vm_read8(at + i)); i++) {}
    if (std::strncmp(name, k_prefix, sizeof k_prefix - 1)) return false;
    const char* op = name + sizeof k_prefix - 1;
    const double first = argument(ctx, 0);
    const uint32_t row = (uint32_t)first % k_all_rows;
    const Page& page = k_pages[(uint32_t)first % std::size(k_pages)];
    if (!std::strcmp(op, "Rows")) reply_number(ctx, page.end - page.first);
    else if (!std::strcmp(op, "First")) reply_number(ctx, page.first);
    else if (!std::strcmp(op, "Page")) reply(ctx, 0, page.title);
    else if (!std::strcmp(op, "PageHelp")) reply(ctx, 0, page.help);
    else if (!std::strcmp(op, "Title")) reply(ctx, 0, title(row));
    else if (!std::strcmp(op, "Help")) reply(ctx, 0, help(row).c_str());
    else if (!std::strcmp(op, "Count")) reply_number(ctx, count(row));
    else if (!std::strcmp(op, "Label")) reply(ctx, 0, label(row, (uint32_t)argument(ctx, 1)).c_str());
    else if (!std::strncmp(op, "Get", 3)) reply_number(ctx, current(std::strtoul(op + 3, nullptr, 10) % k_all_rows));
    else if (!std::strncmp(op, "Set", 3)) {
        set(std::strtoul(op + 3, nullptr, 10), (uint32_t)first);
        if (g_gfx_aspect_dirty.exchange(false, std::memory_order_acq_rel)) refresh_gfx_viewports(ctx);
    } else if (!std::strcmp(op, "Default")) {
        reset(page);
        if (g_gfx_aspect_dirty.exchange(false, std::memory_order_acq_rel)) refresh_gfx_viewports(ctx);
    } else if (!std::strcmp(op, "Decide")) reply_number(ctx, decide(row));
    else if (!std::strcmp(op, "Capture")) reply_number(ctx, capture(first != 0));
    else if (!std::strcmp(op, "Prompt")) first ? remember(g_prompt, movie_player(ctx)) : g_prompt.store(0, std::memory_order_relaxed);
    else if (!std::strcmp(op, "StartText")) reply(ctx, 0, start_text(text_argument(ctx, 0)).c_str());
    else if (!std::strcmp(op, "Diagram")) g_controller_screen.store(first != 0, std::memory_order_relaxed);
    else if (!std::strcmp(op, "Quit")) quit_game();
    return true;
}

uint64_t fnv1a(const uint8_t* p, uint32_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

uint32_t first_frame(const uint8_t* swf)
{
    uint32_t at = 8u + (5u + 4u * (swf[8] >> 3) + 7u) / 8u + 4u;
    for (;;) {
        const uint32_t head = swf[at] | swf[at + 1] << 8;
        const bool long_form = (head & 0x3F) == 0x3F;
        if (head >> 6 == 1) return at;
        at += (long_form ? 6u + le32(swf + at + 2) : 2u + (head & 0x3F));
    }
}

struct Menu {
    uint32_t size;
    uint64_t hash;
    std::span<const uint8_t> tags;
    uint32_t patched = 0;
    uint32_t patched_size = 0;
};

Menu g_menus[] = {
    { 150929, 0x79296EE1A4592172ull, k_title_menu_tags },
    { 137803, 0xA9057316A0914E31ull, k_pause_menu_tags },
    { 84243, 0x5577A21354B4EAFFull, k_start_menu_tags },
    { 45352, 0xC58DF766DDD3BC80ull, k_logo_menu_tags },
    { 30905, 0x6EDFF88E0597725Cull, k_wide_cover_tags },
    { 51014, 0x8722369DB976359Cull, k_wide_cover_tags },
};
std::mutex g_menu_lock;

void build_menu(ppu_context* ctx, const uint8_t* swf, Menu& menu)
{
    const uint32_t split = first_frame(swf);
    std::vector<uint8_t> out(swf, swf + split);
    out.insert(out.end(), menu.tags.begin(), menu.tags.end());
    out.insert(out.end(), swf + split, swf + menu.size);
    const uint32_t size = (uint32_t)out.size();
    for (int i = 0; i < 4; i++) out[4 + i] = (uint8_t)(size >> 8 * i);
    ctx->gpr[3] = size;
    ctx->gpr[4] = 16;
    run(func_0002AED0, ctx);
    ppu_store_block((uint32_t)ctx->gpr[3], out.data(), size);
    menu.patched_size = size;
    menu.patched = (uint32_t)ctx->gpr[3];
}

}

std::atomic<uint64_t> g_pc_options_hooks[64]{};

void pc_options_boot(const std::filesystem::path& dir)
{
    std::lock_guard<std::mutex> hold(g_settings_lock);
    g_settings_path = dir / "asura_settings.ini";
    load();
    rsx_vk_on_display_toggle(display_toggled);
    rsx_vk_on_display_modes(display_modes_changed);
    rsx_vk_on_window_resize(window_resized);
    pad_set_menu_probe(ui_context);
    pad_set_layout_probe(face_layout);
    pad_set_mouse_route_probe(mouse_route);
    for (uint32_t r = 0; r < k_rows; r++) sync(r);
}

void pc_options_link(uint32_t function, const std::string& path)
{
    if (!function) return;
    for (size_t i = 0; i < std::size(k_hooks); i++) {
        if (path != k_hooks[i].first) continue;
        const std::lock_guard<std::mutex> hold(g_hooks_lock);
        static uint32_t used = 0;
        uint32_t s = (function * 0x9E3779B1u) >> 26;
        for (uint32_t n = 0; n < 64u; ++n, s = (s + 1u) & 63u) {
            const uint64_t cur = g_pc_options_hooks[s].load(std::memory_order_relaxed);
            if ((uint32_t)(cur >> 32) == function) {
                g_pc_options_hooks[s].store((uint64_t)function << 32 | k_hooks[i].second, std::memory_order_release);
                break;
            }
            if (!cur) {
                if (used < 63u) {
                    ++used;
                    g_pc_options_hooks[s].store((uint64_t)function << 32 | k_hooks[i].second, std::memory_order_release);
                }
                break;
            }
        }
        return;
    }
}

uint32_t pc_options_call_hook(uint32_t hook, uint32_t self)
{
    if (hook == hook_judge_update || hook == hook_judge_destroyed) {
        g_stick_lit.store(hook == hook_judge_update ? stick_lit(self) : 0, std::memory_order_relaxed);
        return 0;
    }
    if (hook == hook_scene_manager && m_player_owner.obj(self)) remember(g_hud, self);
    if (hook != hook_stick_rotation) return hook;
    const uint8_t command = m_command.byte(self);
    if (m_judging.flag(self) && command >= k_left_rotation_first && command <= k_left_rotation_last) {
        note_stick_rotation();
        const uint64_t now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        g_spin_cmd.store(command, std::memory_order_relaxed);
        g_spin_ms.store(now_ms, std::memory_order_relaxed);
    }
    return 0;
}

void pc_options_returned(uint32_t hook, uint32_t result)
{
    remember(hook == hook_save_param ? g_save_param : g_scene_manager, vm_read32(result));
}

bool pc_options_controller_screen()
{
    return g_controller_screen.load(std::memory_order_relaxed) && ui_context();
}

uint32_t pc_options_stick_lit()
{
    uint32_t lit = g_stick_lit.load(std::memory_order_relaxed) & ~k_start_judged;
    uint64_t spin_ms = g_spin_ms.load(std::memory_order_relaxed);
    if (spin_ms) {
        const uint64_t now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_ms - spin_ms < 150u) {
            constexpr uint32_t k_cw[4]  = { 1u, 8u, 4u, 2u };
            constexpr uint32_t k_ccw[4] = { 1u, 2u, 4u, 8u };
            const uint32_t phase = (uint32_t)((now_ms / 140u) & 3u);
            const uint8_t cmd = g_spin_cmd.load(std::memory_order_relaxed);
            lit = (lit & ~0xFu) | ((cmd & 1u) ? k_ccw[phase] : k_cw[phase]);
        } else {
            g_spin_ms.compare_exchange_strong(spin_ms, 0, std::memory_order_relaxed);
        }
    }
    return lit;
}

void pc_options_mouse_scale(float* yaw, float* pitch)
{
    *yaw = g_mouse_units.load(std::memory_order_relaxed);
    *pitch = g_invert_mouse.load(std::memory_order_relaxed) ? -*yaw : *yaw;
}

void func_001F43B4(ppu_context* ctx)
{
    const uint32_t tex = (uint32_t)ctx->gpr[4];
    run(func_001F43B4_lifted, ctx);
    const int32_t bias = (int32_t)ctx->gpr[3];
    if (bias < 0 || !is_texture2d(tex)) return;
    const uint32_t mips_data = vm_read32(tex + k_tex_mips);
    const int32_t mips_num = (int32_t)vm_read32(tex + k_tex_mips + 4u);
    if (!mips_data || mips_num <= 1 || bias >= mips_num) return;
    int32_t valid = bias;
    while (valid < mips_num && !mip_has_data(vm_read32(mips_data + (uint32_t)valid * 4u)))
        valid++;
    if (valid < mips_num && valid != bias)
        ctx->gpr[3] = (uint64_t)(int64_t)valid;
}

extern void func_00873378_lifted(ppu_context* ctx);

void func_00873378(ppu_context* ctx)
{
    run(func_00873378_lifted, ctx);
    vm_write32(k_rhi_max_shadow_x, k_shadow_res);
    vm_write32(k_rhi_max_shadow_y, k_shadow_res);
    vm_write32(k_rhi_max_dom_shadow, k_shadow_res);
}

void func_001F8100(ppu_context* ctx)
{
    run(func_001F8100_lifted, ctx);
    std::lock_guard<std::mutex> hold(g_settings_lock);
    g_settings_parsed = true;
    vm_write32(k_use_vsync, g_applied[row_frame_rate] == 30u);
    for (uint32_t g = 0; g < k_sys_tex_lod_count; g++) {
        const uint32_t bias_ea = k_sys_tex_lod_base + g * k_sys_tex_lod_stride + 8u;
        if ((int32_t)vm_read32(bias_ea) > 0) vm_write32(bias_ea, 0u);
    }
    vm_write32(k_sys_shadow_filter_quality_bias, 1u);
    vm_write32(k_sys_min_shadow_res, 128u);
    vm_write32(k_sys_min_preshadow_res, 32u);
    vm_write32(k_sys_max_shadow_res, k_shadow_res);
    vm_write32(k_sys_max_dom_shadow_res, k_shadow_res);
    vm_write32(k_sys_shadow_texels_per_pixel, to_bits(8.0f));
    vm_write32(k_sys_preshadow_res_factor, to_bits(1.0f));
    vm_write32(k_sys_shadow_filter_radius, to_bits(2.0f));
    vm_write32(k_sys_shadow_depth_bias, to_bits(0.012f));
    vm_write32(k_sys_shadow_fade_res, 256u);
    vm_write32(k_sys_preshadow_fade_res, 64u);
}

void func_014749B8(ppu_context* ctx)
{
    if (!external_call(ctx)) run(func_014749B8_lifted, ctx);
}

void func_01472450(ppu_context* ctx)
{
    run(func_01472450_lifted, ctx);
    const uint32_t file = (uint32_t)ctx->gpr[3];
    if (!file || vm_read32(file) != k_memory_file_vtable || vm_read32(file + 0xC) < 8) return;
    const uint32_t size = vm_read32(file + 0xC);
    const uint32_t swf_ea = vm_read32(file + 8);
    if (!swf_ea) return;
    const uint8_t* swf = vm_base + swf_ea;
    for (Menu& menu : g_menus) {
        if (size < menu.size || le32(swf + 4) != menu.size || fnv1a(swf, menu.size) != menu.hash) continue;
        uint32_t patched = 0, patched_size = 0;
        {
            const std::lock_guard<std::mutex> hold(g_menu_lock);
            if (!menu.patched) build_menu(ctx, swf, menu);
            patched = menu.patched;
            patched_size = menu.patched_size;
        }
        vm_write32(file + 8, patched);
        vm_write32(file + 0xC, patched_size);
        ctx->gpr[3] = file;
        return;
    }
}

extern void func_010ED3A0_lifted(ppu_context* ctx);

namespace {

struct captured_view {
    uint32_t mode = RSX_VK_UPSCALE_OFF;
    uint32_t render_w = 1280u;
    uint32_t render_h = 720u;
    float view[16]{};
    float curr_vp[16]{};
    float inv_curr_vp[16]{};
    float fwd[3]{};
    float pos[3]{};
    float jx = 0.f;
    float jy = 0.f;
    float near_clip = 10.f;
    float far_clip = 100000.f;
    float fov_y = 1.04719755f;
};

struct upscale_state {
    std::mutex lock;
    bool has_prev = false;
    bool vel_valid = false;
    uint32_t frame_index = 0;
    uint32_t prev_mode = RSX_VK_UPSCALE_OFF;
    float prev_vp[16]{};
    float prev_fwd[3]{};
    float prev_pos[3]{};
    float prev_fov = 1.04719755f;
    float prev_jx = 0.f;
    float prev_jy = 0.f;
    float vel_coeff_x = 0.f;
    float vel_coeff_y = 0.f;
    std::deque<captured_view> queue;
    std::chrono::steady_clock::time_point last_time{};
} g_up;

void flush_guest_gcm()
{
    const uint32_t gcm = vm_read32(k_gcm_ctx_ptr);
    if (!gcm) return;
    const uint32_t cur = vm_read32(gcm + 8u);
    const uint32_t io = rsx_ea_to_io(cur);
    if (io != 0xFFFFFFFFu) {
        vm_write32(RSX_PUT_EA, io);
        rsx_core_doorbell();
    }
}

}

void func_00214910(ppu_context* ctx)
{
    run(func_00214910_lifted, ctx);
    const uint32_t proxy = (uint32_t)ctx->gpr[3];
    if (!proxy) return;
    if (!g_depth_of_field.load(std::memory_order_relaxed))
        for (const uint32_t field : k_proxy_blur) vm_write32(proxy + field, 0);
    if (!g_bloom.load(std::memory_order_relaxed)) vm_write32(proxy + k_proxy_bloom_scale, 0);
}

void func_010ED3A0(ppu_context* ctx)
{
    const uint32_t proxy = (uint32_t)ctx->gpr[3];
    const bool suppress_mb = proxy && !g_motion_blur.load(std::memory_order_relaxed);
    uint32_t orig_mb = 0;
    if (suppress_mb) {
        orig_mb = vm_read32(proxy + k_proxy_motion_blur);
        vm_write32(proxy + k_proxy_motion_blur, 0);
    }
    run(func_010ED3A0_lifted, ctx);
    if (suppress_mb) vm_write32(proxy + k_proxy_motion_blur, orig_mb);
}

void func_006AC518(ppu_context* ctx)
{
    const bool mb = g_motion_blur.load(std::memory_order_relaxed);
    const uint32_t up = g_upscaler_mode.load(std::memory_order_relaxed);
    if (!mb && up == RSX_VK_UPSCALE_OFF) return;
    if (up == RSX_VK_UPSCALE_OFF) {
        run(func_006AC518_lifted, ctx);
        return;
    }
    const uint32_t dpg = (uint32_t)ctx->gpr[4];
    const uint32_t sr = (uint32_t)ctx->gpr[3];
    const uint32_t views = sr ? vm_read32(sr + 0x58u) : 0u;
    const uint32_t count = sr ? std::min(vm_read32(sr + 0x5Cu), 4u) : 0u;
    struct saved_vel { uint32_t view, fam, vs, mv, ts; } saved[4]{};
    uint32_t nsaved = 0;
    float vel_max = 1.f, vel_scale = 1.f, time_scale = 1.f, aspect = 9.f / 16.f;
    bool have_view = false;
    if (dpg == 1u) {
        for (uint32_t i = 0; i < count && views; ++i) {
            const uint32_t v = views + i * 0x10A0u;
            if ((vm_read32(v + 0xF38u) & 0x40000000u) == 0u) continue;
            const uint32_t fam = vm_read32(v + 0x04u);
            if (!fam) continue;
            const float sx = vm_read_f32(v + 0x38u), sy = vm_read_f32(v + 0x3Cu);
            if (sx < 1024.f || sy < 576.f) continue;
            if (!have_view) {
                aspect = sy / sx;
                vel_scale = mb ? vm_read_f32(v + 0x100Cu) : 1.f;
                vel_max = mb ? vm_read_f32(v + 0x1010u) : 8.f;
                time_scale = mb ? vm_read_f32(fam + 0x238u) : 1.f;
                have_view = true;
            }
            if (!mb) {
                saved[nsaved++] = { v, fam, vm_read32(v + 0x100Cu), vm_read32(v + 0x1010u), vm_read32(fam + 0x238u) };
                vm_write_f32(v + 0x100Cu, 1.f);
                vm_write_f32(v + 0x1010u, 8.f);
                vm_write_f32(fam + 0x238u, 1.f);
            }
        }
    }
    run(func_006AC518_lifted, ctx);
    for (uint32_t i = 0; i < nsaved; ++i) {
        vm_write32(saved[i].view + 0x100Cu, saved[i].vs);
        vm_write32(saved[i].view + 0x1010u, saved[i].mv);
        vm_write32(saved[i].fam + 0x238u, saved[i].ts);
    }
    if (dpg == 1u && have_view && std::fabs(time_scale * vel_scale) > 1e-5f && vel_max > 1e-5f) {
        flush_guest_gcm();
        rsx_vk_upscale_capture_velocity();
        const float cx = -2.f * (vel_max * 0.0125f) / (time_scale * vel_scale);
        const float cy = cx / aspect;
        std::lock_guard<std::mutex> hold(g_up.lock);
        g_up.vel_coeff_x = cx;
        g_up.vel_coeff_y = cy;
        g_up.vel_valid = true;
    }
}

extern void func_00661DA8_lifted(ppu_context* ctx);
extern void func_00662E70_lifted(ppu_context* ctx);
extern void func_006784C4_lifted(ppu_context* ctx);

namespace {

void mat4_mul_f64(const float a[16], const float b[16], float out[16])
{
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k)
                s += (double)a[r * 4 + k] * (double)b[k * 4 + c];
            out[r * 4 + c] = (float)s;
        }
    }
}

bool mat4_invert_f64(const float in[16], float out[16])
{
    double m[4][8]{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            m[r][c] = (double)in[r * 4 + c];
            m[r][c + 4] = (r == c) ? 1.0 : 0.0;
        }
    }
    for (int i = 0; i < 4; ++i) {
        int piv = i;
        double best = std::fabs(m[i][i]);
        for (int r = i + 1; r < 4; ++r) {
            const double v = std::fabs(m[r][i]);
            if (v > best) {
                best = v;
                piv = r;
            }
        }
        if (best < 1e-18) return false;
        if (piv != i) {
            for (int c = 0; c < 8; ++c) std::swap(m[i][c], m[piv][c]);
        }
        const double inv = 1.0 / m[i][i];
        for (int c = 0; c < 8; ++c) m[i][c] *= inv;
        for (int r = 0; r < 4; ++r) {
            if (r == i) continue;
            const double f = m[r][i];
            for (int c = 0; c < 8; ++c) m[r][c] -= f * m[i][c];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r * 4 + c] = (float)m[r][c + 4];
    return true;
}

}

static void widen_scene_view(ppu_context* ctx, void (*lifted)(ppu_context*))
{
    if (__builtin_expect(g_gfx_aspect_dirty.exchange(false, std::memory_order_acq_rel), 0))
        refresh_gfx_viewports(ctx);
    const float aspect = g_aspect_scale.load(std::memory_order_relaxed);
    const uint32_t up_mode = g_upscaler_mode.load(std::memory_order_relaxed);
    uint32_t proj_ea = 0, orig_m00 = 0, orig_m20 = 0, orig_m21 = 0;
    bool patched = false, jittered = false;
    const float sx = (float)ctx->fpr[3], sy = (float)ctx->fpr[4];
    const bool wide_view = sx >= 16.f && sy >= 9.f && sx * 10.f >= sy * 15.f;
    if (wide_view && (aspect > 1.f || up_mode != RSX_VK_UPSCALE_OFF)) {
        proj_ea = vm_read32((uint32_t)ctx->gpr[1] + 0xACu);
        if (proj_ea && vm_read_f32(proj_ea + 0x2Cu) != 0.f && vm_read_f32(proj_ea + 0x3Cu) == 0.f) {
            if (aspect > 1.f) {
                orig_m00 = vm_read32(proj_ea);
                vm_write_f32(proj_ea, from_bits(orig_m00) / aspect);
                patched = true;
            }
            if (up_mode != RSX_VK_UPSCALE_OFF && sx >= 1024.f && sy >= 576.f &&
                std::fabs(vm_read_f32(proj_ea + 0x20u)) < 1e-6f && std::fabs(vm_read_f32(proj_ea + 0x24u)) < 1e-6f) {
                const uint32_t view_ea = vm_read32((uint32_t)ctx->gpr[1] + 0xA4u);
                if (view_ea) {
                    float view[16], proj[16], vp[16], inv_vp[16];
                    for (uint32_t i = 0; i < 16u; ++i) {
                        view[i] = vm_read_f32(view_ea + i * 4u);
                        proj[i] = vm_read_f32(proj_ea + i * 4u);
                    }
                    mat4_mul_f64(view, proj, vp);
                    if (mat4_invert_f64(vp, inv_vp)) {
                        const uint32_t dw = g_eff_w.load(std::memory_order_relaxed);
                        const uint32_t dh = g_eff_h.load(std::memory_order_relaxed);
                        uint32_t rw = dw, rh = dh;
                        rsx_vk_upscale_render_size(dw, dh, (int)up_mode, &rw, &rh);
                        if (rw && rh) {
                            float jx = 0.f, jy = 0.f;
                            {
                                std::lock_guard<std::mutex> hold(g_up.lock);
                                const uint32_t fi = g_up.frame_index++;
                                rsx_vk_upscale_jitter((int)up_mode, rw, dw, fi, &jx, &jy);
                                captured_view cv{};
                                cv.mode = up_mode;
                                cv.render_w = rw;
                                cv.render_h = rh;
                                std::memcpy(cv.view, view, sizeof view);
                                std::memcpy(cv.curr_vp, vp, sizeof vp);
                                std::memcpy(cv.inv_curr_vp, inv_vp, sizeof inv_vp);
                                cv.fwd[0] = view[2];
                                cv.fwd[1] = view[6];
                                cv.fwd[2] = view[10];
                                cv.pos[0] = -(view[12] * view[0] + view[13] * view[1] + view[14] * view[2]);
                                cv.pos[1] = -(view[12] * view[4] + view[13] * view[5] + view[14] * view[6]);
                                cv.pos[2] = -(view[12] * view[8] + view[13] * view[9] + view[14] * view[10]);
                                cv.jx = jx;
                                cv.jy = jy;
                                const float m22 = proj[10], m32 = proj[14], m11 = proj[5];
                                cv.near_clip = std::fabs(m22) > 1e-6f ? std::max(0.01f, -m32 / m22) : 10.f;
                                cv.far_clip = (m22 > 1.000001f)
                                    ? std::max(cv.near_clip + 1.f, -m32 / (m22 - 1.f)) : 100000.f;
                                cv.fov_y = std::fabs(m11) > 1e-6f ? 2.f * std::atan(1.f / std::fabs(m11)) : 1.04719755f;
                                if (g_up.queue.size() >= 6u) g_up.queue.pop_front();
                                g_up.queue.push_back(cv);
                            }
                            orig_m20 = vm_read32(proj_ea + 0x20u);
                            orig_m21 = vm_read32(proj_ea + 0x24u);
                            vm_write_f32(proj_ea + 0x20u, from_bits(orig_m20) + (2.f * jx) / (float)rw);
                            vm_write_f32(proj_ea + 0x24u, from_bits(orig_m21) + (-2.f * jy) / (float)rh);
                            jittered = true;
                        }
                    }
                }
            }
        }
    }
    run(lifted, ctx);
    if (jittered) {
        vm_write32(proj_ea + 0x20u, orig_m20);
        vm_write32(proj_ea + 0x24u, orig_m21);
    }
    if (patched) vm_write32(proj_ea, orig_m00);
}

void func_00661DA8(ppu_context* ctx) { widen_scene_view(ctx, func_00661DA8_lifted); }
void func_00662E70(ppu_context* ctx) { widen_scene_view(ctx, func_00662E70_lifted); }

void func_006784C4(ppu_context* ctx)
{
    const uint32_t sr = (uint32_t)ctx->gpr[3];
    bool main_view = false;
    float cur_view[16]{};
    if (sr) {
        const uint32_t views = vm_read32(sr + 0x58u);
        const uint32_t count = vm_read32(sr + 0x5Cu);
        if (views && count) {
            const float sx = vm_read_f32(views + 0x38u), sy = vm_read_f32(views + 0x3Cu);
            main_view = sx >= 1024.f && sy >= 576.f;
            if (main_view) {
                for (uint32_t i = 0; i < 16u; ++i)
                    cur_view[i] = vm_read_f32(views + 0x60u + i * 4u);
            }
        }
    }
    run(func_006784C4_lifted, ctx);
    if (!main_view) return;
    const uint32_t up_mode = g_upscaler_mode.load(std::memory_order_relaxed);
    if (up_mode == RSX_VK_UPSCALE_OFF) {
        std::lock_guard<std::mutex> hold(g_up.lock);
        g_up.queue.clear();
        g_up.has_prev = false;
        g_up.vel_valid = false;
        return;
    }
    rsx_vk_upscale_frame frame{};
    {
        std::lock_guard<std::mutex> hold(g_up.lock);
        if (g_up.queue.empty()) return;
        size_t match = g_up.queue.size();
        for (size_t i = 0; i < g_up.queue.size(); ++i) {
            if (std::memcmp(g_up.queue[i].view, cur_view, sizeof cur_view) == 0) {
                match = i;
                break;
            }
        }
        if (match < g_up.queue.size()) {
            for (size_t i = 0; i < match; ++i) g_up.queue.pop_front();
        }
        const captured_view cv = g_up.queue.front();
        g_up.queue.pop_front();
        const auto now = std::chrono::steady_clock::now();
        float raw_dt_ms = 16.6667f;
        if (g_up.last_time.time_since_epoch().count() != 0)
            raw_dt_ms = std::chrono::duration<float, std::milli>(now - g_up.last_time).count();
        const float dt_ms = std::clamp(raw_dt_ms, 1.f, 250.f);
        g_up.last_time = now;
        int reset = (!g_up.has_prev || g_up.prev_mode != cv.mode || g_up.prev_mode != up_mode || raw_dt_ms > 150.f) ? 1 : 0;
        if (!reset) {
            const float dot = cv.fwd[0] * g_up.prev_fwd[0] + cv.fwd[1] * g_up.prev_fwd[1] + cv.fwd[2] * g_up.prev_fwd[2];
            const float dx = cv.pos[0] - g_up.prev_pos[0];
            const float dy = cv.pos[1] - g_up.prev_pos[1];
            const float dz = cv.pos[2] - g_up.prev_pos[2];
            if (dot < 0.85f || (dx * dx + dy * dy + dz * dz) > 10000.f || std::fabs(cv.fov_y - g_up.prev_fov) > 0.08f)
                reset = 1;
        }
        std::memcpy(frame.inv_curr_vp, cv.inv_curr_vp, sizeof frame.inv_curr_vp);
        std::memcpy(frame.prev_vp, reset ? cv.curr_vp : g_up.prev_vp, sizeof frame.prev_vp);
        frame.jitter_x = cv.jx;
        frame.jitter_y = cv.jy;
        frame.near_clip = cv.near_clip;
        frame.far_clip = cv.far_clip;
        frame.fov_y = cv.fov_y;
        frame.dt_ms = dt_ms;
        frame.sharpness = (float)g_fsr_sharpness.load(std::memory_order_relaxed) * 0.01f;
        frame.vel_scale_x = g_up.vel_valid ? g_up.vel_coeff_x * (float)cv.render_w : 0.f;
        frame.vel_scale_y = g_up.vel_valid ? g_up.vel_coeff_y * (float)cv.render_h : 0.f;
        frame.prev_jitter_x = reset ? cv.jx : g_up.prev_jx;
        frame.prev_jitter_y = reset ? cv.jy : g_up.prev_jy;
        frame.reset = reset;
        g_up.vel_valid = false;
        g_up.prev_jx = cv.jx;
        g_up.prev_jy = cv.jy;
        g_up.prev_fov = cv.fov_y;
        std::memcpy(g_up.prev_vp, cv.curr_vp, sizeof g_up.prev_vp);
        std::memcpy(g_up.prev_fwd, cv.fwd, sizeof g_up.prev_fwd);
        std::memcpy(g_up.prev_pos, cv.pos, sizeof g_up.prev_pos);
        g_up.prev_mode = up_mode;
        g_up.has_prev = true;
    }
    flush_guest_gcm();
    rsx_vk_upscale_trigger(&frame);
}

extern void func_00D71F04_lifted(ppu_context* ctx);

void func_00D71F04(ppu_context* ctx)
{
    if (__builtin_expect(g_gfx_aspect_dirty.exchange(false, std::memory_order_acq_rel), 0))
        refresh_gfx_viewports(ctx);
    const float aspect = g_aspect_scale.load(std::memory_order_relaxed);
    const uint32_t vp = (uint32_t)ctx->gpr[4];
    uint32_t orig_ar = 0;
    bool patched = false;
    if (vp) {
        const uint32_t bw = vm_read32(vp + 0x00u);
        const uint32_t bh = vm_read32(vp + 0x04u);
        const uint32_t flags = vm_read32(vp + 0x30u);
        if (!(flags & 1u) && bw >= 1280u && (uint64_t)bw * 9u == (uint64_t)bh * 16u) {
            orig_ar = vm_read32(vp + 0x2Cu);
            const uint32_t want = to_bits(aspect);
            if (orig_ar != want) {
                vm_write32(vp + 0x2Cu, want);
                patched = true;
            }
        }
    }
    run(func_00D71F04_lifted, ctx);
    if (patched) vm_write32(vp + 0x2Cu, orig_ar);
}

extern void func_002BE2A8_lifted(ppu_context* ctx);

void func_002BE2A8(ppu_context* ctx)
{
    const float aspect = g_aspect_scale.load(std::memory_order_relaxed);
    const bool pillarbox = aspect > 1.f && (uint32_t)ctx->lr == k_bink_fullscreen_lr;
    if (pillarbox) set_gcm_viewport_x(ctx, 640.f / aspect);
    run(func_002BE2A8_lifted, ctx);
    if (pillarbox) set_gcm_viewport_x(ctx, 640.f);
}
