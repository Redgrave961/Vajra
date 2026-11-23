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

constexpr float k_w = 900, k_h = 640, k_pad = 36;
constexpr float k_hero_h = 186;
constexpr float k_panel_y = 200, k_row_h = 62;
constexpr float k_pi = 3.14159265359f;

struct rgba {
    uint8_t r, g, b, a = 255;
};

// Volcanic obsidian -> molten crimson + burnished Vajra gold
constexpr rgba c_bg_top{ 10, 7, 8 }, c_bg_bot{ 25, 10, 10 };
constexpr rgba c_panel_top{ 20, 15, 16, 232 }, c_panel_bot{ 15, 11, 12, 242 };
constexpr rgba c_panel_border{ 96, 68, 44, 150 };
constexpr rgba c_row_hover{ 42, 28, 26, 130 };
constexpr rgba c_divider{ 214, 168, 82, 32 };

constexpr rgba c_text{ 246, 239, 230 }, c_sub{ 176, 160, 148 }, c_dim{ 122, 108, 100 };
constexpr rgba c_gold{ 240, 194, 102 }, c_gold_dim{ 168, 126, 58 }, c_gold_bright{ 255, 228, 158 };

constexpr rgba c_crimson_top{ 208, 44, 28 }, c_crimson_bot{ 142, 20, 18 };
constexpr rgba c_crimson_hot_top{ 236, 68, 36 }, c_crimson_hot_bot{ 172, 30, 22 };
constexpr rgba c_crimson_down{ 120, 18, 16 };

constexpr rgba c_btn_top{ 48, 35, 32, 235 }, c_btn_bot{ 34, 25, 23, 245 };
constexpr rgba c_btn_hot_top{ 72, 50, 42, 245 }, c_btn_hot_bot{ 52, 36, 31, 250 };
constexpr rgba c_btn_down{ 28, 20, 19, 250 };
constexpr rgba c_disabled{ 32, 25, 25, 220 };

