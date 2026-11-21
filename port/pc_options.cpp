#include "guest.h"
#include "pc_options.h"
#include "rsx_core.h"
#include "vk/rsx_vk.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

#include "option_menu_tags.inc"

constexpr uint32_t k_memory_file_vtable = 0x01751910;
constexpr uint32_t k_set_return_value = 0xCC;
constexpr uint32_t k_value_number = 3;
constexpr uint32_t k_value_string = 4;
constexpr uint32_t k_value_size = 0x10;
constexpr uint32_t k_proxy_blur[] = { 0x14, 0x18, 0x1C };
constexpr uint32_t k_proxy_bloom_scale = 0x38;
constexpr uint32_t k_proxy_motion_blur = 0x110;
constexpr uint32_t k_use_vsync = 0x01A56E04;
constexpr char k_prefix[] = "PCOpt_";

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

constexpr uint32_t k_max_scale = 8;

std::vector<rsx_vk_mode> display_modes()
{
    std::vector<rsx_vk_mode> modes;
    for (;;) {
        const uint32_t n = rsx_vk_display_modes(nullptr, 0);
        modes.resize(n);
        if (rsx_vk_display_modes(modes.data(), n) == n) return modes;
    }
}

constexpr uint32_t k_resolution_heights[] = { 720, 900, 1080, 1440, 1800, 2160, 2880, 4320 };

std::optional<uint32_t> parse_resolution(const std::string& token)
{
    uint32_t w = 0, h = 0;
    if (std::sscanf(token.c_str(), "%ux%u", &w, &h) != 2 || h < 720 || h > 720 * k_max_scale || w * 9 != h * 16) return std::nullopt;
    return h;
}

std::optional<uint32_t> parse_frame_rate(const std::string& token)
{
    if (token == "unlimited") return 0u;
    const uint32_t hz = (uint32_t)std::strtoul(token.c_str(), nullptr, 10);
    return hz ? std::optional<uint32_t>(hz) : std::nullopt;
}

Choices display_choices() { return k_display; }
Choices switch_choices() { return k_switch; }

Choices resolution_choices()
{
    const std::vector<rsx_vk_mode> modes = display_modes();
    if (modes.empty()) return {};
    const rsx_vk_mode native = *std::max_element(modes.begin(), modes.end(), [](const rsx_vk_mode& a, const rsx_vk_mode& b) {
        return (uint64_t)a.width * a.height < (uint64_t)b.width * b.height;
    });
    std::vector<uint32_t> heights(std::begin(k_resolution_heights), std::end(k_resolution_heights));
    if (parse_resolution(std::to_string(native.width) + "x" + std::to_string(native.height))) heights.push_back(native.height);
    std::sort(heights.begin(), heights.end());
    heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
    const auto distance = [&](uint32_t h) { return h > native.height ? h - native.height : native.height - h; };
    const uint32_t preferred = *std::min_element(heights.begin(), heights.end(), [&](uint32_t a, uint32_t b) { return distance(a) < distance(b); });
    Choices list;
    for (const uint32_t h : heights) {
        const std::string w = std::to_string(h * 16 / 9);
        list.push_back({ w + "x" + std::to_string(h), w + " x " + std::to_string(h), h, h == preferred });
    }
    return list;
}

Choices frame_rate_choices()
{
    const std::vector<rsx_vk_mode> modes = display_modes();
    if (modes.empty()) return {};
    std::vector<uint32_t> rates;
    for (const rsx_vk_mode& m : modes)
        if (const uint32_t hz = (uint32_t)std::lround(m.refresh); hz && hz != 30) rates.push_back(hz);
    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    Choices list{ { "30", "30", 30 } };
    for (const uint32_t hz : rates) list.push_back({ std::to_string(hz), std::to_string(hz), hz });
    list.push_back({ "unlimited", "Unlimited", 0 });
    return list;
}

enum : uint32_t { row_display = 0, row_frame_rate = 2 };

bool g_settings_parsed;
std::atomic<bool> g_motion_blur{ true };
std::atomic<bool> g_depth_of_field{ true };
std::atomic<bool> g_bloom{ true };

