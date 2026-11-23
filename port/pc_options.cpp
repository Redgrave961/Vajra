#include "guest.h"
#include "pc_options.h"
#include "vk/rsx_vk.h"
#include "../ps3recomp/libs/input/pad_bindings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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
constexpr uint32_t k_gnames = 0x01A0B014;
constexpr uint32_t k_object_name = 0x2C;
constexpr uint32_t k_object_class = 0x34;
constexpr uint32_t k_field_next = 0x3C;
constexpr uint32_t k_struct_super = 0x40;
constexpr uint32_t k_struct_children = 0x44;
constexpr uint32_t k_property_element = 0x44;
constexpr uint32_t k_property_offset = 0x54;
constexpr uint32_t k_property_detail = 0x64;
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

constexpr uint32_t k_max_scale = 8;

std::vector<rsx_vk_mode> g_modes;
bool g_modes_valid = false;

const std::vector<rsx_vk_mode>& display_modes()
{
    if (!g_modes_valid) {
        for (;;) {
            const uint32_t n = rsx_vk_display_modes(nullptr, 0);
            g_modes.resize(n);
            if (rsx_vk_display_modes(g_modes.data(), n) == n) break;
        }
        g_modes_valid = !g_modes.empty();
    }
    return g_modes;
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

constexpr uint32_t k_sensitivity_steps = 20;

Choices sensitivity_choices()
{
    Choices list;
    for (uint32_t n = 1; n <= k_sensitivity_steps; n++) list.push_back({ std::to_string(n), std::to_string(n), n });
    return list;
}

Choices resolution_choices()
{
    const std::vector<rsx_vk_mode>& modes = display_modes();
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
    const std::vector<rsx_vk_mode>& modes = display_modes();
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

enum : uint32_t { row_display = 0, row_frame_rate = 2, row_mouse = 6 };

bool g_settings_parsed;
std::atomic<bool> g_motion_blur{ true };
std::atomic<bool> g_depth_of_field{ true };
std::atomic<bool> g_bloom{ true };
std::atomic<float> g_mouse_units{ 30.f };
std::atomic<bool> g_invert_mouse{ false };

void apply_display(uint32_t value) { rsx_vk_set_display((rsx_vk_display)value); }
void apply_resolution(uint32_t value) { rsx_vk_set_resolution_scale(value, 720); }
void apply_frame_rate(uint32_t value)
{
    const bool console = value == 30;
    rsx_vk_set_frame_rate(!console, console ? 0 : value);
    if (g_settings_parsed) vm_write32(k_use_vsync, console);
}
void apply_motion_blur(uint32_t value) { g_motion_blur.store(value != 0, std::memory_order_relaxed); }
void apply_depth_of_field(uint32_t value) { g_depth_of_field.store(value != 0, std::memory_order_relaxed); }
void apply_bloom(uint32_t value) { g_bloom.store(value != 0, std::memory_order_relaxed); }
void apply_sensitivity(uint32_t value) { g_mouse_units.store(value * k_mouse_units_per_step, std::memory_order_relaxed); }
void apply_invert_mouse(uint32_t value) { g_invert_mouse.store(value != 0, std::memory_order_relaxed); }

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
    { "motion_blur", "Motion Blur", "Blur from camera and object motion.", switch_choices, "on", nullptr, apply_motion_blur },
    { "depth_of_field", "Depth of Field", "Blur outside the camera's focus, mainly in cutscenes.", switch_choices, "on", nullptr, apply_depth_of_field },
    { "bloom", "Bloom", "Glow around bright lights.", switch_choices, "on", nullptr, apply_bloom },
    { "mouse_sensitivity", "Sensitivity", "Mouse: How fast the camera turns with the mouse.", sensitivity_choices, "10", nullptr, apply_sensitivity },
    { "invert_mouse_y", "Invert Y", "Mouse: Moving the mouse forward looks down.", switch_choices, "off", nullptr, apply_invert_mouse },
};

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
    { "Graphics", "Display, resolution, frame rate and image effects.", 0, row_mouse },
    { "Keyboard & Mouse", "Key and mouse button bindings, mouse sensitivity and inversion.", row_mouse, k_all_rows },
};

std::atomic<int> g_capture{ -1 };
uint32_t g_slot[std::size(k_bindings)];
int g_swap_row = -1;
std::string g_swap_text;
constexpr uint64_t hook_bit(uint32_t fn) { return 1ull << ((fn * 0x9E3779B1u) >> 26); }

