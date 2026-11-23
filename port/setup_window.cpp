#include "setup_window.h"

#include "firmware.h"
#include "font.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

namespace {

constexpr float k_w = 860, k_h = 668, k_pad = 32;
constexpr float k_drop_y = 114, k_drop_h = 52;
constexpr float k_cards_y = 180, k_card_h = 68, k_card_gap = 9;
constexpr float k_pi = 3.14159265359f;

struct rgba {
    uint8_t r, g, b, a = 255;
};

// Asura's Wrath / Vajra Mantra palette: volcanic obsidian, burnished gold, molten crimson
constexpr rgba c_bg_top{ 11, 8, 9 }, c_bg_bot{ 24, 11, 10 };
constexpr rgba c_card_top{ 30, 22, 21 }, c_card_bot{ 21, 16, 16 };
constexpr rgba c_card_border{ 64, 46, 38 }, c_card_border_ok{ 70, 96, 68 };
constexpr rgba c_drop_bg{ 25, 18, 17 }, c_drop_border{ 112, 82, 46 }, c_drop_hot{ 226, 174, 78 };

constexpr rgba c_text{ 244, 236, 226 }, c_muted{ 168, 152, 140 }, c_dim{ 116, 102, 94 };
constexpr rgba c_gold{ 236, 188, 96 }, c_gold_dim{ 168, 126, 58 }, c_gold_bright{ 255, 222, 146 };

constexpr rgba c_crimson_top{ 204, 44, 30 }, c_crimson_bot{ 146, 22, 18 };
constexpr rgba c_crimson_hot_top{ 232, 66, 36 }, c_crimson_hot_bot{ 176, 32, 22 };
constexpr rgba c_crimson_down{ 126, 20, 16 };

constexpr rgba c_btn_top{ 52, 39, 35 }, c_btn_bot{ 38, 29, 26 };
constexpr rgba c_btn_hot_top{ 74, 54, 46 }, c_btn_hot_bot{ 54, 40, 34 };
constexpr rgba c_btn_down{ 30, 23, 21 };
constexpr rgba c_disabled{ 34, 28, 27 };

constexpr rgba c_ok{ 98, 196, 112 }, c_warn{ 238, 178, 66 }, c_bad{ 232, 86, 72 }, c_missing{ 122, 104, 94 };

enum action { a_files, a_folder, a_dlc_clear, a_download, a_play };

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

struct button {
    SDL_FRect r;
    std::string label;
    action id;
    bool enabled, accent, gold_outline;
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
            em.vy = 14.f + rng() * 28.f;
            em.size = 1.6f + rng() * 2.8f;
            em.phase = rng() * k_pi * 2.f;
            em.speed = 0.8f + rng() * 1.4f;
            em.alpha = 0.25f + rng() * 0.55f;
        }
    }

    bool open(const std::filesystem::path& icon)
    {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return false;
        if (!load_font(regular, font_weight::regular) || !load_font(bold, font_weight::bold)) return false;
        const float ds = std::max(1.f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
        window = SDL_CreateWindow("Vajra \xE2\x80\x93 Asura's Wrath", (int)(k_w * ds), (int)(k_h * ds), SDL_WINDOW_HIGH_PIXEL_DENSITY);
        renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
        if (!renderer) return false;
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        if (SDL_Surface* image = icon.empty() ? nullptr : SDL_LoadPNG(icon.string().c_str())) {
            SDL_SetWindowIcon(window, image);
            icon_tex = SDL_CreateTextureFromSurface(renderer, image);
            if (icon_tex) SDL_SetTextureScaleMode(icon_tex, SDL_SCALEMODE_LINEAR);
            SDL_DestroySurface(image);
        }
        SDL_SetRenderVSync(renderer, 1);
        int pw = 0, ph = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &pw, &ph);
        s = (float)pw / k_w;
        bake(title, bold, 32);
        bake(play_font, bold, 20);
        bake(heading, bold, 15.5f);
        bake(body, regular, 14.5f);
        bake(small, regular, 12.5f);
        t0 = std::chrono::steady_clock::now();
        last_t = t0;
        return true;
    }

    ~setup_ui()
    {
        if (job.worker.joinable()) job.worker.join();
        if (icon_tex) SDL_DestroyTexture(icon_tex);
        for (face* f : { &title, &play_font, &heading, &body, &small })
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

        // 1. Atmospheric volcanic background + bottom Mantra furnace glow
        grad_rect({ 0, 0, k_w, k_h }, c_bg_top, c_bg_bot);
        radial_glow(k_w * 0.5f, k_h * 1.08f, k_w * 0.72f, k_h * 0.55f, { 168, 42, 18, 62 });
        radial_glow(k_w * 0.14f, 54.f, 260.f, 160.f, { 214, 152, 54, 28 });

        // 2. Slowly rotating sacred Mantra mandala halo behind header
        draw_mandala(k_w - 110.f, 54.f, 118.f, t * 0.08f);

        // 3. Drifting golden-crimson Mantra embers
        for (ember& em : embers) {
            em.y -= em.vy * dt;
            if (em.y < -10.f) {
                em.y = k_h + 10.f;
                em.x = std::fmod(em.x + 173.f, k_w);
            }
            const float ex = em.x + std::sin(t * em.speed + em.phase) * 14.f;
            const float fade = std::clamp(std::min(em.y / 140.f, (k_h - em.y) / 90.f), 0.f, 1.f);
            const uint8_t a = (uint8_t)(em.alpha * fade * 255.f);
            if (a > 3) {
                round_rect({ ex - em.size * 1.6f, em.y - em.size * 1.6f, em.size * 3.2f, em.size * 3.2f }, em.size * 1.6f, { 235, 110, 32, (uint8_t)(a / 3) });
                round_rect({ ex - em.size * 0.5f, em.y - em.size * 0.5f, em.size, em.size }, em.size * 0.5f, { 255, 206, 110, a });
            }
        }

        layout();

        // 4. Header: Icon + VAJRA + Subtitle + Readiness Badge + Ornate Gold Divider
        float hx = k_pad;
        if (icon_tex) {
            const SDL_FRect ir{ hx, 24, 58, 58 };
            round_rect({ ir.x - 2, ir.y - 2, ir.w + 4, ir.h + 4 }, 12, { 214, 168, 82, 160 });
            round_rect(ir, 10, c_bg_top);
            const SDL_FRect dst{ ir.x * s, ir.y * s, ir.w * s, ir.h * s };
            SDL_RenderTexture(renderer, icon_tex, nullptr, &dst);
            hx += 74.f;
        }
        text(title, "VAJRA", hx, 23, c_gold);
        text(small, "ASURA'S WRATH  \xC2\xB7  PC RECOMPILATION", hx + 1.f, 61, c_muted);

        // Readiness pill badge on top-right
        int done_n = 0;
        for (const item_status* st : { &check.game, &check.update, &check.dlc, &check.firmware })
            if (st->state == item_state::ok) done_n++;
        const bool all_ok = check.ready();
        const std::string badge_txt = all_ok ? "READY TO LAUNCH" : (std::to_string(done_n) + " / 4 READY");
        const float bw = measure(small, badge_txt) + 28.f;
        const SDL_FRect br{ k_w - k_pad - bw, 39, bw, 28 };
        round_rect(br, 14, all_ok ? rgba{ 42, 84, 48, 220 } : rgba{ 68, 44, 24, 210 });
        round_rect({ br.x + 1, br.y + 1, br.w - 2, br.h - 2 }, 13, all_ok ? rgba{ 22, 46, 26, 230 } : rgba{ 36, 24, 18, 230 });
        text(small, badge_txt, br.x + 14.f, br.y + 7.f, all_ok ? c_ok : c_gold);

        // Ornate engraved gold divider
        draw_gold_divider(k_pad, 96.f, k_w - 2 * k_pad);

        // 5. Drag-and-drop quick-add bar
        const SDL_FRect dr{ k_pad, k_drop_y, k_w - 2 * k_pad, k_drop_h };
        const bool drop_hot = hit(dr, mouse_x, mouse_y);
        round_rect(dr, 10, drop_hot ? c_drop_hot : c_drop_border);
        round_rect({ dr.x + 1, dr.y + 1, dr.w - 2, dr.h - 2 }, 9, c_drop_bg);
        text(body, "Drop ISO, game folder, 1.02 .pkg, or DLC .pkg / .rap anywhere", dr.x + 18.f, dr.y + 17.f, c_muted);

        // 6. Status cards
        const char* names[] = { "Game (BLES01227)", "Update 1.02", "DLC Packages", "DLC Licences", "PS3 Firmware" };
        const char* hints[] = {
            "Drop your PS3_GAME folder or decrypted ISO",
            "Drop the official 1.02 update .pkg",
            "Optional \xE2\x80\x93 drop DLC .pkg files",
            "Optional \xE2\x80\x93 drop matching .rap files for DLC",
            "Click Download to install audio codecs from Sony",
        };
        const item_status* items[] = { &check.game, &check.update, &check.dlc, &check.licences, &check.firmware };
        for (int i = 0; i < 5; i++) card(i, names[i], hints[i], *items[i], t);

        // 7. Buttons (including pulsing Play button)
        for (size_t i = 0; i < buttons.size(); i++) draw_button(buttons[i], (int)i == focus, t);

        // 8. Subtle footer controls hint
        const std::string foot = "Drag & drop files  \xC2\xB7  Arrow keys / Gamepad to navigate  \xC2\xB7  Enter / A to select  \xC2\xB7  Esc to quit";
        text(small, foot, (k_w - measure(small, foot)) * 0.5f, k_h - 22.f, c_dim);

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
        stbtt_PackSetOversampling(&pc, 2, 1);
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
    }

    float measure(const face& f, const std::string& str) const
    {
        float x = 0, y = 0;
        stbtt_aligned_quad q;
        for (size_t i = 0; i < str.size();) stbtt_GetPackedQuad(f.chars.data(), f.aw, f.ah, glyph_index(next_cp(str, i)), &x, &y, &q, 0);
        return x / s;
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

    void text(const face& f, const std::string& str, float x, float y, rgba c)
    {
        SDL_SetTextureColorMod(f.tex, c.r, c.g, c.b);
        SDL_SetTextureAlphaMod(f.tex, c.a);
        float px = std::round(x * s), py = std::round(y * s + f.ascent);
        stbtt_aligned_quad q;
        for (size_t i = 0; i < str.size();) {
            stbtt_GetPackedQuad(f.chars.data(), f.aw, f.ah, glyph_index(next_cp(str, i)), &px, &py, &q, 0);
            const SDL_FRect src{ q.s0 * f.aw, q.t0 * f.ah, (q.s1 - q.s0) * f.aw, (q.t1 - q.t0) * f.ah };
            const SDL_FRect dst{ q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0 };
            SDL_RenderTexture(renderer, f.tex, &src, &dst);
        }
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

    void draw_mandala(float cx, float cy, float radius, float angle)
    {
        constexpr int spokes = 12;
        const rgba ring_col{ 226, 174, 78, 18 };
        const rgba spoke_col{ 226, 174, 78, 14 };
        for (int r_idx = 0; r_idx < 2; r_idx++) {
            const float rad = r_idx == 0 ? radius : radius * 0.62f;
            constexpr int n = 36;
            for (int i = 0; i < n; i++) {
                const float a0 = angle + (float)i / n * 2.f * k_pi;
                const float a1 = angle + (float)(i + 1) / n * 2.f * k_pi;
                line(cx + std::cos(a0) * rad, cy + std::sin(a0) * rad, cx + std::cos(a1) * rad, cy + std::sin(a1) * rad, 1.3f, ring_col);
            }
        }
        for (int i = 0; i < spokes; i++) {
            const float a = angle + (float)i / spokes * 2.f * k_pi;
            line(cx + std::cos(a) * (radius * 0.24f), cy + std::sin(a) * (radius * 0.24f),
                 cx + std::cos(a) * radius, cy + std::sin(a) * radius, 1.1f, spoke_col);
        }
    }

    void draw_gold_divider(float x, float y, float w)
    {
        const float mid = x + w * 0.5f;
        const SDL_FColor dim{ 214 / 255.f, 168 / 255.f, 82 / 255.f, 0.f };
        const SDL_FColor hi{ 236 / 255.f, 188 / 255.f, 96 / 255.f, 0.65f };
        const float y0 = y * s, y1 = (y + 1.5f) * s;
        const SDL_Vertex vl[4] = { { { x * s, y0 }, dim, {} }, { { mid * s, y0 }, hi, {} },
                                   { { mid * s, y1 }, hi, {} }, { { x * s, y1 }, dim, {} } };
        const SDL_Vertex vr[4] = { { { mid * s, y0 }, hi, {} }, { { (x + w) * s, y0 }, dim, {} },
                                   { { (x + w) * s, y1 }, dim, {} }, { { mid * s, y1 }, hi, {} } };
        const int idx[6] = { 0, 1, 2, 0, 2, 3 };
        SDL_RenderGeometry(renderer, nullptr, vl, 4, idx, 6);
        SDL_RenderGeometry(renderer, nullptr, vr, 4, idx, 6);
        // Central Mantra diamond accent
        const float d = 4.5f * s, ym = (y + 0.75f) * s;
        const SDL_FColor jewel = fcol(c_gold_bright);
        const SDL_Vertex dm[4] = { { { mid * s, ym - d }, jewel, {} }, { { mid * s + d, ym }, jewel, {} },
                                   { { mid * s, ym + d }, jewel, {} }, { { mid * s - d, ym }, jewel, {} } };
        SDL_RenderGeometry(renderer, nullptr, dm, 4, idx, 6);
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
        round_rect({ cx - 15, cy - 15, 30, 30 }, 15, { c.r, c.g, c.b, 38 });
        round_rect({ cx - 11, cy - 11, 22, 22 }, 11, c);
        if (st == item_state::ok) {
            line(cx - 5.f, cy + 0.5f, cx - 1.2f, cy + 4.2f, 2.4f, c_bg_top);
            line(cx - 1.2f, cy + 4.2f, cx + 5.6f, cy - 3.6f, 2.4f, c_bg_top);
        } else if (st == item_state::bad) {
            line(cx - 4.2f, cy - 4.2f, cx + 4.2f, cy + 4.2f, 2.4f, c_bg_top);
            line(cx + 4.2f, cy - 4.2f, cx - 4.2f, cy + 4.2f, 2.4f, c_bg_top);
        } else if (st == item_state::warn) {
            line(cx, cy - 5.2f, cx, cy + 1.2f, 2.4f, c_bg_top);
            round_rect({ cx - 1.5f, cy + 3.4f, 3.f, 3.f }, 1.5f, c_bg_top);
        } else {
            line(cx - 4.5f, cy, cx + 4.5f, cy, 2.4f, c_bg_top);
        }
    }

    SDL_FRect card_rect(int i) const { return { k_pad, k_cards_y + i * (k_card_h + k_card_gap), k_w - 2 * k_pad, k_card_h }; }

    void layout()
    {
        buttons.clear();
        const bool busy = job.phase == 1 || job.phase == 2;
        // Direct Add Files / Add Folder buttons inside drop bar
        const float bw_folder = 116.f, bw_files = 108.f;
        const float by = k_drop_y + (k_drop_h - 34.f) * 0.5f;
        buttons.push_back({ { k_w - k_pad - 10.f - bw_folder - 8.f - bw_files, by, bw_files, 34.f }, "+ Add Files", a_files, !busy, false, true });
        buttons.push_back({ { k_w - k_pad - 10.f - bw_folder, by, bw_folder, 34.f }, "+ Add Folder", a_folder, !busy, false, true });

        auto add_card_btn = [&](int row, const char* label, action act, bool gold) {
            const SDL_FRect r = card_rect(row);
            const float w = std::max(96.f, measure(body, label) + 28.f);
            buttons.push_back({ { r.x + r.w - 16.f - w, r.y + (r.h - 34.f) * 0.5f, w, 34.f }, label, act, !busy, false, gold });
        };

        if (check.game.state != item_state::ok) add_card_btn(0, "Browse", a_folder, false);
        if (check.update.state != item_state::ok) add_card_btn(1, "Browse", a_files, false);
        if (!paths.dlcs.empty()) add_card_btn(2, "Clear", a_dlc_clear, false);
        add_card_btn(4, check.firmware.state == item_state::ok ? "Reinstall" : "Download", a_download, check.firmware.state != item_state::ok);

        const bool ready = check.ready() && !busy;
        buttons.push_back({ { k_pad, k_h - 88.f, k_w - 2 * k_pad, 52.f },
                            ready ? "PLAY  ASURA'S WRATH" : "COMPLETE SETUP ABOVE TO PLAY",
                            a_play, ready, true, false });
        focus = std::clamp(focus, 0, (int)buttons.size() - 1);
    }

    void card(int i, const char* name, const char* hint, const item_status& st, float t)
    {
        const SDL_FRect r = card_rect(i);
        const rgba accent = st.state == item_state::ok ? c_ok : st.state == item_state::warn ? c_warn : st.state == item_state::bad ? c_bad : c_missing;
        round_rect(r, 10, st.state == item_state::ok ? c_card_border_ok : c_card_border);
        round_rect_grad({ r.x + 1, r.y + 1, r.w - 2, r.h - 2 }, 9, c_card_top, c_card_bot);
        // Left vertical Mantra status bar
        round_rect({ r.x + 1, r.y + 10, 3.5f, r.h - 20 }, 2, { accent.r, accent.g, accent.b, 210 });

        status_icon(r.x + 32, r.y + r.h * 0.5f, st.state);
        text(heading, name, r.x + 60, r.y + 13, c_text);

        float right = r.x + r.w - 18.f;
        for (const button& b : buttons)
            if (b.r.y >= r.y && b.r.y + b.r.h <= r.y + r.h) right = std::min(right, b.r.x - 16.f);
        const float width = std::max(80.f, right - (r.x + 60.f));

        if (i == 4 && (job.phase == 1 || job.phase == 2)) {
            const float p = std::clamp(job.progress.load(), 0.f, 1.f);
            round_rect({ r.x + 60, r.y + 44, width, 9 }, 4.5f, { 44, 32, 28, 255 });
            if (p > 0.f) {
                const float fw = std::max(10.f, width * p);
                round_rect_grad({ r.x + 60, r.y + 44, fw, 9 }, 4.5f, c_gold, c_crimson_hot_top);
            }
            const std::string label = std::string(job.phase == 1 ? "Downloading PS3 Firmware " : "Extracting Codecs ") + std::to_string((int)(p * 100)) + "%";
            text(small, label, r.x + 60 + width - measure(small, label), r.y + 15, c_gold);
            (void)t;
            return;
        }
        const bool failed = i == 4 && job.phase == 3;
        const std::string detail = failed ? job_error : (st.state == item_state::missing ? std::string(hint) : st.text);
        const rgba c = failed || st.state == item_state::bad ? c_bad : st.state == item_state::warn ? c_warn : st.state == item_state::ok ? c_text : c_muted;
        text(body, fit(body, detail, width), r.x + 60, r.y + 39, c);
    }

    void draw_button(const button& b, bool focused, float t)
    {
        const bool hot = b.enabled && hit(b.r, mouse_x, mouse_y);
        const bool down = hot && pressed;

        if (b.accent && b.enabled) {
            const float pulse = 0.5f + 0.5f * std::sin(t * 2.8f);
            round_rect({ b.r.x - 4, b.r.y - 4, b.r.w + 8, b.r.h + 8 }, 13, { 226, 72, 28, (uint8_t)(42 + pulse * 48) });
        }
        if (focused && b.enabled) {
            round_rect({ b.r.x - 2.5f, b.r.y - 2.5f, b.r.w + 5, b.r.h + 5 }, 11, c_gold);
        } else if (b.gold_outline && b.enabled) {
            round_rect({ b.r.x - 1, b.r.y - 1, b.r.w + 2, b.r.h + 2 }, 9, hot ? c_gold : c_gold_dim);
        } else if (b.accent && b.enabled) {
            round_rect({ b.r.x - 1, b.r.y - 1, b.r.w + 2, b.r.h + 2 }, 10, { 246, 178, 86, 190 });
        }

        if (!b.enabled) {
            round_rect(b.r, b.accent ? 9.f : 8.f, c_disabled);
        } else if (b.accent) {
            if (down) round_rect(b.r, 9, c_crimson_down);
            else round_rect_grad(b.r, 9, hot ? c_crimson_hot_top : c_crimson_top, hot ? c_crimson_hot_bot : c_crimson_bot);
        } else {
            if (down) round_rect(b.r, 8, c_btn_down);
            else round_rect_grad(b.r, 8, hot ? c_btn_hot_top : c_btn_top, hot ? c_btn_hot_bot : c_btn_bot);
        }

        const face& f = b.accent ? (b.enabled ? play_font : body) : body;
        const float tw = measure(f, b.label);
        const rgba tc = b.enabled ? (b.accent ? c_gold_bright : c_text) : c_dim;
        text(f, b.label, b.r.x + (b.r.w - tw) * 0.5f, b.r.y + (b.r.h - f.size / s) * 0.5f - 1.f, tc);
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
        static const SDL_DialogFileFilter files{ "PS3 files", "iso;pkg;rap;PUP;pup" };
        switch (id) {
        case a_files: SDL_ShowOpenFileDialog(on_pick, this, window, &files, 1, nullptr, true); break;
        case a_folder: SDL_ShowOpenFolderDialog(on_pick, this, window, nullptr, true); break;
        case a_dlc_clear:
            paths.dlcs.clear();
            recheck();
            break;
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
    SDL_Texture* icon_tex = nullptr;
    firmware_job job;
    font regular, bold;
    face title, play_font, heading, body, small;
    float s = 1, mouse_x = -1, mouse_y = -1;
    bool pressed = false, play = false;
    int focus = 0;
    std::vector<button> buttons;
    std::string job_error;
    std::mutex picks_lock;
    std::vector<std::filesystem::path> picks;
    std::array<ember, 28> embers{};
    std::chrono::steady_clock::time_point t0, last_t;
};

}

bool run_setup(const std::filesystem::path& exe, const std::filesystem::path& icon, game_paths& paths, game_check check)
{
    setup_ui ui(exe, paths, std::move(check));
    return ui.open(icon) && ui.run();
}
