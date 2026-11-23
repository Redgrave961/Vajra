#include "setup_window.h"

#include "firmware.h"
#include "font.h"
#include "launcher_art.h"
#include "../ps3recomp/libs/codec/stb_image.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

namespace {

constexpr float k_w = 940, k_h = 690, k_pad = 40;
constexpr float k_hero_h = 188;
constexpr float k_panel_y = 204, k_row_h = 74;
constexpr float k_pi = 3.14159265359f;

struct rgba {
    uint8_t r, g, b, a = 255;
};

// Volcanic obsidian -> crimson palette with refined warm gold & ivory typography
constexpr rgba c_bg_top{ 10, 7, 8 }, c_bg_bot{ 26, 10, 10 };
constexpr rgba c_panel_top{ 20, 15, 16, 232 }, c_panel_bot{ 15, 11, 12, 244 };
constexpr rgba c_panel_border{ 120, 88, 62, 95 };
constexpr rgba c_row_hover{ 48, 32, 30, 105 };
constexpr rgba c_divider{ 220, 178, 118, 28 };

constexpr rgba c_text{ 248, 244, 238 }, c_sub{ 202, 188, 178 }, c_dim{ 138, 124, 116 };
constexpr rgba c_gold{ 242, 198, 110 }, c_gold_bright{ 255, 232, 174 };

constexpr rgba c_play_top{ 214, 50, 34 }, c_play_bot{ 156, 24, 20 };
constexpr rgba c_play_hot_top{ 236, 68, 42 }, c_play_hot_bot{ 182, 32, 24 };
constexpr rgba c_play_down{ 132, 20, 18 };

constexpr rgba c_btn_fill{ 44, 34, 34, 218 }, c_btn_border{ 178, 146, 116, 85 };
constexpr rgba c_btn_hot{ 68, 48, 44, 242 }, c_btn_hot_border{ 242, 198, 110, 195 };
constexpr rgba c_btn_down{ 30, 22, 22, 245 };

constexpr rgba c_gold_btn_fill{ 68, 48, 26, 230 }, c_gold_btn_hot{ 96, 66, 32, 245 };
constexpr rgba c_disabled{ 34, 26, 26, 210 }, c_disabled_border{ 86, 68, 64, 70 };

constexpr rgba c_ok{ 102, 212, 124 }, c_warn{ 245, 186, 74 }, c_bad{ 240, 96, 82 }, c_missing{ 136, 120, 112 };

enum action { a_game_folder, a_game_iso, a_update_pkg, a_dlc_add, a_dlc_clear, a_rap_add, a_download, a_play };

struct face {
    SDL_Texture* tex = nullptr;
    std::array<stbtt_packedchar, 96 + 96 + 3> chars{};
    float size = 0, ascent = 0;
    int aw = 0, ah = 0;
};

int glyph_index(uint32_t cp)
{
    if (cp >= 32 && cp < 128) return (int)cp - 32;
    if (cp >= 160 && cp < 256) return 96 + (int)cp - 160;
    if (cp == 0x2013) return 192;
    if (cp == 0x2026) return 193;
    if (cp == 0x00B7) return 194;
    return '?' - 32;
}

enum class btn_style { secondary, gold, play };

struct button {
    SDL_FRect r;
    std::string label;
    action id;
    bool enabled;
    btn_style style;
};

struct ember {
    float x, y, vy, size, phase, speed, alpha;
};

struct firmware_job {
    std::thread worker;
    std::atomic<int> phase{ 0 };
    std::atomic<float> progress{ 0 };
    std::mutex lock;
    std::string error;
};

class setup_ui {
public:
    setup_ui(const std::filesystem::path& exe, game_paths& paths, game_check check) : exe(exe), paths(paths), check(std::move(check))
    {
        uint32_t seed = 0x9E3779B9u;
        auto rng = [&] {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            return (float)(seed & 0xFFFFu) / 65535.f;
        };
        for (ember& em : embers) {
            em.x = rng() * k_w;
            em.y = rng() * k_h;
            em.vy = 13.f + rng() * 24.f;
            em.size = 1.5f + rng() * 2.4f;
            em.phase = rng() * k_pi * 2.f;
            em.speed = 0.7f + rng() * 1.2f;
            em.alpha = 0.20f + rng() * 0.48f;
        }
    }

