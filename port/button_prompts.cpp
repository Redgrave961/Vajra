#include "button_prompts.h"
#include "prompt_art.h"
#include "pc_options.h"
#include "vk/rsx_vk.h"
#include "../ps3recomp/libs/input/pad_bindings.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t k_dxt1 = 0x86, k_dxt5 = 0x88, k_kbm = 3, k_menu = 4, k_lit_shift = 3, k_lit = 0xFFu << k_lit_shift, k_version_shift = 11;
constexpr uint32_t k_qte_size = 128;

enum glyph : uint8_t { g_lstick = PAD_ACTION_COUNT, g_rstick, g_dpad };

enum class ring : uint8_t { blank, squircle };

struct slot {
    uint8_t glyph;
    prompt_skin skin;
    prompt_frame frame;
};

struct cell {
    uint8_t col, row;
    slot s;
};

struct qte {
    uint64_t hash;
    slot s;
    uint32_t width = k_qte_size, height = k_qte_size;
};

struct page_mark {
    uint8_t glyph;
    prompt_mark m;
};

struct page {
    uint64_t hash;
    page_mark marks[7];
};

constexpr page_mark face(uint8_t glyph, float x, float y, float d)
{
    return { glyph, { x, y, d, d, true } };
}

constexpr page_mark plate(uint8_t glyph, float x, float y, float h)
{
    return { glyph, { x, y, 22, h, false } };
}

constexpr page burst_page(uint64_t hash, float r2_x, float r2_y, float fire_x, float fire_y)
{
    return { hash, { plate(PAD_R2, r2_x, r2_y, 22), face(PAD_TRIANGLE, fire_x, fire_y, 18), face(PAD_TRIANGLE, 678.5f, 421.5f, 24),
                     face(PAD_CIRCLE, 678.5f, 460.5f, 24), face(PAD_CROSS, 678.5f, 499.5f, 24), face(PAD_SQUARE, 678.5f, 539.5f, 24) } };
}

constexpr page counter_page(uint64_t hash, float r2_x, float r2_y, float r1_x, float r1_y)
{
    return { hash, { plate(PAD_R2, r2_x, r2_y, 22), plate(PAD_R1, r1_x, r1_y, 14), face(PAD_TRIANGLE, 765.5f, 213, 26), face(PAD_CIRCLE, 186.5f, 424.5f, 24),
                     face(PAD_CROSS, 186.5f, 463.5f, 24), face(PAD_TRIANGLE, 186.5f, 502.5f, 24), face(PAD_SQUARE, 186.5f, 542.5f, 24) } };
}

const page k_pages[] = {
    burst_page(0xECCC222F79CFF79Bull, 261.5f, 320.5f, 1017.5f, 305.5f),   counter_page(0xD75B78AD61818E85ull, 261.5f, 320.5f, 738.5f, 569.5f),
    burst_page(0xF2FFB065B7DBDD95ull, 215.5f, 334.5f, 865.5f, 338.5f),    counter_page(0x79DB313DC8223BF9ull, 215.5f, 334.5f, 800.5f, 568.5f),
    burst_page(0x02B6E2A8CA02D871ull, 261.5f, 334.5f, 762.5f, 339.5f),    counter_page(0x29A5FC8B44E32704ull, 261.5f, 334.5f, 841.5f, 568.5f),
    burst_page(0x58987B86D796C316ull, 228.5f, 334.5f, 863.5f, 320.5f),    counter_page(0xA162E5644E2B39EFull, 228.5f, 334.5f, 737.5f, 568.5f),
    burst_page(0xA174E7A008137539ull, 230.5f, 334.5f, 866.5f, 323.5f),    counter_page(0x8D8CA8A460F8D75Full, 230.5f, 334.5f, 791.5f, 568.5f),
    burst_page(0x5587C292AB8332D7ull, 371.5f, 334.5f, 666.5f, 338.5f),    counter_page(0x67348260C3474D7Aull, 371.5f, 334.5f, 716.5f, 568.5f),
};

constexpr uint32_t k_page_width = 1280, k_page_height = 720;

struct atlas {
    uint64_t hash;
    uint32_t width, height, cell_w, cell_h;
    const cell* cells;
    size_t count;
    bool faces, glow;
};

struct cache {
    uint32_t variant = ~0u;
    std::vector<uint8_t> data;
};

struct qte_cache {
    uint32_t base_variant = ~0u;
    std::unordered_map<uint32_t, std::vector<uint8_t>> by_lit;
};