void apply_display(uint32_t value) { rsx_vk_set_display((rsx_vk_display)value); }
void apply_resolution(uint32_t value) { rsx_vk_set_resolution_scale(value, 720); }
void apply_frame_rate(uint32_t value)
{
    const bool console = value == 30;
    rsx_core_set_frame_limit(console ? 0 : value);
    rsx_vk_set_free_flips(!console);
    if (g_settings_parsed) vm_write32(k_use_vsync, console);
}
void apply_vsync(uint32_t value) { rsx_vk_set_vsync((int)value); }
void apply_motion_blur(uint32_t value) { g_motion_blur.store(value != 0, std::memory_order_relaxed); }
void apply_depth_of_field(uint32_t value) { g_depth_of_field.store(value != 0, std::memory_order_relaxed); }
void apply_bloom(uint32_t value) { g_bloom.store(value != 0, std::memory_order_relaxed); }

struct Option {
    const char* key;
    const char* title;
    const char* help;
    Choices (*choices)();
    const char* initial;
    std::optional<uint32_t> (*parse)(const std::string& token);
    void (*apply)(uint32_t value);
};

const Option k_options[] = {
    { "display", "Display Mode", "Windowed, borderless window, or exclusive fullscreen at the display's native mode.", display_choices, "borderless", nullptr, apply_display },
    { "resolution", "Resolution", "Internal rendering resolution.", resolution_choices, nullptr, parse_resolution, apply_resolution },
    { "frame_rate", "Frame Rate", "Frame rate limit. 30 uses the console's original pacing.", frame_rate_choices, "unlimited", parse_frame_rate, apply_frame_rate },
    { "vsync", "VSync", "Paces frames to the display refresh. Off shows each new frame as soon as it is ready.", switch_choices, "off", nullptr, apply_vsync },
    { "motion_blur", "Motion Blur", "Blur from camera and object motion.", switch_choices, "on", nullptr, apply_motion_blur },
    { "depth_of_field", "Depth of Field", "Blur outside the camera's focus, mainly in cutscenes.", switch_choices, "on", nullptr, apply_depth_of_field },
    { "bloom", "Bloom", "Glow around bright lights.", switch_choices, "on", nullptr, apply_bloom },
};

constexpr uint32_t k_rows = (uint32_t)std::size(k_options);
std::string g_saved[k_rows];
std::optional<uint32_t> g_applied[k_rows];
std::mutex g_settings_lock;

std::string settings_path()
{
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string dir(path, n);
    dir.resize(dir.find_last_of("\\/") + 1);
    return dir + "asura_settings.ini";
}

void load()
{
    FILE* f = std::fopen(settings_path().c_str(), "r");
    if (!f) return;
    char line[128];
    while (std::fgets(line, sizeof line, f)) {
        line[std::strcspn(line, "\r\n")] = 0;
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        for (uint32_t r = 0; r < k_rows; r++)
            if (!std::strcmp(line, k_options[r].key)) g_saved[r] = eq + 1;
    }
    std::fclose(f);
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
    const Choices list = k_options[row].choices();
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
    FILE* f = std::fopen(settings_path().c_str(), "w");
    if (!f) return;
    for (uint32_t r = 0; r < k_rows; r++) {
        const Choices list = k_options[r].choices();
        const auto c = selected(r, list);
        const std::string token = !g_saved[r].empty() ? g_saved[r] : c ? list[*c].token : std::string();
        if (!token.empty()) std::fprintf(f, "%s=%s\n", k_options[r].key, token.c_str());
    }
    std::fclose(f);
}

void set(uint32_t row, uint32_t index)
{
    std::lock_guard<std::mutex> hold(g_settings_lock);
    if (row >= k_rows) return;
    const Choices list = k_options[row].choices();
    if (index >= list.size() || selected(row, list) == index) return;
    g_saved[row] = list[index].token;
    sync(row);
    save();
}

void reset()
{
    std::lock_guard<std::mutex> hold(g_settings_lock);
    for (uint32_t r = 0; r < k_rows; r++) {
        g_saved[r] = k_options[r].initial ? k_options[r].initial : "";
        sync(r);
    }
    save();
}

void display_toggled(rsx_vk_display display)
{
    std::lock_guard<std::mutex> hold(g_settings_lock);
    for (const Choice& c : k_display)
        if (c.value == (uint32_t)display) g_saved[row_display] = c.token;
    g_applied[row_display] = (uint32_t)display;
    save();
}