    bool open(const std::filesystem::path& icon)
    {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return false;
        if (!load_font(regular, font_weight::regular) || !load_font(semibold, font_weight::semibold) || !load_font(black, font_weight::black))
            return false;
        const float ds = std::max(1.f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
        window = SDL_CreateWindow("Vajra", (int)(k_w * ds), (int)(k_h * ds), SDL_WINDOW_HIGH_PIXEL_DENSITY);
        renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
        if (!renderer) return false;
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        if (SDL_Surface* image = icon.empty() ? nullptr : SDL_LoadPNG(icon.string().c_str())) {
            SDL_SetWindowIcon(window, image);
            SDL_DestroySurface(image);
        }

        int aw = 0, ah = 0, comp = 0;
        if (uint8_t* px = stbi_load_from_memory(k_launcher_art, (int)sizeof(k_launcher_art), &aw, &ah, &comp, 4)) {
            art_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, aw, ah);
            if (art_tex) {
                SDL_UpdateTexture(art_tex, nullptr, px, aw * 4);
                SDL_SetTextureBlendMode(art_tex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(art_tex, SDL_SCALEMODE_LINEAR);
                art_w = aw;
                art_h = ah;
            }
            stbi_image_free(px);
        }

        SDL_SetRenderVSync(renderer, 1);
        int pw = 0, ph = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &pw, &ph);
        s = (float)pw / k_w;
        bake(hero_title, black, 46);
        bake(hero_sub, semibold, 14.5f);
        bake(play_font, black, 20);
        bake(heading, semibold, 18);
        bake(body, regular, 15.5f);
        bake(btn_font, semibold, 15);
        t0 = std::chrono::steady_clock::now();
        last_t = t0;
        return true;
    }

    ~setup_ui()
    {
        if (job.worker.joinable()) job.worker.join();
        if (art_tex) SDL_DestroyTexture(art_tex);
        for (face* f : { &hero_title, &hero_sub, &play_font, &heading, &body, &btn_font })
            if (f->tex) SDL_DestroyTexture(f->tex);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    bool run()
    {
        for (;;) {
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, 16)) {
                do {
                    if (!handle(e)) return false;
                } while (SDL_PollEvent(&e));
            }
            apply_picks();
            finish_job();
            if (play) return true;
            draw();
        }
    }

private:
    void draw()
    {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(0.05f, std::chrono::duration<float>(now - last_t).count());
        const float t = std::chrono::duration<float>(now - t0).count();
        last_t = now;

        // 1. Volcanic obsidian -> crimson base
        grad_rect({ 0, 0, k_w, k_h }, c_bg_top, c_bg_bot);

        // 2. Full-bleed Vajra Asura hero artwork fading smoothly into obsidian-crimson
        if (art_tex && art_w > 0 && art_h > 0) {
            const float target_h = 340.f;
            const float src_aspect = (float)art_w / (float)art_h;
            const float dst_aspect = k_w / target_h;
            SDL_FRect src{ 0, 0, (float)art_w, (float)art_h };
            if (src_aspect < dst_aspect) {
                src.h = (float)art_w / dst_aspect;
                src.y = ((float)art_h - src.h) * 0.20f;
            } else {
                src.w = (float)art_h * dst_aspect;
                src.x = ((float)art_w - src.w) * 0.5f;
            }
            const SDL_FRect dst{ 0, 0, k_w * s, target_h * s };
            SDL_SetTextureAlphaMod(art_tex, 215);
            SDL_RenderTexture(renderer, art_tex, &src, &dst);

            grad_rect({ 0, 0, k_w, 84 }, { 10, 7, 8, 145 }, { 10, 7, 8, 10 });
            grad_rect({ 0, 120, k_w, 222 }, { 13, 8, 9, 15 }, { 16, 8, 9, 255 });
            horiz_grad({ 0, 0, 480, k_hero_h + 40 }, { 10, 7, 8, 190 }, { 10, 7, 8, 0 });
        }

        // 3. Subtle volcanic bottom furnace glow & drifting embers
        radial_glow(k_w * 0.5f, k_h * 1.06f, k_w * 0.72f, k_h * 0.48f, { 176, 44, 18, 66 });
        for (ember& em : embers) {
            em.y -= em.vy * dt;
            if (em.y < -10.f) {
                em.y = k_h + 10.f;
                em.x = std::fmod(em.x + 173.f, k_w);
            }
            const float ex = em.x + std::sin(t * em.speed + em.phase) * 14.f;
            const float fade = std::clamp(std::min(em.y / 130.f, (k_h - em.y) / 80.f), 0.f, 1.f);
            const uint8_t a = (uint8_t)(em.alpha * fade * 255.f);
            if (a > 3) {
                round_rect({ ex - em.size * 1.6f, em.y - em.size * 1.6f, em.size * 3.2f, em.size * 3.2f }, em.size * 1.6f,
                           { 235, 110, 32, (uint8_t)(a / 3) });
                round_rect({ ex - em.size * 0.5f, em.y - em.size * 0.5f, em.size, em.size }, em.size * 0.5f, { 255, 214, 126, a });
            }
        }

        layout();

        // 4. Clean Hero Title & Drop Hint
        text_shadow(hero_title, "VAJRA", k_pad, 42.f, c_gold_bright, 3.5f);
        text_shadow(hero_sub, "ASURA'S WRATH", k_pad + 2.f, 96.f, c_gold, 2.5f);
        text_shadow(body, "Select your game files below, or drag and drop them anywhere.", k_pad, k_hero_h - 36.f, c_sub);

        // 5. Unified Frosted Volcanic-Obsidian Setup Panel
        const float panel_h = 5.f * k_row_h + 16.f;
        const SDL_FRect pr{ k_pad, k_panel_y, k_w - 2 * k_pad, panel_h };
        round_rect(pr, 14.f, c_panel_border);
        round_rect_grad({ pr.x + 1, pr.y + 1, pr.w - 2, pr.h - 2 }, 13.f, c_panel_top, c_panel_bot);

        const char* names[] = { "Game", "Update 1.02", "DLC", "Licences", "Firmware" };
        const char* hints[] = {
            "BLES01227 disc folder or decrypted ISO",
            "Official 1.02 update .pkg",
            "Optional DLC .pkg packages",
            "Optional .rap keys for DLC",
            "PS3 audio codecs (automatic Sony download)",
        };
        const item_status* items[] = { &check.game, &check.update, &check.dlc, &check.licences, &check.firmware };
        for (int i = 0; i < 5; i++) draw_row(i, names[i], hints[i], *items[i]);

        // 6. Clean Modern Buttons + Play CTA
        for (size_t i = 0; i < buttons.size(); i++) draw_button(buttons[i], (int)i == focus, t);

        SDL_RenderPresent(renderer);
    }