std::atomic<uint64_t> g_hooks[std::size(k_hooks)];
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
    FILE* f = std::fopen(g_settings_path.string().c_str(), "r");
    if (!f) return;
    char line[128];
    while (std::fgets(line, sizeof line, f)) {
        line[std::strcspn(line, "\r\n")] = 0;
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        for (uint32_t r = 0; r < k_rows; r++)
            if (!std::strcmp(line, k_options[r].key)) g_saved[r] = eq + 1;
        if (std::strncmp(line, k_bind_prefix, sizeof k_bind_prefix - 1)) continue;
        for (int a = 0; a < PAD_ACTION_COUNT; a++) {
            if (std::strcmp(line + sizeof k_bind_prefix - 1, k_action_keys[a])) continue;
            char* at = eq + 1;
            for (int s = 0; s < PAD_BINDING_SLOTS; s++, at += *at == ',')
                pad_binding_set(a, s, (int)std::strtol(at, &at, 10));
        }
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
    FILE* f = std::fopen(g_settings_path.string().c_str(), "w");
    if (!f) return;
    for (uint32_t r = 0; r < k_rows; r++) {
        std::string token = g_saved[r];
        if (token.empty()) {
            const Choices list = k_options[r].choices();
            if (const auto c = selected(r, list)) token = list[*c].token;
        }
        if (!token.empty()) std::fprintf(f, "%s=%s\n", k_options[r].key, token.c_str());
    }
    for (int a = 0; a < PAD_ACTION_COUNT; a++) {
        std::fprintf(f, "%s%s=", k_bind_prefix, k_action_keys[a]);
        for (int s = 0; s < PAD_BINDING_SLOTS; s++) std::fprintf(f, s ? ",%d" : "%d", pad_binding(a, s));
        std::fputc('\n', f);
    }
    std::fclose(f);
}

void set(uint32_t row, uint32_t index)
{
    if (binding_row(row) && index < PAD_BINDING_SLOTS && g_slot[row - k_rows] != index) {
        g_slot[row - k_rows] = index;
        g_swap_row = -1;
    }
    std::lock_guard<std::mutex> hold(g_settings_lock);
    if (row >= k_rows) return;
    const Choices list = k_options[row].choices();
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
    for (uint32_t r = 0; r < k_rows; r++) sync(r);
}

uint32_t count(uint32_t row)
{
    if (row >= k_rows) return binding_row(row) ? PAD_BINDING_SLOTS : 1;
    std::lock_guard<std::mutex> hold(g_settings_lock);
    const Choices list = k_options[row].choices();
    return list.empty() || list[0].label.empty() ? 0 : (uint32_t)list.size();
}

std::string label(uint32_t row, uint32_t index)
{
    if (row == k_reset_row) return "Reset";
    if (row >= k_rows) return binding_label(row, index);
    std::lock_guard<std::mutex> hold(g_settings_lock);
    const Choices list = k_options[row].choices();
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

bool object_name_eq(uint32_t object, const char* want)
{
    if (vm_read32(object + k_object_name + 4)) return false;
    const uint32_t entry = vm_read32(vm_read32(k_gnames) + vm_read32(object + k_object_name) * 4);
    if (vm_read32(entry) & 1) return false;
    for (uint32_t at = entry + 8;; ++at, ++want) {
        const char c = (char)vm_read8(at);
        if (c != *want) return false;
        if (!c) return true;
    }
}

struct Member {
    struct Entry {
        std::atomic<uint32_t> owner{ 0 };
        uint32_t offset = 0;
        uint32_t detail = 0;
        bool found = false;
    };
    static constexpr uint32_t k_cap = 8;

    const char* name;
    std::atomic<uint32_t> detail{ 0 };
    std::atomic<uint32_t> count{ 0 };
    Entry cache[k_cap]{};
    std::mutex lock{};

    const Entry* bind(uint32_t cls)
    {
        const uint32_t n = count.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n; ++i)
            if (cache[i].owner.load(std::memory_order_relaxed) == cls) return &cache[i];

        std::lock_guard<std::mutex> guard(lock);
        const uint32_t n2 = count.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n2; ++i)
            if (cache[i].owner.load(std::memory_order_relaxed) == cls) return &cache[i];

        uint32_t field = 0;
        for (uint32_t c = cls; c && !field; c = vm_read32(c + k_struct_super))
            for (uint32_t f = vm_read32(c + k_struct_children); f && !field; f = vm_read32(f + k_field_next))
                if (object_name_eq(f, name)) field = f;

        const uint32_t slot = n2 < k_cap ? n2 : (k_cap - 1);
        Entry& e = cache[slot];
        e.found = field != 0;
        e.offset = field ? vm_read32(field + k_property_offset) : 0;
        e.detail = field ? vm_read32(field + k_property_detail) : 0;
        e.owner.store(cls, std::memory_order_relaxed);
        if (n2 < k_cap) count.store(n2 + 1, std::memory_order_release);
        return &e;
    }

    uint32_t at(uint32_t base, uint32_t cls)
    {
        const Entry* e = bind(cls);
        detail.store(e->detail, std::memory_order_relaxed);
        return e->found ? base + e->offset : 0;
    }

    uint32_t object(uint32_t object) { const uint32_t a = at(object, vm_read32(object + k_object_class)); return a ? vm_read32(a) : 0; }
    uint8_t byte(uint32_t object) { const uint32_t a = at(object, vm_read32(object + k_object_class)); return a ? vm_read8(a) : 0; }
    bool flag(uint32_t object)
    {
        const Entry* e = bind(vm_read32(object + k_object_class));
        return e->found && (vm_read32(object + e->offset) & e->detail);
    }
};