void display_modes_changed()
{
    std::lock_guard<std::mutex> hold(g_settings_lock);
    for (uint32_t r = 0; r < k_rows; r++) sync(r);
}

uint32_t count(uint32_t row)
{
    std::lock_guard<std::mutex> hold(g_settings_lock);
    const Choices list = k_options[row].choices();
    return list.empty() || list[0].label.empty() ? 0 : (uint32_t)list.size();
}

std::string label(uint32_t row, uint32_t index)
{
    std::lock_guard<std::mutex> hold(g_settings_lock);
    const Choices list = k_options[row].choices();
    return index < list.size() ? list[index].label : std::string();
}

uint32_t current(uint32_t row)
{
    std::lock_guard<std::mutex> hold(g_settings_lock);
    return selected(row, k_options[row].choices()).value_or(0);
}

double argument(const ppu_context* ctx, uint32_t i)
{
    if (i >= (uint32_t)ctx->gpr[7]) return 0;
    const uint32_t at = (uint32_t)ctx->gpr[6] + i * k_value_size;
    if ((vm_read32(at + 4) & 0xF) != k_value_number) return 0;
    const uint64_t bits = vm_read64(at + 8);
    double v;
    std::memcpy(&v, &bits, sizeof v);
    return v;
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
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    reply(ctx, bits, nullptr);
}

bool external_call(ppu_context* ctx)
{
    char name[24] = {};
    const uint32_t at = (uint32_t)ctx->gpr[5];
    for (uint32_t i = 0; i + 1 < sizeof name && (name[i] = (char)vm_read8(at + i)); i++) {}
    if (std::strncmp(name, k_prefix, sizeof k_prefix - 1)) return false;
    const char* op = name + sizeof k_prefix - 1;
    const double first = argument(ctx, 0);
    const uint32_t row = (uint32_t)first % k_rows;
    if (!std::strcmp(op, "Rows")) reply_number(ctx, k_rows);
    else if (!std::strcmp(op, "Page")) reply(ctx, 0, "Graphics");
    else if (!std::strcmp(op, "PageHelp")) reply(ctx, 0, "Display, resolution, frame rate and image effects.");
    else if (!std::strcmp(op, "Title")) reply(ctx, 0, k_options[row].title);
    else if (!std::strcmp(op, "Help")) reply(ctx, 0, k_options[row].help);
    else if (!std::strcmp(op, "Count")) reply_number(ctx, count(row));
    else if (!std::strcmp(op, "Label")) reply(ctx, 0, label(row, (uint32_t)argument(ctx, 1)).c_str());
    else if (!std::strncmp(op, "Get", 3)) reply_number(ctx, current(std::strtoul(op + 3, nullptr, 10) % k_rows));
    else if (!std::strncmp(op, "Set", 3)) set(std::strtoul(op + 3, nullptr, 10), (uint32_t)first);
    else if (!std::strcmp(op, "Default")) reset();
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

void pc_options_boot()
{
    std::lock_guard<std::mutex> hold(g_settings_lock);
    load();
    rsx_vk_on_display_toggle(display_toggled);
    rsx_vk_on_display_modes(display_modes_changed);
    for (uint32_t r = 0; r < k_rows; r++) sync(r);
}

void func_001F8100(ppu_context* ctx)
{
    run(func_001F8100_lifted, ctx);
    std::lock_guard<std::mutex> hold(g_settings_lock);
    g_settings_parsed = true;
    vm_write32(k_use_vsync, g_applied[row_frame_rate] == 30u);
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
    const uint8_t* swf = vm_base + vm_read32(file + 8);
    for (Menu& menu : g_menus) {
        if (size < menu.size || le32(swf + 4) != menu.size || fnv1a(swf, menu.size) != menu.hash) continue;
        {
            const std::lock_guard<std::mutex> hold(g_menu_lock);
            if (!menu.patched) build_menu(ctx, swf, menu);
        }
        vm_write32(file + 8, menu.patched);
        vm_write32(file + 0xC, menu.patched_size);
        ctx->gpr[3] = file;
        return;
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
    if (!g_motion_blur.load(std::memory_order_relaxed)) vm_write32(proxy + k_proxy_motion_blur, 0);
}

void func_006AC518(ppu_context* ctx)
{
    if (g_motion_blur.load(std::memory_order_relaxed)) run(func_006AC518_lifted, ctx);
}