    void bake(face& f, font& src, float size)
    {
        f.size = std::round(size * s);
        f.aw = 1024;
        f.ah = (int)std::max(256.f, f.size * 10);
        std::vector<uint8_t> alpha((size_t)f.aw * f.ah);
        stbtt_pack_context pc;
        stbtt_PackBegin(&pc, alpha.data(), f.aw, f.ah, 0, 1, nullptr);
        stbtt_PackSetOversampling(&pc, 2, 2);
        stbtt_pack_range ranges[3] = {};
        ranges[0] = { f.size, 32, nullptr, 96, f.chars.data(), 0, 0 };
        ranges[1] = { f.size, 160, nullptr, 96, f.chars.data() + 96, 0, 0 };
        int special[3] = { 0x2013, 0x2026, 0x00B7 };
        ranges[2] = { f.size, 0, special, 3, f.chars.data() + 192, 0, 0 };
        stbtt_PackFontRanges(&pc, src.ttf.data(), 0, ranges, 3);
        stbtt_PackEnd(&pc);
        int ascent = 0, descent = 0, gap = 0;
        stbtt_GetFontVMetrics(&src.info, &ascent, &descent, &gap);
        f.ascent = ascent * stbtt_ScaleForPixelHeight(&src.info, f.size);
        std::vector<uint8_t> px((size_t)f.aw * f.ah * 4, 255);
        for (size_t i = 0; i < alpha.size(); i++) px[i * 4 + 3] = alpha[i];
        f.tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, f.aw, f.ah);
        SDL_UpdateTexture(f.tex, nullptr, px.data(), f.aw * 4);
        SDL_SetTextureBlendMode(f.tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(f.tex, SDL_SCALEMODE_NEAREST);
    }

    float measure(const face& f, const std::string& str, float track = 0.f) const
    {
        float x = 0, y = 0;
        int n = 0;
        stbtt_aligned_quad q;
        for (size_t i = 0; i < str.size(); n++) {
            stbtt_GetPackedQuad(f.chars.data(), f.aw, f.ah, glyph_index(next_cp(str, i)), &x, &y, &q, 1);
            x += track * s;
        }
        return std::max(0.f, (x - (n > 0 ? track * s : 0.f)) / s);
    }