constexpr prompt_frame h_face{ 16, 16, 27, 31, 27 }, h_wide{ 16, 16, 30, 31, 20 }, h_square{ 16, 16, 27, 31, 27 }, h_stick{ 16, 16, 32, 32, 32 };

const cell k_cells[] = {
    { 0, 0, { PAD_CROSS, prompt_skin::hud, h_face } },    { 1, 0, { PAD_CIRCLE, prompt_skin::hud, h_face } },
    { 2, 0, { PAD_SQUARE, prompt_skin::hud, h_face } },   { 3, 0, { PAD_TRIANGLE, prompt_skin::hud, h_face } },
    { 4, 0, { PAD_L1, prompt_skin::hud, h_wide } },       { 5, 0, { PAD_R1, prompt_skin::hud, h_wide } },
    { 6, 0, { PAD_L2, prompt_skin::hud, h_square } },     { 7, 0, { PAD_R2, prompt_skin::hud, h_square } },
    { 0, 1, { g_lstick, prompt_skin::hud, h_stick } },    { 1, 1, { g_rstick, prompt_skin::hud, h_stick } },
    { 2, 1, { g_dpad, prompt_skin::hud, h_stick } },      { 3, 1, { PAD_START, prompt_skin::hud, h_wide } },
    { 4, 1, { PAD_SELECT, prompt_skin::hud, h_wide } },
};

const cell k_plate_cells[] = { { 0, 0, { PAD_L1, prompt_skin::hud, { 32, 16, 30, 60, 26 } } } };

const atlas k_atlases[] = {
    { 0xD7A5C10A385CCEE8ull, 256, 128, 32, 32, k_cells, std::size(k_cells), true, false },
    { 0x3582050752B6F32Bull, 128, 64, 64, 32, k_plate_cells, std::size(k_plate_cells), false, true },
};

constexpr prompt_frame q_tilt{ 64, 60, 66, 104, 70 }, q_tilt_down{ 64, 64, 66, 104, 70 }, q_flat{ 64, 64, 62, 100, 62 }, q_stick{ 64, 64, 90, 90, 90 },
    q_wide{ 63.5f, 61.5f, 62, 100, 36 }, q_square{ 63.5f, 60.5f, 56, 100, 56 }, q_big{ 63.5f, 61.5f, 96, 104, 96 }, q_label{ 64, 65, 0, 100, 34 };