Member m_current_scene{ "CurrentScene" }, m_pause_menu{ "PauseMenu" }, m_common{ "Common" }, m_movie_open{ "bMovieIsOpen" },
    m_dialog_state{ "DialogState" }, m_dialog_type{ "dialogType" }, m_system_param{ "SystemParam" }, m_key_config{ "KeyConfigType" },
    m_judging{ "bJudge" }, m_command{ "CommandType" }, m_title_menu{ "TitleMenu" }, m_enable_input{ "bEnableInput" },
    m_judge_info{ "JudgeInfo" }, m_request{ "Request" }, m_buttons{ "Buttons" }, m_input_info{ "InputInfo" }, m_enable_judge{ "bEnableJudge" },
    m_paused{ "bPause" }, m_shown{ "bShow" }, m_succeeded{ "bSuccess" },
    m_player_owner{ "PlayerOwner" }, m_player_pawn{ "BCPlayerPawn" }, m_generated_events{ "GeneratedEvents" }, m_enabled{ "bEnabled" },
    m_trigger_count{ "TriggerCount" }, m_max_trigger_count{ "MaxTriggerCount" }, m_input_buttons{ "InputButtons" };
std::atomic<uint32_t> g_push_button_class{ 0 };

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
    const uint32_t player = hud ? m_player_owner.object(hud) : 0;
    const uint32_t pawn = player ? m_player_pawn.object(player) : 0;
    const uint32_t events = pawn ? m_generated_events.at(pawn, vm_read32(pawn + k_object_class)) : 0;
    for (uint32_t i = 0, n = events ? vm_read32(events + 4) : 0; i < n; i++) {
        const uint32_t event = vm_read32(vm_read32(events) + i * 4);
        if (!event || !push_button_event(event) || !m_enabled.flag(event)) continue;
        const uint32_t limit = m_max_trigger_count.object(event);
        if (limit && m_trigger_count.object(event) >= limit) continue;
        const uint32_t buttons = m_input_buttons.at(event, vm_read32(event + k_object_class));
        for (uint32_t b = 0, count = buttons ? vm_read32(buttons + 4) : 0; b < count; b++)
            if (vm_read8(vm_read32(buttons) + b) == k_start_button) return true;
    }
    return false;
}

struct Elements {
    uint32_t data = 0, count = 0, stride = 0, type = 0;
};

Elements elements(Member& array, uint32_t base, uint32_t owner)
{
    const uint32_t at = array.at(base, owner);
    if (!at) return {};
    const uint32_t inner = array.detail.load(std::memory_order_relaxed);
    return { vm_read32(at), vm_read32(at + 4), vm_read32(inner + k_property_element), vm_read32(inner + k_property_detail) };
}