constexpr rgba c_ok{ 96, 204, 118 }, c_warn{ 242, 182, 68 }, c_bad{ 236, 88, 74 }, c_missing{ 124, 106, 96 };

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
            em.vy = 14.f + rng() * 26.f;
            em.size = 1.5f + rng() * 2.5f;
            em.phase = rng() * k_pi * 2.f;
            em.speed = 0.7f + rng() * 1.3f;
            em.alpha = 0.22f + rng() * 0.52f;
        }
    }

    bool open(const std::filesystem::path& icon)
    {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return false;
        if (!load_font(regular, font_weight::regular) || !load_font(semibold, font_weight::semibold) || !load_font(black, font_weight::black))
            return false;
        const float ds = std::max(1.f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
        window = SDL_CreateWindow("Vajra \xE2\x80\x93 Asura's Wrath", (int)(k_w * ds), (int)(k_h * ds), SDL_WINDOW_HIGH_PIXEL_DENSITY);
        renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
        if (!renderer) return false;
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        // Window icon
        if (SDL_Surface* image = icon.empty() ? nullptr : SDL_LoadPNG(icon.string().c_str())) {
            SDL_SetWindowIcon(window, image);
            SDL_DestroySurface(image);
        }

        // Decode embedded Vajra Asura key art
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
        bake(hero_title, black, 40);
        bake(play_font, black, 18);
        bake(heading, semibold, 15.5f);
        bake(body, regular, 14.f);
        bake(small, semibold, 12.f);
        bake(micro, regular, 12.f);
        t0 = std::chrono::steady_clock::now();
        last_t = t0;
        return true;
    }

    ~setup_ui()
    {
        if (job.worker.joinable()) job.worker.join();
        if (art_tex) SDL_DestroyTexture(art_tex);
        for (face* f : { &hero_title, &play_font, &heading, &body, &small, &micro })
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

        // 1. Volcanic obsidian -> crimson base canvas
        grad_rect({ 0, 0, k_w, k_h }, c_bg_top, c_bg_bot);

        // 2. Full-bleed Vajra Asura artwork in upper half, smoothly fading into obsidian-crimson
        if (art_tex && art_w > 0 && art_h > 0) {
            const float target_h = 330.f;
            const float src_aspect = (float)art_w / (float)art_h;
            const float dst_aspect = k_w / target_h;
            SDL_FRect src{ 0, 0, (float)art_w, (float)art_h };
            if (src_aspect < dst_aspect) {
                src.h = (float)art_w / dst_aspect;
                src.y = ((float)art_h - src.h) * 0.22f; // frame Asura's six arms & fiery sky
            } else {
                src.w = (float)art_h * dst_aspect;
                src.x = ((float)art_w - src.w) * 0.5f;
            }
            const SDL_FRect dst{ 0, 0, k_w * s, target_h * s };
            SDL_SetTextureAlphaMod(art_tex, 205);
            SDL_RenderTexture(renderer, art_tex, &src, &dst);

            // Cinematic dark vignette + smooth vertical melt into obsidian-crimson
            grad_rect({ 0, 0, k_w, 95 }, { 10, 7, 8, 155 }, { 10, 7, 8, 15 });
            grad_rect({ 0, 120, k_w, 212 }, { 13, 8, 9, 20 }, { 16, 8, 9, 255 });
            // Left readability scrim behind VAJRA title
            horiz_grad({ 0, 0, 440, k_hero_h + 40 }, { 10, 7, 8, 185 }, { 10, 7, 8, 0 });
        }

        // 3. Bottom molten Mantra furnace glow & subtle rotating mandala accent
        radial_glow(k_w * 0.5f, k_h * 1.06f, k_w * 0.70f, k_h * 0.50f, { 172, 42, 18, 68 });
        draw_mandala(k_w - 86.f, 76.f, 72.f, t * 0.09f);

        // 4. Drifting golden-crimson Mantra embers
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
                round_rect({ ex - em.size * 0.5f, em.y - em.size * 0.5f, em.size, em.size }, em.size * 0.5f, { 255, 212, 122, a });
            }
        }

        layout();

        // 5. Clean Hero Title & Status Pill
        text_shadow(hero_title, "VAJRA", k_pad, 44.f, c_gold_bright);
        text_shadow(small, "ASURA'S WRATH  \xC2\xB7  PC PORT", k_pad + 2.f, 92.f, c_text);

        int done_n = 0;
        for (const item_status* st : { &check.game, &check.update, &check.dlc, &check.firmware })
            if (st->state == item_state::ok) done_n++;
        const bool all_ok = check.ready();
        const std::string badge_txt = all_ok ? "READY" : (std::to_string(done_n) + " / 4 READY");
        const float bw = measure(small, badge_txt) + 28.f;
        const SDL_FRect br{ k_w - k_pad - bw, 52.f, bw, 28.f };
        round_rect(br, 14, all_ok ? rgba{ 56, 120, 68, 220 } : rgba{ 112, 78, 38, 210 });
        round_rect({ br.x + 1, br.y + 1, br.w - 2, br.h - 2 }, 13, all_ok ? rgba{ 18, 40, 22, 230 } : rgba{ 28, 18, 14, 230 });
        text(small, badge_txt, br.x + 14.f, br.y + 7.f, all_ok ? c_ok : c_gold);

        // Quick-add row cleanly anchored at bottom of hero band
        text_shadow(body, "Drop your ISO, game folder, update .pkg, or DLC files anywhere", k_pad, k_hero_h - 32.f, c_sub);

        // Engraved gold accent line separating hero art from setup panel
        draw_gold_divider(k_pad, k_hero_h + 2.f, k_w - 2 * k_pad);

        // 6. Unified frosted volcanic-obsidian checklist panel
        const float panel_h = 5.f * k_row_h + 12.f;
        const SDL_FRect pr{ k_pad, k_panel_y, k_w - 2 * k_pad, panel_h };
        round_rect(pr, 12, c_panel_border);
        round_rect_grad({ pr.x + 1, pr.y + 1, pr.w - 2, pr.h - 2 }, 11, c_panel_top, c_panel_bot);

        const char* names[] = { "Game", "Update 1.02", "DLC", "Licences", "Firmware" };
        const char* hints[] = {
            "Drop BLES01227 disc folder or decrypted ISO",
            "Drop official 1.02 update .pkg",
            "Optional \xE2\x80\x93 drop DLC .pkg packages",
            "Optional \xE2\x80\x93 drop matching .rap files for DLC",
            "Click Download to install Sony audio codecs automatically",
        };
        const item_status* items[] = { &check.game, &check.update, &check.dlc, &check.licences, &check.firmware };
        for (int i = 0; i < 5; i++) draw_row(i, names[i], hints[i], *items[i]);

        // 7. Action buttons + Hero PLAY bar
        for (size_t i = 0; i < buttons.size(); i++) draw_button(buttons[i], (int)i == focus, t);

        // 8. Minimal footer
        const std::string foot = "Drag & drop files  \xC2\xB7  Keyboard / Gamepad supported  \xC2\xB7  Esc to quit";
        text(micro, foot, (k_w - measure(micro, foot)) * 0.5f, k_h - 21.f, c_dim);

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

    void text_shadow(const face& f, const std::string& str, float x, float y, rgba c)
    {
        text(f, str, x + 1.f, y + 1.5f, { 0, 0, 0, (uint8_t)(c.a * 0.75f) });
        text(f, str, x, y, c);
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

    void draw_mandala(float cx, float cy, float radius, float angle)
    {
        constexpr int spokes = 12;
        const rgba ring_col{ 240, 194, 102, 26 };
        const rgba spoke_col{ 240, 194, 102, 18 };
        for (int r_idx = 0; r_idx < 2; r_idx++) {
            const float rad = r_idx == 0 ? radius : radius * 0.62f;
            constexpr int n = 36;
            for (int i = 0; i < n; i++) {
                const float a0 = angle + (float)i / n * 2.f * k_pi;
                const float a1 = angle + (float)(i + 1) / n * 2.f * k_pi;
                line(cx + std::cos(a0) * rad, cy + std::sin(a0) * rad, cx + std::cos(a1) * rad, cy + std::sin(a1) * rad, 1.2f, ring_col);
            }
        }
        for (int i = 0; i < spokes; i++) {
            const float a = angle + (float)i / spokes * 2.f * k_pi;
            line(cx + std::cos(a) * (radius * 0.24f), cy + std::sin(a) * (radius * 0.24f),
                 cx + std::cos(a) * radius, cy + std::sin(a) * radius, 1.0f, spoke_col);
        }
    }

    void draw_gold_divider(float x, float y, float w)
    {
        const float mid = x + w * 0.5f;
        const SDL_FColor dim{ 214 / 255.f, 168 / 255.f, 82 / 255.f, 0.f };
        const SDL_FColor hi{ 240 / 255.f, 194 / 255.f, 102 / 255.f, 0.75f };
        const float y0 = y * s, y1 = (y + 1.5f) * s;
        const SDL_Vertex vl[4] = { { { x * s, y0 }, dim, {} }, { { mid * s, y0 }, hi, {} },
                                   { { mid * s, y1 }, hi, {} }, { { x * s, y1 }, dim, {} } };
        const SDL_Vertex vr[4] = { { { mid * s, y0 }, hi, {} }, { { (x + w) * s, y0 }, dim, {} },
                                   { { (x + w) * s, y1 }, dim, {} }, { { mid * s, y1 }, hi, {} } };
        const int idx[6] = { 0, 1, 2, 0, 2, 3 };
        SDL_RenderGeometry(renderer, nullptr, vl, 4, idx, 6);
        SDL_RenderGeometry(renderer, nullptr, vr, 4, idx, 6);
        const float d = 4.2f * s, ym = (y + 0.75f) * s;
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
        round_rect({ cx - 14, cy - 14, 28, 28 }, 14, { c.r, c.g, c.b, 34 });
        round_rect({ cx - 10, cy - 10, 20, 20 }, 10, c);
        if (st == item_state::ok) {
            line(cx - 4.6f, cy + 0.4f, cx - 1.0f, cy + 3.8f, 2.3f, c_bg_top);
            line(cx - 1.0f, cy + 3.8f, cx + 5.0f, cy - 3.2f, 2.3f, c_bg_top);
        } else if (st == item_state::bad) {
            line(cx - 3.8f, cy - 3.8f, cx + 3.8f, cy + 3.8f, 2.3f, c_bg_top);
            line(cx + 3.8f, cy - 3.8f, cx - 3.8f, cy + 3.8f, 2.3f, c_bg_top);
        } else if (st == item_state::warn) {
            line(cx, cy - 4.8f, cx, cy + 1.0f, 2.3f, c_bg_top);
            round_rect({ cx - 1.4f, cy + 3.0f, 2.8f, 2.8f }, 1.4f, c_bg_top);
        } else {
            line(cx - 4.0f, cy, cx + 4.0f, cy, 2.3f, c_bg_top);
        }
    }

    SDL_FRect row_rect(int i) const { return { k_pad + 6.f, k_panel_y + 6.f + i * k_row_h, k_w - 2 * k_pad - 12.f, k_row_h }; }

    void layout()
    {
        buttons.clear();
        const bool busy = job.phase == 1 || job.phase == 2;

        // Clean Add Files / Add Folder buttons right-aligned on the hero bottom bar
        const float bw_folder = 112.f, bw_files = 104.f;
        const float by = k_hero_h - 42.f;
        buttons.push_back({ { k_w - k_pad - bw_folder - 8.f - bw_files, by, bw_files, 32.f }, "Add Files", a_files, !busy, false, true });
        buttons.push_back({ { k_w - k_pad - bw_folder, by, bw_folder, 32.f }, "Add Folder", a_folder, !busy, false, true });

        auto add_row_btn = [&](int row, const char* label, action act, bool gold) {
            const SDL_FRect r = row_rect(row);
            const float w = std::max(90.f, measure(small, label) + 28.f);
            buttons.push_back({ { r.x + r.w - 14.f - w, r.y + (r.h - 30.f) * 0.5f, w, 30.f }, label, act, !busy, false, gold });
        };

        if (check.game.state != item_state::ok) add_row_btn(0, "Browse", a_folder, false);
        if (check.update.state != item_state::ok) add_row_btn(1, "Browse", a_files, false);
        if (!paths.dlcs.empty()) add_row_btn(2, "Clear", a_dlc_clear, false);
        add_row_btn(4, check.firmware.state == item_state::ok ? "Reinstall" : "Download", a_download, check.firmware.state != item_state::ok);

        const bool ready = check.ready() && !busy;
        buttons.push_back({ { k_pad, k_h - 86.f, k_w - 2 * k_pad, 50.f },
                            ready ? "PLAY" : "COMPLETE SETUP TO PLAY",
                            a_play, ready, true, false });
        focus = std::clamp(focus, 0, (int)buttons.size() - 1);
    }

    void draw_row(int i, const char* name, const char* hint, const item_status& st)
    {
        const SDL_FRect r = row_rect(i);
        if (hit(r, mouse_x, mouse_y))
            round_rect({ r.x + 2, r.y + 3, r.w - 4, r.h - 6 }, 8, c_row_hover);

        // Hairline gold divider between rows
        if (i > 0)
            round_rect({ r.x + 14.f, r.y, r.w - 28.f, 1.f }, 0, c_divider);

        status_icon(r.x + 28.f, r.y + r.h * 0.5f, st.state);
        text(heading, name, r.x + 56.f, r.y + 12.f, c_text);

        float right = r.x + r.w - 16.f;
        for (const button& b : buttons)
            if (b.r.y >= r.y && b.r.y + b.r.h <= r.y + r.h) right = std::min(right, b.r.x - 16.f);
        const float width = std::max(80.f, right - (r.x + 56.f));

        if (i == 4 && (job.phase == 1 || job.phase == 2)) {
            const float p = std::clamp(job.progress.load(), 0.f, 1.f);
            round_rect({ r.x + 56.f, r.y + 40.f, width, 7.f }, 3.5f, { 42, 30, 28, 255 });
            if (p > 0.f)
                round_rect_grad({ r.x + 56.f, r.y + 40.f, std::max(8.f, width * p), 7.f }, 3.5f, c_gold, c_crimson_hot_top);
            const std::string label = std::string(job.phase == 1 ? "Downloading " : "Installing ") + std::to_string((int)(p * 100)) + "%";
            text(small, label, r.x + 56.f + width - measure(small, label), r.y + 14.f, c_gold);
            return;
        }
        const bool failed = i == 4 && job.phase == 3;
        const std::string detail = failed ? job_error : (st.state == item_state::missing ? std::string(hint) : st.text);
        const rgba c = failed || st.state == item_state::bad ? c_bad : st.state == item_state::warn ? c_warn : st.state == item_state::ok ? c_sub : c_dim;
        text(body, fit(body, detail, width), r.x + 56.f, r.y + 35.f, c);
    }

    void draw_button(const button& b, bool focused, float t)
    {
        const bool hot = b.enabled && hit(b.r, mouse_x, mouse_y);
        const bool down = hot && pressed;

        if (b.accent && b.enabled) {
            const float pulse = 0.5f + 0.5f * std::sin(t * 2.6f);
            round_rect({ b.r.x - 4, b.r.y - 4, b.r.w + 8, b.r.h + 8 }, 13, { 226, 68, 28, (uint8_t)(38 + pulse * 46) });
        }
        if (focused && b.enabled) {
            round_rect({ b.r.x - 2.5f, b.r.y - 2.5f, b.r.w + 5, b.r.h + 5 }, 11, c_gold);
        } else if (b.gold_outline && b.enabled) {
            round_rect({ b.r.x - 1, b.r.y - 1, b.r.w + 2, b.r.h + 2 }, 9, hot ? c_gold : c_gold_dim);
        } else if (b.accent && b.enabled) {
            round_rect({ b.r.x - 1, b.r.y - 1, b.r.w + 2, b.r.h + 2 }, 10, { 246, 184, 92, 190 });
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

        const face& f = b.accent ? (b.enabled ? play_font : small) : small;
        const float tw = measure(f, b.label);
        const rgba tc = b.enabled ? (b.accent ? c_gold_bright : c_text) : c_dim;
        const float tx = b.r.x + (b.r.w - tw) * 0.5f;
        const float ty = b.r.y + (b.r.h - f.size / s) * 0.5f - 1.f;
        if (b.accent && b.enabled) text_shadow(f, b.label, tx, ty, tc);
        else text(f, b.label, tx, ty, tc);
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
    SDL_Texture* art_tex = nullptr;
    int art_w = 0, art_h = 0;
    firmware_job job;
    font regular, semibold, black;
    face hero_title, play_font, heading, body, small, micro;
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