const qte k_qtes[] = {
    { 0x925E7C78E2752602ull, { PAD_CROSS, prompt_skin::tilt_dark, q_tilt } },
    { 0xA54A3C0AACBFDF53ull, { PAD_CROSS, prompt_skin::tilt_dark, q_tilt } },
    { 0xAA4385C5B5086DE5ull, { PAD_CIRCLE, prompt_skin::tilt_dark, q_tilt } },
    { 0x637DA7ADE577810Dull, { PAD_CIRCLE, prompt_skin::tilt_dark, q_tilt } },
    { 0xB29097785E9E6E56ull, { PAD_SQUARE, prompt_skin::tilt_dark, q_tilt } },
    { 0x5D6E8FA6FEF2D629ull, { PAD_SQUARE, prompt_skin::tilt_dark, q_tilt } },
    { 0x2B6FD1D2A37EF61Eull, { PAD_TRIANGLE, prompt_skin::tilt_dark, q_tilt } },
    { 0x48E4E64745471B39ull, { PAD_TRIANGLE, prompt_skin::tilt_dark, q_tilt } },
    { 0x2F50F6DD3EC21AC2ull, { PAD_CROSS, prompt_skin::tilt_fire, q_tilt_down } },
    { 0x01F621CA214429BFull, { PAD_CIRCLE, prompt_skin::tilt_fire, q_tilt_down } },
    { 0x311C66872D6EE6FFull, { PAD_SQUARE, prompt_skin::tilt_fire, q_tilt_down } },
    { 0x08B3EC5B7DB75339ull, { PAD_TRIANGLE, prompt_skin::tilt_fire, q_tilt_down } },
    { 0xD03E5A8390C20DCCull, { PAD_CROSS, prompt_skin::flat_dark, q_flat } },
    { 0x878DB20425939BACull, { PAD_CIRCLE, prompt_skin::flat_dark, q_flat } },
    { 0xD24F1E8B8DB4DCCEull, { PAD_SQUARE, prompt_skin::flat_dark, q_flat } },
    { 0xF31656F7C6569569ull, { PAD_TRIANGLE, prompt_skin::flat_dark, q_flat } },
    { 0xBCDA062DBF8FF731ull, { PAD_CROSS, prompt_skin::flat_fire, q_flat } },
    { 0xEEE6490B9FD7A25Dull, { PAD_CIRCLE, prompt_skin::flat_fire, q_flat } },
    { 0x7111685213526858ull, { PAD_SQUARE, prompt_skin::flat_fire, q_flat } },
    { 0xF568CC70DB9D34F7ull, { PAD_TRIANGLE, prompt_skin::flat_fire, q_flat } },
    { 0x79A2F22FEFF31BC6ull, { g_lstick, prompt_skin::cap_dark, q_stick } },
    { 0x5C7008462A3541E9ull, { g_rstick, prompt_skin::cap_dark, q_stick } },
    { 0xC927E5E45EE086D8ull, { g_lstick, prompt_skin::cap_fire, q_stick } },
    { 0x005851BE455D7AA0ull, { g_rstick, prompt_skin::cap_fire, q_stick } },
    { 0x18EFA69ACCE92554ull, { PAD_L1, prompt_skin::cap_dark, q_wide } },
    { 0x592A25EA5C0560A8ull, { PAD_R1, prompt_skin::cap_dark, q_wide } },
    { 0x47C946FF9DC5C061ull, { PAD_L1, prompt_skin::cap_fire, q_wide } },
    { 0x6AE2D6B4CAC82FA5ull, { PAD_R1, prompt_skin::cap_fire, q_wide } },
    { 0x7CBFE009A169B892ull, { PAD_L2, prompt_skin::cap_dark, q_square } },
    { 0x509A39EE2BA2AD3Dull, { PAD_R2, prompt_skin::cap_dark, q_square } },
    { 0xFD0A369D166ABD32ull, { PAD_L2, prompt_skin::cap_fire, q_square } },
    { 0xA3734694E1F42523ull, { PAD_R2, prompt_skin::cap_fire, q_square } },
    { 0x7047FB8057BF23B1ull, { PAD_R2, prompt_skin::cap_dark, q_big } },
    { 0x4601F8F7764E9A2Aull, { PAD_R2, prompt_skin::cap_fire, q_big } },
    { 0x859C7C7F11BCADF8ull, { PAD_R2, prompt_skin::label, q_label } },
    { 0x0615EF6EF84C2702ull, { PAD_TRIANGLE, prompt_skin::sync, q_flat } },
    { 0x3CC672A4501A8E2Dull, { PAD_L2, prompt_skin::gauge, { 32, 16, 0, 56, 20 } }, 64, 32 },
};

struct ring_texture {
    uint64_t hash;
    uint32_t bytes;
    ring op;
};

const ring_texture k_rings[] = {
    { 0xC4B16F12A1AA30A8ull, 16384, ring::blank }, { 0x5AD6D93E7CD81FA4ull, 512, ring::blank },
    { 0x75929EC3FB67FE41ull, 128, ring::blank },   { 0xBE57FE83A25CC8ECull, 4096, ring::blank },
    { 0xA86C3339314E8A95ull, 16384, ring::blank }, { 0x09411377F5C58CF5ull, 16384, ring::blank },
    { 0xF7C36A2107A7036Eull, 16384, ring::blank }, { 0xB4BA7F930E7C6CCDull, 16384, ring::blank },
    { 0x2A6A879E6AC506C9ull, 16384, ring::blank }, { 0xB0AFFB72FD2DFAA1ull, 65536, ring::blank },
    { 0x30118477881F80F4ull, 65536, ring::blank }, { 0x2A1F509DCC49054Dull, 65536, ring::blank },
};

constexpr char k_letters[2][4] = { { 'A', 'B', 'X', 'Y' }, { 'B', 'A', 'Y', 'X' } };
constexpr uint8_t k_nintendo_cells[4] = { 1, 0, 3, 2 };
constexpr const char* k_shoulders[4] = { "LB", "RB", "LT", "RT" };

cache g_atlas[std::size(k_atlases)];
qte_cache g_qte[std::size(k_qtes)];
cache g_page[std::size(k_pages)];
std::vector<uint8_t> g_ring[std::size(k_rings)];

uint64_t hash(const uint8_t* p, uint64_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint64_t i = 0; i < n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
    }
    return h;
}

uint32_t letter_tint(char c)
{
    return c == 'A' ? 0x5CC83C : c == 'B' ? 0xE03C32 : c == 'X' ? 0x3C78E6 : 0xF0C020;
}