    std::string fit(const face& f, const std::string& str, float width) const
    {
        if (measure(f, str) <= width) return str;
        std::vector<size_t> starts;
        for (size_t i = 0; i < str.size(); next_cp(str, i)) starts.push_back(i);
        for (size_t cut = 1; cut < starts.size(); cut++) {
            const size_t head = (starts.size() - cut) / 2, tail = starts.size() - cut - head;
            const std::string t = str.substr(0, starts[head]) + "\xE2\x80\xA6" + str.substr(starts[starts.size() - tail]);
            if (measure(f, t) <= width) return t;
        }
        return "\xE2\x80\xA6";
    }

    void text(const face& f, const std::string& str, float x, float y, rgba c, float track = 0.f)
    {
        SDL_SetTextureColorMod(f.tex, c.r, c.g, c.b);
        SDL_SetTextureAlphaMod(f.tex, c.a);
        float px = std::round(x * s), py = std::round(y * s + f.ascent);
        stbtt_aligned_quad q;
        for (size_t i = 0; i < str.size();) {
            stbtt_GetPackedQuad(f.chars.data(), f.aw, f.ah, glyph_index(next_cp(str, i)), &px, &py, &q, 1);
            const SDL_FRect src{ q.s0 * f.aw, q.t0 * f.ah, (q.s1 - q.s0) * f.aw, (q.t1 - q.t0) * f.ah };
            const SDL_FRect dst{ std::round(q.x0), std::round(q.y0), std::round(q.x1 - q.x0), std::round(q.y1 - q.y0) };
            SDL_RenderTexture(renderer, f.tex, &src, &dst);
            px += track * s;
        }
    }

    void text_shadow(const face& f, const std::string& str, float x, float y, rgba c, float track = 0.f)
    {
        text(f, str, x + 1.f, y + 1.5f, { 0, 0, 0, (uint8_t)(c.a * 0.80f) }, track);
        text(f, str, x, y, c, track);
    }

    static SDL_FColor fcol(rgba c) { return { c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f }; }

    void grad_rect(SDL_FRect r, rgba top, rgba bot)
    {
        r = { r.x * s, r.y * s, r.w * s, r.h * s };
        const SDL_FColor ct = fcol(top), cb = fcol(bot);
        const SDL_Vertex v[4] = {
            { { r.x, r.y }, ct, {} },
            { { r.x + r.w, r.y }, ct, {} },
            { { r.x + r.w, r.y + r.h }, cb, {} },
            { { r.x, r.y + r.h }, cb, {} },
        };
        const int idx[6] = { 0, 1, 2, 0, 2, 3 };
        SDL_RenderGeometry(renderer, nullptr, v, 4, idx, 6);
    }

    void horiz_grad(SDL_FRect r, rgba left, rgba right)
    {
        r = { r.x * s, r.y * s, r.w * s, r.h * s };
        const SDL_FColor cl = fcol(left), cr = fcol(right);
        const SDL_Vertex v[4] = {
            { { r.x, r.y }, cl, {} },
            { { r.x + r.w, r.y }, cr, {} },
            { { r.x + r.w, r.y + r.h }, cr, {} },
            { { r.x, r.y + r.h }, cl, {} },
        };
        const int idx[6] = { 0, 1, 2, 0, 2, 3 };
        SDL_RenderGeometry(renderer, nullptr, v, 4, idx, 6);
    }