bool bit(Member& member, uint32_t base, uint32_t owner)
{
    const uint32_t at = member.at(base, owner);
    return at && (vm_read32(at) & member.detail.load(std::memory_order_relaxed));
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
        const uint32_t request = m_request.at(judge, judges.type);
        if (!request || !bit(m_enable_judge, judge, judges.type) || bit(m_paused, judge, judges.type)) continue;
        const Elements buttons = elements(m_buttons, request, m_request.detail.load(std::memory_order_relaxed));
        const Elements inputs = elements(m_input_info, judge, judges.type);
        for (uint32_t b = 0; b < buttons.count && b < inputs.count; b++) {
            const uint32_t input = inputs.data + b * inputs.stride;
            if (bit(m_succeeded, input, inputs.type)) continue;
            const uint8_t command = vm_read8(buttons.data + b * buttons.stride);
            if (command == k_start_command) lit |= k_start_judged;
            else if (bit(m_shown, input, inputs.type)) lit |= stick_bits(command);
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
    const uint32_t system = m_system_param.at(save, vm_read32(save + k_object_class));
    const uint32_t key = system ? m_key_config.at(system, m_system_param.detail.load(std::memory_order_relaxed)) : 0;
    const uint32_t type = key ? vm_read8(key) : 0;
    return type < k_key_config_types ? (int)type : 0;
}

int ui_context()
{
    const uint32_t scenes = live(g_scene_manager);
    if (!scenes) return 0;
    if (m_current_scene.object(scenes)) return PAD_CONTEXT_UI;
    const uint32_t pause = m_pause_menu.object(scenes);
    if (pause && m_movie_open.flag(pause)) return PAD_CONTEXT_UI;
    const uint32_t title = m_title_menu.object(scenes);
    if (title && m_movie_open.flag(title) && m_enable_input.flag(title)) return PAD_CONTEXT_UI;
    const uint32_t common = m_common.object(scenes);
    if (common && m_dialog_state.byte(common) != k_dialog_idle && m_dialog_type.byte(common) != k_dialog_passive) return PAD_CONTEXT_UI;
    const uint32_t prompt = live(g_prompt);
    if (prompt && m_movie_open.flag(prompt)) return PAD_CONTEXT_UI;
    return (g_stick_lit.load(std::memory_order_relaxed) & k_start_judged) || waits_for_start(live(g_hud)) ? PAD_CONTEXT_START : 0;
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
    else if (!std::strncmp(op, "Set", 3)) set(std::strtoul(op + 3, nullptr, 10), (uint32_t)first);
    else if (!std::strcmp(op, "Default")) reset(page);
    else if (!std::strcmp(op, "Decide")) reply_number(ctx, decide(row));
    else if (!std::strcmp(op, "Capture")) reply_number(ctx, capture(first != 0));
    else if (!std::strcmp(op, "Prompt")) first ? remember(g_prompt, movie_player(ctx)) : g_prompt.store(0, std::memory_order_relaxed);
    else if (!std::strcmp(op, "StartText")) reply(ctx, 0, start_text(text_argument(ctx, 0)).c_str());
    else if (!std::strcmp(op, "Diagram")) g_controller_screen.store(first != 0, std::memory_order_relaxed);
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

std::atomic<uint64_t> g_pc_options_hook_mask{ 0 };

void pc_options_boot(const std::filesystem::path& dir)
{
    std::lock_guard<std::mutex> hold(g_settings_lock);
    g_settings_path = dir / "asura_settings.ini";
    load();
    rsx_vk_on_display_toggle(display_toggled);
    rsx_vk_on_display_modes(display_modes_changed);
    pad_set_menu_probe(ui_context);
    pad_set_layout_probe(face_layout);
    pad_set_mouse_route_probe(mouse_route);
    for (uint32_t r = 0; r < k_rows; r++) sync(r);
}

void pc_options_link(uint32_t function, const std::string& path)
{
    for (size_t i = 0; i < std::size(k_hooks); i++)
        if (path == k_hooks[i].first) {
            g_hooks[i].store((uint64_t)function << 32 | k_hooks[i].second, std::memory_order_relaxed);
            g_pc_options_hook_mask.fetch_or(hook_bit(function), std::memory_order_relaxed);
        }
}

uint32_t pc_options_call_slow(uint32_t self, uint32_t function)
{
    if (!function || !(g_pc_options_hook_mask.load(std::memory_order_relaxed) & hook_bit(function))) return 0;
    for (const std::atomic<uint64_t>& slot : g_hooks) {
        const uint64_t hook = slot.load(std::memory_order_relaxed);
        if ((uint32_t)(hook >> 32) != function) continue;
        if ((uint32_t)hook == hook_judge_update || (uint32_t)hook == hook_judge_destroyed) {
            g_stick_lit.store((uint32_t)hook == hook_judge_update ? stick_lit(self) : 0, std::memory_order_relaxed);
            return 0;
        }
        if ((uint32_t)hook == hook_scene_manager && m_player_owner.object(self)) remember(g_hud, self);
        if ((uint32_t)hook != hook_stick_rotation) return (uint32_t)hook;
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
    const uint64_t spin_ms = g_spin_ms.load(std::memory_order_relaxed);
    if (spin_ms) {
        const uint64_t now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now_ms - spin_ms < 150u) {
            constexpr uint32_t k_cw[4]  = { 1u, 8u, 4u, 2u };
            constexpr uint32_t k_ccw[4] = { 1u, 2u, 4u, 8u };
            const uint32_t phase = (uint32_t)((now_ms / 140u) & 3u);
            const uint8_t cmd = g_spin_cmd.load(std::memory_order_relaxed);
            lit = (lit & ~0xFu) | ((cmd & 1u) ? k_ccw[phase] : k_cw[phase]);
        }
    }
    return lit;
}

void pc_options_mouse_scale(float* yaw, float* pitch)
{
    *yaw = g_mouse_units.load(std::memory_order_relaxed);
    *pitch = g_invert_mouse.load(std::memory_order_relaxed) ? -*yaw : *yaw;
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