bool bound(int action, uint32_t variant, prompt_glyph& g)
{
    const int input = pad_prompt_input(action, (variant & k_menu) != 0);
    if (!input) return false;
    if (input == PAD_INPUT_WHEEL_UP || input == PAD_INPUT_WHEEL_DOWN) {
        g.kind = input == PAD_INPUT_WHEEL_UP ? prompt_kind::wheel_up : prompt_kind::wheel_down;
    } else if (input >= PAD_INPUT_MOUSE(0) && input < PAD_INPUT_WHEEL_UP) {
        g.kind = prompt_kind::mouse;
        g.button = input - PAD_INPUT_MOUSE(0);
    } else {
        const char* name = pad_input_name(input);
        if (!name || !*name) return false;
        g.label[0] = name;
    }
    return true;
}

bool resolve(uint8_t glyph, uint32_t variant, prompt_glyph& g)
{
    const uint32_t mode = variant & 3;
    if (mode != k_kbm) {
        if (glyph <= PAD_TRIANGLE) {
            g.kind = prompt_kind::face;
            g.label[0] = k_letters[mode - 1][glyph];
            g.tint = letter_tint(g.label[0][0]);
            return true;
        }
        if (glyph > PAD_R2) return false;
        g.label[0] = k_shoulders[glyph - PAD_L1];
        return true;
    }
    if (glyph == g_rstick) {
        g.kind = prompt_kind::mouse_move;
        g.lit = (uint8_t)(variant >> (k_lit_shift + 4) & 15);
        return true;
    }
    if (glyph == g_lstick || glyph == g_dpad) {
        const int first = glyph == g_lstick ? PAD_MOVE_UP : PAD_UP;
        const int order[4] = { first, first + 2, first + 1, first + 3 };
        g.kind = prompt_kind::keys_wasd;
        g.lit = glyph == g_lstick ? (uint8_t)(variant >> k_lit_shift & 15) : 0;
        for (int i = 0; i < 4; i++) {
            prompt_glyph one;
            if (bound(order[i], variant, one) && one.kind == prompt_kind::key) g.label[i] = one.label[0];
        }
        return true;
    }
    return bound(glyph, variant, g);
}

bool draw(const slot& s, uint32_t variant, uint32_t width, uint32_t height, uint8_t* out, size_t pitch, uint8_t* glow = nullptr)
{
    prompt_glyph g;
    if (!resolve(s.glyph, variant, g)) return false;
    const int size = (int)std::max(width, height);
    std::vector<uint8_t> rgba((size_t)size * size * 4);
    if (!prompt_render(s.skin, s.frame, g, size, rgba.data())) return false;
    prompt_bc3(rgba.data(), (int)width, (int)height, out, pitch);
    if (glow) {
        prompt_glow(rgba.data(), size);
        prompt_bc3(rgba.data(), (int)width, (int)height, glow, pitch);
    }
    return true;
}

void patch_atlas(size_t index, uint32_t variant, const uint8_t* guest, uint8_t* staging)
{
    if (variant == 0) return;
    const atlas& a = k_atlases[index];
    const uint32_t pitch = a.width / 4 * 16, half = pitch * a.height / 8, cell_rows = a.cell_h / 4, cell_bytes = a.cell_w / 4 * 16;
    if ((variant & 3) != k_kbm) {
        std::memcpy(staging + half, guest, half);
        if (variant == 2 && a.faces)
            for (uint32_t c = 0; c < 4; c++)
                for (uint32_t r = 0; r < cell_rows; r++)
                    std::memcpy(staging + half + r * pitch + c * cell_bytes, guest + r * pitch + k_nintendo_cells[c] * cell_bytes, cell_bytes);
        return;
    }
    const uint32_t neutral = variant & ~k_lit;
    cache& c = g_atlas[index];
    if (c.variant != neutral) {
        c.variant = neutral;
        c.data.assign(guest + half, guest + 2 * half);
        for (size_t i = 0; i < a.count; i++) {
            uint8_t* const at = c.data.data() + a.cells[i].row * cell_rows * pitch + a.cells[i].col * cell_bytes;
            draw(a.cells[i].s, neutral, a.cell_w, a.cell_h, at, pitch, a.glow ? at + cell_bytes : nullptr);
        }
    }
    std::memcpy(staging + half, c.data.data(), half);
}