    void radial_glow(float cx, float cy, float rx, float ry, rgba center)
    {
        constexpr int seg = 36;
        std::array<SDL_Vertex, seg + 2> v;
        v[0] = { { cx * s, cy * s }, fcol(center), {} };
        const SDL_FColor edge{ center.r / 255.f, center.g / 255.f, center.b / 255.f, 0.f };
        for (int i = 0; i <= seg; i++) {
            const float a = (float)i / seg * 2.f * k_pi;
            v[(size_t)i + 1] = { { (cx + std::cos(a) * rx) * s, (cy + std::sin(a) * ry) * s }, edge, {} };
        }
        std::array<int, seg * 3> idx;
        for (int i = 0; i < seg; i++) {
            idx[(size_t)i * 3] = 0;
            idx[(size_t)i * 3 + 1] = i + 1;
            idx[(size_t)i * 3 + 2] = i + 2;
        }
        SDL_RenderGeometry(renderer, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
    }

    void round_rect_grad(SDL_FRect r, float radius, rgba top, rgba bot)
    {
        r = { r.x * s, r.y * s, r.w * s, r.h * s };
        radius = std::min(radius * s, std::min(r.w, r.h) / 2);
        auto sample = [&](float py) -> SDL_FColor {
            const float t = r.h > 0 ? std::clamp((py - r.y) / r.h, 0.f, 1.f) : 0.f;
            return {
                (top.r + (bot.r - top.r) * t) / 255.f,
                (top.g + (bot.g - top.g) * t) / 255.f,
                (top.b + (bot.b - top.b) * t) / 255.f,
                (top.a + (bot.a - top.a) * t) / 255.f,
            };
        };
        std::array<SDL_Vertex, 37> v;
        v[0] = { { r.x + r.w / 2, r.y + r.h / 2 }, sample(r.y + r.h / 2), {} };
        const float cx[4] = { r.x + r.w - radius, r.x + radius, r.x + radius, r.x + r.w - radius };
        const float cy[4] = { r.y + radius, r.y + radius, r.y + r.h - radius, r.y + r.h - radius };
        size_t nv = 1;
        for (int k = 0; k < 4; k++)
            for (int j = 0; j <= 8; j++) {
                const float a = (float)(k * 90 + j * 90 / 8) * k_pi / 180.f;
                const float px = cx[k] + std::cos(a) * radius;
                const float py = cy[k] - std::sin(a) * radius;
                v[nv++] = { { px, py }, sample(py), {} };
            }
        std::array<int, 36 * 3> idx;
        for (int i = 1; i < 37; i++) {
            idx[(size_t)(i - 1) * 3] = 0;
            idx[(size_t)(i - 1) * 3 + 1] = i;
            idx[(size_t)(i - 1) * 3 + 2] = i + 1 < 37 ? i + 1 : 1;
        }
        SDL_RenderGeometry(renderer, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
    }

    void round_rect(SDL_FRect r, float radius, rgba c) { round_rect_grad(r, radius, c, c); }

    void line(float x0, float y0, float x1, float y1, float w, rgba c)
    {
        const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0) return;
        const float nx = -dy / len * w / 2 * s, ny = dx / len * w / 2 * s;
        const SDL_FColor col = fcol(c);
        const SDL_Vertex v[4] = { { { x0 * s + nx, y0 * s + ny }, col, {} }, { { x1 * s + nx, y1 * s + ny }, col, {} },
                                  { { x1 * s - nx, y1 * s - ny }, col, {} }, { { x0 * s - nx, y0 * s - ny }, col, {} } };
        const int idx[6] = { 0, 1, 2, 0, 2, 3 };
        SDL_RenderGeometry(renderer, nullptr, v, 4, idx, 6);
    }

    void status_icon(float cx, float cy, item_state st)
    {
        const rgba c = st == item_state::ok ? c_ok : st == item_state::warn ? c_warn : st == item_state::bad ? c_bad : c_missing;
        round_rect({ cx - 16, cy - 16, 32, 32 }, 16, { c.r, c.g, c.b, 30 });
        round_rect({ cx - 11.5f, cy - 11.5f, 23, 23 }, 11.5f, c);
        if (st == item_state::ok) {
            line(cx - 5.2f, cy + 0.5f, cx - 1.2f, cy + 4.4f, 2.5f, c_bg_top);
            line(cx - 1.2f, cy + 4.4f, cx + 5.6f, cy - 3.6f, 2.5f, c_bg_top);
        } else if (st == item_state::bad) {
            line(cx - 4.2f, cy - 4.2f, cx + 4.2f, cy + 4.2f, 2.5f, c_bg_top);
            line(cx + 4.2f, cy - 4.2f, cx - 4.2f, cy + 4.2f, 2.5f, c_bg_top);
        } else if (st == item_state::warn) {
            line(cx, cy - 5.2f, cx, cy + 1.2f, 2.5f, c_bg_top);
            round_rect({ cx - 1.5f, cy + 3.4f, 3.f, 3.f }, 1.5f, c_bg_top);
        } else {
            line(cx - 4.5f, cy, cx + 4.5f, cy, 2.5f, c_bg_top);
        }
    }

    SDL_FRect row_rect(int i) const { return { k_pad + 8.f, k_panel_y + 8.f + i * k_row_h, k_w - 2 * k_pad - 16.f, k_row_h }; }

    void layout()
    {
        buttons.clear();
        const bool busy = job.phase == 1 || job.phase == 2;

        auto add_row_btns = [&](int row, std::initializer_list<std::tuple<const char*, action, btn_style>> items) {
            const SDL_FRect r = row_rect(row);
            float x = r.x + r.w - 16.f;
            for (auto it = std::rbegin(items); it != std::rend(items); ++it) {
                const char* label = std::get<0>(*it);
                const float w = std::max(96.f, measure(btn_font, label) + 32.f);
                x -= w;
                buttons.push_back({ { x, r.y + (r.h - 38.f) * 0.5f, w, 38.f }, label, std::get<1>(*it), !busy, std::get<2>(*it) });
                x -= 10.f;
            }
            std::reverse(buttons.end() - (ptrdiff_t)items.size(), buttons.end());
        };

        // Direct per-row controls (clear, intuitive, zero ambiguity)
        add_row_btns(0, { { "Select Folder", a_game_folder, btn_style::secondary }, { "Select ISO", a_game_iso, btn_style::secondary } });
        add_row_btns(1, { { "Select .pkg", a_update_pkg, btn_style::secondary } });
        if (paths.dlcs.empty()) add_row_btns(2, { { "Add DLC", a_dlc_add, btn_style::secondary } });
        else add_row_btns(2, { { "Add DLC", a_dlc_add, btn_style::secondary }, { "Clear", a_dlc_clear, btn_style::secondary } });
        if (check.licences.state == item_state::warn) add_row_btns(3, { { "Add .rap", a_rap_add, btn_style::gold } });
        add_row_btns(4, { { check.firmware.state == item_state::ok ? "Reinstall" : "Download", a_download,
                            check.firmware.state == item_state::ok ? btn_style::secondary : btn_style::gold } });

        const bool ready = check.ready() && !busy;
        buttons.push_back({ { k_pad, k_h - 78.f, k_w - 2 * k_pad, 54.f },
                            ready ? "Play" : "Complete required setup above to play",
                            a_play, ready, btn_style::play });
        focus = std::clamp(focus, 0, (int)buttons.size() - 1);
    }

    void draw_row(int i, const char* name, const char* hint, const item_status& st)
    {
        const SDL_FRect r = row_rect(i);
        if (hit(r, mouse_x, mouse_y))
            round_rect({ r.x + 2, r.y + 4, r.w - 4, r.h - 8 }, 10.f, c_row_hover);

        if (i > 0)
            round_rect({ r.x + 16.f, r.y, r.w - 32.f, 1.f }, 0, c_divider);

        status_icon(r.x + 32.f, r.y + r.h * 0.5f, st.state);
        text(heading, name, r.x + 64.f, r.y + 13.f, c_text);

        float right = r.x + r.w - 18.f;
        for (const button& b : buttons)
            if (b.r.y >= r.y && b.r.y + b.r.h <= r.y + r.h) right = std::min(right, b.r.x - 18.f);
        const float width = std::max(80.f, right - (r.x + 64.f));

        if (i == 4 && (job.phase == 1 || job.phase == 2)) {
            const float p = std::clamp(job.progress.load(), 0.f, 1.f);
            round_rect({ r.x + 64.f, r.y + 46.f, width, 8.f }, 4.f, { 42, 32, 30, 255 });
            if (p > 0.f)
                round_rect_grad({ r.x + 64.f, r.y + 46.f, std::max(8.f, width * p), 8.f }, 4.f, c_gold, c_play_hot_top);
            const std::string label = std::string(job.phase == 1 ? "Downloading " : "Installing ") + std::to_string((int)(p * 100)) + "%";
            text(body, label, r.x + 64.f + width - measure(body, label), r.y + 15.f, c_gold);
            return;
        }
        const bool failed = i == 4 && job.phase == 3;
        const std::string detail = failed ? job_error : (st.state == item_state::missing ? std::string(hint) : st.text);
        const rgba c = failed || st.state == item_state::bad ? c_bad : st.state == item_state::warn ? c_warn : st.state == item_state::ok ? c_sub : c_dim;
        text(body, fit(body, detail, width), r.x + 64.f, r.y + 41.f, c);
    }

    void draw_button(const button& b, bool focused, float t)
    {
        const bool hot = b.enabled && hit(b.r, mouse_x, mouse_y);
        const bool down = hot && pressed;
        const bool is_play = b.style == btn_style::play;
        const float rad = is_play ? 11.f : 9.f;

        if (is_play && b.enabled) {
            const float pulse = 0.5f + 0.5f * std::sin(t * 2.4f);
            round_rect({ b.r.x - 4, b.r.y - 3, b.r.w + 8, b.r.h + 6 }, rad + 3.f, { 224, 62, 28, (uint8_t)(34 + pulse * 40) });
        }

        // Refined 1px border
        rgba border = c_btn_border;
        if (!b.enabled) border = c_disabled_border;
        else if (focused) border = c_gold_bright;
        else if (is_play) border = hot ? rgba{ 255, 198, 134, 210 } : rgba{ 242, 166, 98, 145 };
        else if (b.style == btn_style::gold) border = hot ? c_gold_bright : rgba{ 236, 186, 94, 185 };
        else if (hot) border = c_btn_hot_border;
        round_rect({ b.r.x - 1, b.r.y - 1, b.r.w + 2, b.r.h + 2 }, rad + 1.f, border);

        // Clean modern fill
        if (!b.enabled) {
            round_rect(b.r, rad, c_disabled);
        } else if (is_play) {
            if (down) round_rect(b.r, rad, c_play_down);
            else round_rect_grad(b.r, rad, hot ? c_play_hot_top : c_play_top, hot ? c_play_hot_bot : c_play_bot);
        } else if (b.style == btn_style::gold) {
            round_rect(b.r, rad, down ? c_btn_down : hot ? c_gold_btn_hot : c_gold_btn_fill);
        } else {
            round_rect(b.r, rad, down ? c_btn_down : hot ? c_btn_hot : c_btn_fill);
        }

        // Subtle top glass specular highlight
        if (b.enabled && !down) {
            const rgba hi = is_play ? rgba{ 255, 226, 190, 68 } : rgba{ 255, 238, 214, (uint8_t)(hot ? 36 : 18) };
            round_rect({ b.r.x + rad, b.r.y + 1.f, b.r.w - rad * 2.f, 1.f }, 0, hi);
        }

        const face& f = is_play ? (b.enabled ? play_font : body) : btn_font;
        const float track = is_play && b.enabled ? 1.5f : 0.f;
        const float tw = measure(f, b.label, track);
        const rgba tc = !b.enabled ? c_dim : is_play ? c_text : (b.style == btn_style::gold ? c_gold_bright : (hot ? c_text : c_sub));
        const float tx = b.r.x + (b.r.w - tw) * 0.5f;
        const float ty = b.r.y + (b.r.h - f.size / s) * 0.5f - 1.f;
        if (is_play && b.enabled) text_shadow(f, b.label, tx, ty, tc, track);
        else text(f, b.label, tx, ty, tc, track);
    }

    static bool hit(const SDL_FRect& r, float x, float y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

    bool handle(SDL_Event& e)
    {
        SDL_ConvertEventToRenderCoordinates(renderer, &e);
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            return false;
        case SDL_EVENT_MOUSE_MOTION:
            mouse_x = e.motion.x / s;
            mouse_y = e.motion.y / s;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            pressed = e.button.button == SDL_BUTTON_LEFT;
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (!pressed || e.button.button != SDL_BUTTON_LEFT) break;
            pressed = false;
            for (size_t i = 0; i < buttons.size(); i++)
                if (buttons[i].enabled && hit(buttons[i].r, e.button.x / s, e.button.y / s)) {
                    focus = (int)i;
                    activate(buttons[i].id);
                    break;
                }
            break;
        }
        case SDL_EVENT_DROP_FILE: {
            std::lock_guard<std::mutex> hold(picks_lock);
            picks.push_back(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(e.drop.data))));
            break;
        }
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_ESCAPE) return false;
            if (e.key.key == SDLK_UP || e.key.key == SDLK_LEFT || (e.key.key == SDLK_TAB && (e.key.mod & SDL_KMOD_SHIFT))) move(-1);
            else if (e.key.key == SDLK_DOWN || e.key.key == SDLK_RIGHT || e.key.key == SDLK_TAB) move(1);
            else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_SPACE || e.key.key == SDLK_KP_ENTER) press_focus();
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            SDL_OpenGamepad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP || e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT) move(-1);
            else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN || e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) move(1);
            else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) press_focus();
            break;
        default:
            break;
        }
        return true;
    }

    void move(int dir)
    {
        const int n = (int)buttons.size();
        for (int i = 1; i <= n; i++) {
            const int next = ((focus + dir * i) % n + n) % n;
            if (buttons[next].enabled) {
                focus = next;
                return;
            }
        }
    }

    void press_focus()
    {
        if (focus < (int)buttons.size() && buttons[focus].enabled) activate(buttons[focus].id);
    }

    static void on_pick(void* user, const char* const* list, int)
    {
        auto* self = static_cast<setup_ui*>(user);
        if (!list || !*list) return;
        std::lock_guard<std::mutex> hold(self->picks_lock);
        for (; *list; list++) self->picks.push_back(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(*list))));
    }

    void activate(action id)
    {
        static const SDL_DialogFileFilter f_iso{ "PS3 ISO Image", "iso" };
        static const SDL_DialogFileFilter f_pkg{ "PS3 Update Package", "pkg" };
        static const SDL_DialogFileFilter f_dlc{ "PS3 DLC / Licence", "pkg;rap" };
        static const SDL_DialogFileFilter f_rap{ "PS3 Licence (.rap)", "rap" };
        switch (id) {
        case a_game_folder: SDL_ShowOpenFolderDialog(on_pick, this, window, nullptr, false); break;
        case a_game_iso: SDL_ShowOpenFileDialog(on_pick, this, window, &f_iso, 1, nullptr, false); break;
        case a_update_pkg: SDL_ShowOpenFileDialog(on_pick, this, window, &f_pkg, 1, nullptr, false); break;
        case a_dlc_add: SDL_ShowOpenFileDialog(on_pick, this, window, &f_dlc, 1, nullptr, true); break;
        case a_dlc_clear:
            paths.dlcs.clear();
            recheck();
            break;
        case a_rap_add: SDL_ShowOpenFileDialog(on_pick, this, window, &f_rap, 1, nullptr, true); break;
        case a_download: start_job({}); break;
        case a_play: play = true; break;
        }
    }

    void apply_picks()
    {
        std::vector<std::filesystem::path> items;
        {
            std::lock_guard<std::mutex> hold(picks_lock);
            items.swap(picks);
        }
        if (items.empty()) return;
        draw();
        const std::filesystem::path pup = add_game_items(exe, paths, items);
        if (!pup.empty() && job.phase != 1 && job.phase != 2) start_job(pup);
        recheck();
    }

    void recheck()
    {
        draw();
        check = check_game_paths(exe, paths);
    }

    void start_job(std::filesystem::path pup)
    {
        if (job.worker.joinable()) job.worker.join();
        paths.firmware.clear();
        const std::filesystem::path flash = firmware_dir(exe, paths);
        const bool download = pup.empty();
        if (download) pup = exe / "user" / "PS3UPDAT.PUP";
        job.progress = 0;
        job.phase = download ? 1 : 2;
        job.worker = std::thread([this, pup, flash, download] {
            std::string error;
            bool ok = !download || firmware_download(pup, job.progress, error);
            if (ok) {
                job.progress = 0;
                job.phase = 2;
                ok = firmware_extract(pup, flash, job.progress, error);
            }
            if (download) {
                std::error_code ec;
                std::filesystem::remove(pup, ec);
            }
            std::lock_guard<std::mutex> hold(job.lock);
            job.error = error;
            job.phase = ok ? 4 : 3;
        });
    }

    void finish_job()
    {
        if (job.phase < 3 || !job.worker.joinable()) return;
        job.worker.join();
        std::lock_guard<std::mutex> hold(job.lock);
        job_error = job.error;
        check.firmware = firmware_installed(firmware_dir(exe, paths)) ? item_status{ item_state::ok, "Installed" } : check.firmware;
        if (job.phase == 4) job.phase = 0;
    }

    std::filesystem::path exe;
    game_paths& paths;
    game_check check;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* art_tex = nullptr;
    int art_w = 0, art_h = 0;
    firmware_job job;
    font regular, semibold, black;
    face hero_title, hero_sub, play_font, heading, body, btn_font;
    float s = 1, mouse_x = -1, mouse_y = -1;
    bool pressed = false, play = false;
    int focus = 0;
    std::vector<button> buttons;
    std::string job_error;
    std::mutex picks_lock;
    std::vector<std::filesystem::path> picks;
    std::array<ember, 26> embers{};
    std::chrono::steady_clock::time_point t0, last_t;
};

}

bool run_setup(const std::filesystem::path& exe, const std::filesystem::path& icon, game_paths& paths, game_check check)
{
    setup_ui ui(exe, paths, std::move(check));
    return ui.open(icon) && ui.run();
}