void patch_page(size_t index, uint32_t variant, const uint8_t* guest, uint64_t bytes, uint8_t* staging)
{
    if (variant == 0) return;
    const uint32_t neutral = variant & ~k_lit;
    cache& c = g_page[index];
    if (c.variant != neutral) {
        c.variant = neutral;
        c.data.assign(guest, guest + bytes);
        for (const page_mark& mark : k_pages[index].marks) {
            prompt_glyph g;
            if (mark.m.w > 0 && resolve(mark.glyph, neutral, g)) prompt_mark_dxt1(c.data.data(), k_page_width, k_page_height, mark.m, prompt_skin::hud, g);
        }
    }
    std::memcpy(staging, c.data.data(), bytes);
}
void patch_qte(size_t index, uint32_t variant, uint8_t* staging)
{
    if (variant == 0) return;
    const qte& q = k_qtes[index];
    qte_cache& c = g_qte[index];
    const uint32_t base = variant & ~k_lit;
    const uint32_t key = q.s.glyph == g_lstick || q.s.glyph == g_rstick ? variant : base;
    if (c.base_variant != base) {
        c.base_variant = base;
        c.by_lit.clear();
    }
    auto it = c.by_lit.find(key);
    if (it == c.by_lit.end()) {
        std::vector<uint8_t> buf((size_t)q.width * q.height);
        if (!draw(q.s, key, q.width, q.height, buf.data(), q.width / 4 * 16)) buf.clear();
        it = c.by_lit.emplace(key, std::move(buf)).first;
    }
    if (!it->second.empty()) std::memcpy(staging, it->second.data(), it->second.size());
}

uint32_t variant()
{
    if (!pad_kbm_last() || pc_options_controller_screen()) return (uint32_t)pad_gamepad_style();
    return pad_bindings_version() << k_version_shift | pc_options_stick_lit() << k_lit_shift | (pad_menu_active() ? k_menu : 0) | k_kbm;
}

void patch_ring(size_t index, uint32_t variant, uint32_t width, uint32_t gcm_format, const uint8_t* guest, uint8_t* staging)
{
    if ((variant & 3) != k_kbm) return;
    std::vector<uint8_t>& data = g_ring[index];
    const uint32_t bytes = k_rings[index].bytes;
    if (data.empty()) {
        data.assign(bytes, 0);
        if (k_rings[index].op == ring::squircle)
            prompt_squircle(guest, (int)width, data.data());
        else if (gcm_format == k_dxt1)
            for (uint32_t i = 4; i < bytes; i += 8) std::memset(data.data() + i, 0xFF, 4);
    }
    std::memcpy(staging, data.data(), bytes);
}

int apply(uint32_t v, uint32_t gcm_format, uint32_t width, uint32_t height, const uint8_t* guest, uint64_t guest_size, uint8_t* staging)
{
    if (gcm_format != k_dxt5 && gcm_format != k_dxt1) return 0;
    const uint64_t bytes = (uint64_t)width * height >> (gcm_format == k_dxt1);
    if (guest_size < bytes) return 0;
    const bool dxt5 = gcm_format == k_dxt5;
    uint64_t h = 0;
    auto hashed = [&] { return h ? h : h = hash(guest, bytes); };
    for (size_t i = 0; i < std::size(k_atlases); i++)
        if (dxt5 && width == k_atlases[i].width && height == k_atlases[i].height && hashed() == k_atlases[i].hash) {
            patch_atlas(i, v, guest, staging);
            return 1;
        }
    for (size_t i = 0; i < std::size(k_qtes); i++)
        if (dxt5 && width == k_qtes[i].width && height == k_qtes[i].height && hashed() == k_qtes[i].hash) {
            patch_qte(i, v, staging);
            return (v & 3) == k_kbm && k_qtes[i].s.glyph == g_lstick ? 3 : 1;
        }
    for (size_t i = 0; i < std::size(k_pages); i++)
        if (!dxt5 && width == k_page_width && height == k_page_height && hashed() == k_pages[i].hash) {
            patch_page(i, v, guest, bytes, staging);
            return 1;
        }
    if (bytes == 128 || bytes == 512 || bytes == 4096 || bytes == 16384 || bytes == 65536) {
        for (size_t i = 0; i < std::size(k_rings); i++)
            if (k_rings[i].bytes == bytes && (k_rings[i].op == ring::blank || (dxt5 && width == height)) && hashed() == k_rings[i].hash) {
                patch_ring(i, v, width, gcm_format, guest, staging);
                return (v & 3) == k_kbm && (bytes == 16384 || bytes == 65536) ? 2 : 1;
            }
    }
    return 0;
}

constexpr rsx_vk_texture_patch k_patch{ variant, apply };

}

void use_button_prompts()
{
    rsx_vk_set_texture_patch(&k_patch);
}
