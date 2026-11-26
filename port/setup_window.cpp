#include "setup_window.h"

#include "firmware.h"
#include "font.h"
#include "launcher_art.h"
#include "raster.h"
#include "../ps3recomp/libs/codec/stb_image.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>

namespace {

constexpr float k_w = 940, k_h = 690, k_pad = 40;
constexpr float k_hero_h = 186;
constexpr float k_panel_y = 202, k_row_h = 74;
constexpr float k_pi = 3.14159265359f;

struct rgba {
    uint8_t r, g, b, a = 255;
};

rgba lerp_c(rgba a, rgba b, float t)
{
    t = std::clamp(t, 0.f, 1.f);
    return {
        (uint8_t)std::lround(a.r + (b.r - a.r) * t),
        (uint8_t)std::lround(a.g + (b.g - a.g) * t),
        (uint8_t)std::lround(a.b + (b.b - a.b) * t),
        (uint8_t)std::lround(a.a + (b.a - a.a) * t),
    };
}

constexpr rgba c_bg_top{ 8, 6, 7 }, c_bg_bot{ 28, 9, 9 };
constexpr rgba c_panel{ 15, 11, 12, 238 }, c_panel_border{ 156, 86, 56, 95 };
constexpr rgba c_row_hover{ 46, 22, 22, 125 };
constexpr rgba c_divider{ 210, 120, 74, 28 };

constexpr rgba c_text{ 248, 243, 238 }, c_sub{ 202, 186, 178 }, c_dim{ 134, 118, 112 };
constexpr rgba c_gold{ 244, 194, 102 }, c_gold_bright{ 255, 230, 172 };

constexpr rgba c_play_base{ 182, 30, 22 }, c_play_hot{ 226, 52, 30 }, c_play_down{ 130, 18, 16 };
constexpr rgba c_btn_base{ 28, 20, 22, 234 }, c_btn_hot{ 56, 28, 26, 248 }, c_btn_down{ 20, 14, 15, 250 };
constexpr rgba c_gold_base{ 56, 30, 18, 238 }, c_gold_hot{ 88, 46, 24, 250 };
constexpr rgba c_disabled{ 24, 18, 19, 215 };

constexpr rgba c_ok{ 98, 214, 122 }, c_warn{ 246, 188, 76 }, c_bad{ 242, 96, 82 }, c_missing{ 138, 122, 114 };

enum action { a_game_folder, a_game_iso, a_update_download, a_dlc_add, a_dlc_clear, a_rap_add, a_download, a_play };

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

struct btn_anim {
    float hover = 0.f;
    float press = 0.f;
    float sweep = -1.f;
};

struct ember {
    float x, y, vy, size, phase, speed, alpha;
};

struct bg_job {
    std::thread worker;
    std::atomic<int> row{ -1 };
    std::atomic<int> phase{ 0 };
    std::atomic<float> progress{ 0 };
    std::atomic<bool> cancel{ false };
    std::mutex lock;
    std::string error;
    std::filesystem::path result;
};

struct pick_state {
    std::mutex lock;
    bool alive = true;
    std::vector<std::pair<std::filesystem::path, game_add_mode>> items;
};

struct pick_req {
    std::shared_ptr<pick_state> state;
    game_add_mode mode = game_add_mode::any;
};

float sd_seg(float px, float py, float ax, float ay, float bx, float by)
{
    const float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    const float h = std::clamp((pax * bax + pay * bay) / std::max(1e-6f, bax * bax + bay * bay), 0.f, 1.f);
    return std::hypot(pax - bax * h, pay - bay * h);
}

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
            em.size = 1.6f + rng() * 2.4f;
            em.phase = rng() * k_pi * 2.f;
            em.speed = 0.7f + rng() * 1.2f;
            em.alpha = 0.22f + rng() * 0.48f;
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

        cur_default = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
        cur_pointer = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);

        if (!icon.empty()) {
            if (SDL_Surface* image = SDL_LoadPNG(utf8_from_path(icon).c_str())) {
                SDL_SetWindowIcon(window, image);
                SDL_DestroySurface(image);
            }
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
        s = pw > 0 ? (float)pw / k_w : ds;

        bake(hero_title, black, 46);
        bake(hero_sub, semibold, 14.5f);
        bake(play_font, black, 20);
        bake(heading, semibold, 18);
        bake(body, regular, 15.5f);
        bake(btn_font, semibold, 15);

        bake_aa_assets();

        t0 = std::chrono::steady_clock::now();
        last_t = t0;
        return true;
    }

    ~setup_ui()
    {
        {
            std::lock_guard<std::mutex> hold(picks->lock);
            picks->alive = false;
        }
        job.cancel.store(true, std::memory_order_relaxed);
        if (job.worker.joinable()) job.worker.join();
        for (SDL_Gamepad* g : gamepads) SDL_CloseGamepad(g);
        gamepads.clear();
        if (cur_pointer) SDL_DestroyCursor(cur_pointer);
        if (cur_default) SDL_DestroyCursor(cur_default);
        if (art_tex) SDL_DestroyTexture(art_tex);
        for (SDL_Texture* t : { pill_fill, pill_border, pill_shadow, glow_dot })
            if (t) SDL_DestroyTexture(t);
        for (SDL_Texture* t : status_tex)
            if (t) SDL_DestroyTexture(t);
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
    void bake_aa_assets()
    {
        auto make_tex = [&](int w, int h, const std::vector<uint8_t>& px) -> SDL_Texture* {
            SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
            if (tex) {
                SDL_UpdateTexture(tex, nullptr, px.data(), w * 4);
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
            }
            return tex;
        };

        {
            pill_r_px = std::max(8, (int)std::lround(10.f * s));
            pill_corner = (float)pill_r_px + 2.f;
            pill_sz = pill_r_px * 2 + 12;
            const float c = pill_sz * 0.5f, half = pill_sz * 0.5f - 1.f, r = (float)pill_r_px;
            const float stroke = std::max(1.0f, 1.15f * s);
            std::vector<uint8_t> fill_px((size_t)pill_sz * pill_sz * 4, 255);
            std::vector<uint8_t> border_px((size_t)pill_sz * pill_sz * 4, 255);
            for (int y = 0; y < pill_sz; y++) {
                for (int x = 0; x < pill_sz; x++) {
                    const float a_in = area([&](float u, float v) { return sd_round_rect(u, v, c, c, half, half, r); }, x + 0.5f, y + 0.5f, 4);
                    const float a_inner = area([&](float u, float v) {
                        return sd_round_rect(u, v, c, c, half - stroke, half - stroke, std::max(1.f, r - stroke));
                    }, x + 0.5f, y + 0.5f, 4);
                    fill_px[((size_t)y * pill_sz + x) * 4 + 3] = (uint8_t)std::lround(a_in * 255.f);
                    border_px[((size_t)y * pill_sz + x) * 4 + 3] = (uint8_t)std::lround(std::max(0.f, a_in - a_inner) * 255.f);
                }
            }
            pill_fill = make_tex(pill_sz, pill_sz, fill_px);
            pill_border = make_tex(pill_sz, pill_sz, border_px);

            const float sigma = std::max(4.f, 5.5f * s);
            const int pad = (int)std::ceil(sigma * 3.8f);
            shadow_pad = (float)pad / s;
            shadow_sz = pad * 2 + pill_r_px * 2 + 12;
            shadow_corner = (float)(pad + pill_r_px + 2);
            const float sc = shadow_sz * 0.5f;
            const float shalf = sc - (float)pad;
            grid m = mask(shadow_sz, shadow_sz, [&](float u, float v) {
                return area([&](float x, float y) { return sd_round_rect(x, y, sc, sc, shalf, shalf, r); }, u, v, 2);
            });
            grid soft = blur(m, sigma);
            std::vector<uint8_t> sh_px((size_t)shadow_sz * shadow_sz * 4, 255);
            for (int i = 0; i < shadow_sz * shadow_sz; i++)
                sh_px[(size_t)i * 4 + 3] = (uint8_t)std::lround(std::clamp(soft.v[i], 0.f, 1.f) * 255.f);
            pill_shadow = make_tex(shadow_sz, shadow_sz, sh_px);
        }

        {
            constexpr int sz = 64;
            const float c = sz * 0.5f;
            std::vector<uint8_t> px((size_t)sz * sz * 4, 255);
            for (int y = 0; y < sz; y++)
                for (int x = 0; x < sz; x++) {
                    const float d = std::hypot(x + 0.5f - c, y + 0.5f - c) / c;
                    const float a = std::clamp(1.f - d, 0.f, 1.f);
                    px[((size_t)y * sz + x) * 4 + 3] = (uint8_t)std::lround(a * a * (3.f - 2.f * a) * 255.f);
                }
            glow_dot = make_tex(sz, sz, px);
        }

        {
            const int sz = std::max(44, (int)std::lround(40.f * s));
            const float c = sz * 0.5f;
            const float rad = sz * 0.30f;
            const rgba cols[4] = { c_ok, c_warn, c_bad, c_missing };
            for (int st = 0; st < 4; st++) {
                std::vector<uint8_t> px((size_t)sz * sz * 4, 0);
                const rgba col = cols[st];
                for (int y = 0; y < sz; y++) {
                    for (int x = 0; x < sz; x++) {
                        const float u = x + 0.5f, v = y + 0.5f;
                        const float dist = std::hypot(u - c, v - c);
                        const float halo = std::clamp(1.f - (dist - rad) / (sz * 0.20f), 0.f, 1.f);
                        float ga = (dist > rad ? halo * halo * 0.24f : 0.f);
                        const float disc = area([&](float px0, float py0) { return std::hypot(px0 - c, py0 - c) - rad; }, u, v, 4);
                        const float stroke = std::max(1.3f, sz * 0.038f);
                        float glyph = 0.f;
                        if (disc > 0.01f) {
                            if (st == 0) {
                                glyph = area([&](float px0, float py0) {
                                    const float d1 = sd_seg(px0, py0, c - rad * 0.44f, c + rad * 0.04f, c - rad * 0.10f, c + rad * 0.38f);
                                    const float d2 = sd_seg(px0, py0, c - rad * 0.10f, c + rad * 0.38f, c + rad * 0.48f, c - rad * 0.32f);
                                    return std::min(d1, d2) - stroke;
                                }, u, v, 4);
                            } else if (st == 1) {
                                glyph = area([&](float px0, float py0) {
                                    const float d1 = sd_seg(px0, py0, c, c - rad * 0.46f, c, c + rad * 0.08f) - stroke;
                                    const float d2 = std::hypot(px0 - c, py0 - (c + rad * 0.38f)) - stroke * 1.05f;
                                    return std::min(d1, d2);
                                }, u, v, 4);
                            } else if (st == 2) {
                                glyph = area([&](float px0, float py0) {
                                    const float d1 = sd_seg(px0, py0, c - rad * 0.36f, c - rad * 0.36f, c + rad * 0.36f, c + rad * 0.36f);
                                    const float d2 = sd_seg(px0, py0, c + rad * 0.36f, c - rad * 0.36f, c - rad * 0.36f, c + rad * 0.36f);
                                    return std::min(d1, d2) - stroke;
                                }, u, v, 4);
                            } else {
                                glyph = area([&](float px0, float py0) {
                                    return sd_seg(px0, py0, c - rad * 0.38f, c, c + rad * 0.38f, c) - stroke;
                                }, u, v, 4);
                            }
                        }
                        const float fa = std::max(ga, disc);
                        if (fa <= 0.002f) continue;
                        const float k = glyph * disc;
                        const uint8_t r = (uint8_t)std::lround(col.r * (1.f - k) + 12.f * k);
                        const uint8_t g = (uint8_t)std::lround(col.g * (1.f - k) + 9.f * k);
                        const uint8_t b = (uint8_t)std::lround(col.b * (1.f - k) + 10.f * k);
                        uint8_t* o = &px[((size_t)y * sz + x) * 4];
                        o[0] = r; o[1] = g; o[2] = b;
                        o[3] = (uint8_t)std::lround(fa * 255.f);
                    }
                }
                status_tex[st] = make_tex(sz, sz, px);
            }
        }
    }

    void draw_9slice(SDL_Texture* tex, int tex_sz, float tex_corner, SDL_FRect r, float dst_corner, rgba c)
    {
        if (!tex) return;
        SDL_SetTextureColorMod(tex, c.r, c.g, c.b);
        SDL_SetTextureAlphaMod(tex, c.a);
        const float x = r.x * s, y = r.y * s, w = r.w * s, h = r.h * s;
        const float dc = std::min(dst_corner * s, std::min(w, h) * 0.5f);
        const float sx[4] = { 0.f, tex_corner, (float)tex_sz - tex_corner, (float)tex_sz };
        const float sy[4] = { 0.f, tex_corner, (float)tex_sz - tex_corner, (float)tex_sz };
        const float dx[4] = { x, x + dc, x + w - dc, x + w };
        const float dy[4] = { y, y + dc, y + h - dc, y + h };
        for (int iy = 0; iy < 3; iy++) {
            const float dh = dy[iy + 1] - dy[iy];
            if (dh <= 0.1f) continue;
            for (int ix = 0; ix < 3; ix++) {
                const float dw = dx[ix + 1] - dx[ix];
                if (dw <= 0.1f) continue;
                const SDL_FRect src{ sx[ix], sy[iy], sx[ix + 1] - sx[ix], sy[iy + 1] - sy[iy] };
                const SDL_FRect dst{ dx[ix], dy[iy], dw, dh };
                SDL_RenderTexture(renderer, tex, &src, &dst);
            }
        }
    }

    void aa_rect(SDL_FRect r, float radius, rgba c) { draw_9slice(pill_fill, pill_sz, pill_corner, r, radius, c); }
    void aa_border(SDL_FRect r, float radius, rgba c) { draw_9slice(pill_border, pill_sz, pill_corner, r, radius, c); }
    void aa_shadow(SDL_FRect r, float expand, float radius, rgba c)
    {
        const float p = std::max(expand, shadow_pad);
        draw_9slice(pill_shadow, shadow_sz, shadow_corner, { r.x - p, r.y - p, r.w + p * 2.f, r.h + p * 2.f }, radius + p, c);
    }

    void draw()
    {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(0.05f, std::chrono::duration<float>(now - last_t).count());
        const float t = std::chrono::duration<float>(now - t0).count();
        last_t = now;

        grad_rect({ 0, 0, k_w, k_h }, c_bg_top, c_bg_bot);

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
            SDL_SetTextureAlphaMod(art_tex, 218);
            SDL_RenderTexture(renderer, art_tex, &src, &dst);

            grad_rect({ 0, 0, k_w, 84 }, { 10, 7, 8, 140 }, { 10, 7, 8, 10 });
            grad_rect({ 0, 120, k_w, 222 }, { 13, 8, 9, 15 }, { 16, 8, 9, 255 });
            horiz_grad({ 0, 0, 480, k_hero_h + 40 }, { 10, 7, 8, 188 }, { 10, 7, 8, 0 });
        }

        radial_glow(k_w * 0.5f, k_h * 1.05f, k_w * 0.78f, k_h * 0.52f, { 196, 42, 18, 86 });
        radial_glow(k_w * 0.12f, k_h * 0.72f, k_w * 0.36f, k_h * 0.34f, { 132, 24, 16, 46 });
        radial_glow(k_w * 0.88f, k_h * 0.68f, k_w * 0.36f, k_h * 0.34f, { 148, 32, 16, 48 });
        if (glow_dot) {
            for (ember& em : embers) {
                em.y -= em.vy * dt;
                if (em.y < -12.f) {
                    em.y = k_h + 12.f;
                    em.x = std::fmod(em.x + 173.f, k_w);
                }
                const float ex = em.x + std::sin(t * em.speed + em.phase) * 14.f;
                const float fade = std::clamp(std::min(em.y / 130.f, (k_h - em.y) / 80.f), 0.f, 1.f);
                const uint8_t a = (uint8_t)(em.alpha * fade * 255.f);
                if (a > 3) {
                    const float gsz = em.size * 5.2f * s;
                    SDL_SetTextureColorMod(glow_dot, 240, 120, 36);
                    SDL_SetTextureAlphaMod(glow_dot, (uint8_t)(a * 0.45f));
                    const SDL_FRect gd{ ex * s - gsz * 0.5f, em.y * s - gsz * 0.5f, gsz, gsz };
                    SDL_RenderTexture(renderer, glow_dot, nullptr, &gd);
                    const float csz = em.size * 1.6f * s;
                    SDL_SetTextureColorMod(glow_dot, 255, 224, 148);
                    SDL_SetTextureAlphaMod(glow_dot, a);
                    const SDL_FRect cd{ ex * s - csz * 0.5f, em.y * s - csz * 0.5f, csz, csz };
                    SDL_RenderTexture(renderer, glow_dot, nullptr, &cd);
                }
            }
        }

        layout();

        bool over_interactive = false;
        for (const button& b : buttons)
            if (b.enabled && hit(b.r, mouse_x, mouse_y)) over_interactive = true;
        if (over_interactive != is_pointer) {
            is_pointer = over_interactive;
            if (cur_pointer && cur_default) SDL_SetCursor(is_pointer ? cur_pointer : cur_default);
        }

        text_shadow(hero_title, "VAJRA", k_pad, 42.f, c_gold_bright, 3.0f);
        text_shadow(hero_sub, "ASURA'S WRATH", k_pad + 2.f, 96.f, c_gold, 0.8f);
        text_shadow(body, "Select your game files below, or drag and drop them anywhere.", k_pad, k_hero_h - 36.f, c_sub);

        const float panel_h = 5.f * k_row_h + 16.f;
        const SDL_FRect pr{ k_pad, k_panel_y, k_w - 2 * k_pad, panel_h };
        aa_shadow({ pr.x, pr.y + 4.f, pr.w, pr.h }, 14.f, 14.f, { 0, 0, 0, 170 });
        aa_shadow(pr, 10.f, 14.f, { 176, 44, 22, 42 });
        aa_rect(pr, 14.f, c_panel);
        aa_border(pr, 14.f, c_panel_border);

        const char* names[] = { "Game", "Update 1.02", "DLC", "Licenses", "Firmware" };
        const char* hints[] = {
            "BLES01227 (EU) or BLUS30721 (US) disc folder or decrypted ISO",
            "Official 1.02 patch (automatic Sony download)",
            "Optional DLC .pkg packages",
            "Optional .rap keys for DLC",
            "PS3 audio codecs (automatic Sony download)",
        };
        const item_status* items[] = { &check.game, &check.update, &check.dlc, &check.licences, &check.firmware };
        for (int i = 0; i < 5; i++) draw_row(i, names[i], hints[i], *items[i], dt);

        for (size_t i = 0; i < buttons.size(); i++) draw_button(buttons[i], kbd_nav && (int)i == focus, dt, t);

        const float drop_target = dragging ? 1.f : 0.f;
        drop_alpha += (drop_target - drop_alpha) * std::min(1.f, dt * 14.f);
        if (drop_alpha > 0.01f) {
            grad_rect({ 0, 0, k_w, k_h }, { 10, 6, 7, (uint8_t)(drop_alpha * 185.f) }, { 24, 9, 9, (uint8_t)(drop_alpha * 205.f) });
            const SDL_FRect dr{ 56.f, 56.f, k_w - 112.f, k_h - 112.f };
            aa_shadow(dr, 18.f, 18.f, { 242, 188, 86, (uint8_t)(drop_alpha * 70.f) });
            aa_border(dr, 18.f, { 244, 200, 112, (uint8_t)(drop_alpha * 235.f) });
            const std::string msg = "Drop files to add to Vajra";
            text_shadow(play_font, msg, (k_w - measure(play_font, msg)) * 0.5f, k_h * 0.5f - 14.f,
                        { 255, 236, 184, (uint8_t)(drop_alpha * 255.f) });
        }

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
        if (!f.tex) return;
        SDL_UpdateTexture(f.tex, nullptr, px.data(), f.aw * 4);
        SDL_SetTextureBlendMode(f.tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(f.tex, SDL_SCALEMODE_LINEAR);
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
            const SDL_FRect dst{ q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0 };
            SDL_RenderTexture(renderer, f.tex, &src, &dst);
            px += track * s;
        }
    }

    void text_shadow(const face& f, const std::string& str, float x, float y, rgba c, float track = 0.f)
    {
        text(f, str, x + 1.f, y + 1.5f, { 0, 0, 0, (uint8_t)(c.a * 0.78f) }, track);
        text(f, str, x, y, c, track);
    }

    static SDL_FColor fcol(rgba c) { return { c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f }; }

    void quad_grad(SDL_FRect r, rgba tl, rgba tr, rgba br, rgba bl)
    {
        r = { r.x * s, r.y * s, r.w * s, r.h * s };
        const SDL_Vertex v[4] = {
            { { r.x, r.y }, fcol(tl), {} },
            { { r.x + r.w, r.y }, fcol(tr), {} },
            { { r.x + r.w, r.y + r.h }, fcol(br), {} },
            { { r.x, r.y + r.h }, fcol(bl), {} },
        };
        const int idx[6] = { 0, 1, 2, 0, 2, 3 };
        SDL_RenderGeometry(renderer, nullptr, v, 4, idx, 6);
    }

    void grad_rect(SDL_FRect r, rgba top, rgba bot) { quad_grad(r, top, top, bot, bot); }
    void horiz_grad(SDL_FRect r, rgba left, rgba right) { quad_grad(r, left, right, right, left); }

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

        add_row_btns(0, { { "Select Folder", a_game_folder, btn_style::secondary }, { "Select ISO", a_game_iso, btn_style::secondary } });
        add_row_btns(1, { { check.update.state == item_state::ok ? "Redownload" : "Download", a_update_download,
                            check.update.state == item_state::ok ? btn_style::secondary : btn_style::gold } });
        if (paths.dlcs.empty()) add_row_btns(2, { { "Add DLC", a_dlc_add, btn_style::secondary } });
        else add_row_btns(2, { { "Add DLC", a_dlc_add, btn_style::secondary }, { "Clear", a_dlc_clear, btn_style::secondary } });
        add_row_btns(3, { { "Add .rap", a_rap_add, check.licences.state == item_state::warn ? btn_style::gold : btn_style::secondary } });
        add_row_btns(4, { { check.firmware.state == item_state::ok ? "Reinstall" : "Download", a_download,
                            check.firmware.state == item_state::ok ? btn_style::secondary : btn_style::gold } });

        const bool ready = check.ready() && !busy;
        buttons.push_back({ { k_pad, k_h - 78.f, k_w - 2 * k_pad, 54.f },
                            ready ? "Play" : "Complete required setup above to play",
                            a_play, ready, btn_style::play });
        focus = std::clamp(focus, 0, (int)buttons.size() - 1);
    }

    void draw_row(int i, const char* name, const char* hint, const item_status& st, float dt)
    {
        const SDL_FRect r = row_rect(i);
        const bool over = hit(r, mouse_x, mouse_y);
        row_hover[i] += ((over ? 1.f : 0.f) - row_hover[i]) * std::min(1.f, dt * 16.f);
        if (row_hover[i] > 0.01f)
            aa_rect({ r.x + 2, r.y + 4, r.w - 4, r.h - 8 }, 10.f, { c_row_hover.r, c_row_hover.g, c_row_hover.b, (uint8_t)(c_row_hover.a * row_hover[i]) });

        if (i > 0)
            grad_rect({ r.x + 16.f, r.y, r.w - 32.f, 1.f }, c_divider, c_divider);

        const int idx = st.state == item_state::ok ? 0 : st.state == item_state::warn ? 1 : st.state == item_state::bad ? 2 : 3;
        if (SDL_Texture* itex = status_tex[idx]) {
            const float isz = 40.f * s;
            const SDL_FRect dst{ (r.x + 32.f) * s - isz * 0.5f, (r.y + r.h * 0.5f) * s - isz * 0.5f, isz, isz };
            SDL_RenderTexture(renderer, itex, nullptr, &dst);
        }

        text(heading, name, r.x + 64.f, r.y + 13.f, c_text);

        float right = r.x + r.w - 18.f;
        for (const button& b : buttons)
            if (b.r.y >= r.y && b.r.y + b.r.h <= r.y + r.h) right = std::min(right, b.r.x - 18.f);
        const float width = std::max(80.f, right - (r.x + 64.f));

        if (i == job.row.load() && (job.phase == 1 || job.phase == 2)) {
            const float p = std::clamp(job.progress.load(), 0.f, 1.f);
            aa_rect({ r.x + 64.f, r.y + 46.f, width, 8.f }, 4.f, { 42, 32, 30, 255 });
            if (p > 0.f) {
                const float fw = std::max(10.f, width * p);
                aa_shadow({ r.x + 64.f, r.y + 46.f, fw, 8.f }, 4.f, 4.f, { 242, 160, 54, 110 });
                aa_rect({ r.x + 64.f, r.y + 46.f, fw, 8.f }, 4.f, c_gold);
            }
            const std::string label = std::string(job.phase == 1 ? "Downloading " : "Installing ") + std::to_string((int)(p * 100)) + "%";
            text(body, label, r.x + 64.f + width - measure(body, label), r.y + 15.f, c_gold);
            return;
        }
        const bool failed = i == job.row.load() && job.phase == 3;
        const bool has_notice = (i == 2 && !dlc_notice.empty()) || (i == 3 && !rap_notice.empty());
        const std::string detail = failed ? job_error
                                 : i == 2 && !dlc_notice.empty() ? dlc_notice
                                 : i == 3 && !rap_notice.empty() ? rap_notice
                                 : (st.state == item_state::missing ? std::string(hint) : st.text);
        const rgba c = failed || has_notice || st.state == item_state::bad ? c_bad : st.state == item_state::warn ? c_warn : st.state == item_state::ok ? c_sub : c_dim;
        text(body, fit(body, detail, width), r.x + 64.f, r.y + 41.f, c);
    }

    void draw_button(const button& b, bool focused, float dt, float t)
    {
        const bool hot = b.enabled && hit(b.r, mouse_x, mouse_y);
        const bool down = hot && pressed;
        const bool is_play = b.style == btn_style::play;

        btn_anim& anim = anims[(size_t)b.id];
        const float rate = std::min(1.f, dt * 16.f);
        const float prev_h = anim.hover;
        anim.hover += ((hot ? 1.f : 0.f) - anim.hover) * rate;
        anim.press += ((down ? 1.f : 0.f) - anim.press) * std::min(1.f, dt * 24.f);
        if (hot && prev_h < 0.08f && anim.sweep < 0.f) anim.sweep = 0.f;
        if (anim.sweep >= 0.f) {
            anim.sweep += dt * 2.1f;
            if (anim.sweep > 1.35f) anim.sweep = -1.f;
        }

        const float lift = b.enabled ? (-1.4f * anim.hover + 2.4f * anim.press) : 0.f;
        const SDL_FRect r{ b.r.x, b.r.y + lift, b.r.w, b.r.h };
        const float rad = is_play ? 12.f : 9.f;

        if (b.enabled) {
            aa_shadow({ r.x, r.y + 3.f, r.w, r.h }, 7.f, rad, { 0, 0, 0, (uint8_t)(95 + anim.hover * 45) });
            if (is_play) {
                const float pulse = 0.5f + 0.5f * std::sin(t * 2.4f);
                aa_shadow(r, 11.f + anim.hover * 4.f, rad, { 232, 68, 28, (uint8_t)(60 + pulse * 45 + anim.hover * 65) });
            } else if (anim.hover > 0.01f) {
                aa_shadow(r, 8.f, rad, { 244, 192, 92, (uint8_t)(anim.hover * 72.f) });
            }
        }

        rgba fill = c_disabled;
        if (b.enabled) {
            if (is_play) fill = lerp_c(lerp_c(c_play_base, c_play_hot, anim.hover), c_play_down, anim.press);
            else if (b.style == btn_style::gold) fill = lerp_c(lerp_c(c_gold_base, c_gold_hot, anim.hover), c_btn_down, anim.press);
            else fill = lerp_c(lerp_c(c_btn_base, c_btn_hot, anim.hover), c_btn_down, anim.press);
        }
        aa_rect(r, rad, fill);

        if (b.enabled && glow_dot) {
            if (anim.hover > 0.01f) {
                const float mx = std::clamp(mouse_x, r.x + rad, r.x + r.w - rad);
                const float gw = std::min(r.w * 0.85f, 160.f) * s;
                const float gh = (r.h - 4.f) * s;
                SDL_SetTextureColorMod(glow_dot, 255, is_play ? 210 : 216, is_play ? 150 : 140);
                SDL_SetTextureAlphaMod(glow_dot, (uint8_t)(anim.hover * (is_play ? 55.f : 38.f)));
                const SDL_FRect gd{ mx * s - gw * 0.5f, (r.y + r.h * 0.5f) * s - gh * 0.5f, gw, gh };
                SDL_RenderTexture(renderer, glow_dot, nullptr, &gd);
            }
            float sp = anim.sweep;
            if (is_play && sp < 0.f) {
                const float cycle = std::fmod(t, 4.2f);
                if (cycle < 0.75f) sp = cycle / 0.75f;
            }
            if (sp >= 0.f && sp <= 1.f) {
                const float edge_fade = std::sin(sp * k_pi);
                const float sx = r.x + rad + sp * (r.w - rad * 2.f);
                const float sw = std::min(r.w * 0.55f, 120.f) * s;
                const float sh = (r.h - 4.f) * s;
                SDL_SetTextureColorMod(glow_dot, 255, 238, 198);
                SDL_SetTextureAlphaMod(glow_dot, (uint8_t)((is_play ? 52.f : 40.f) * edge_fade));
                const SDL_FRect sd{ sx * s - sw * 0.5f, (r.y + r.h * 0.5f) * s - sh * 0.5f, sw, sh };
                SDL_RenderTexture(renderer, glow_dot, nullptr, &sd);
            }
        }

        rgba border{ 72, 50, 48, 80 };
        if (b.enabled) {
            if (focused) border = c_gold_bright;
            else if (is_play) border = lerp_c({ 246, 152, 88, 165 }, { 255, 224, 166, 240 }, anim.hover);
            else if (b.style == btn_style::gold) border = lerp_c({ 238, 168, 78, 195 }, c_gold_bright, anim.hover);
            else border = lerp_c({ 148, 92, 68, 105 }, { 244, 156, 82, 230 }, anim.hover);
        }
        aa_border(r, rad, border);

        const face& f = is_play ? (b.enabled ? play_font : body) : btn_font;
        const float track = is_play && b.enabled ? 1.5f : 0.f;
        const float tw = measure(f, b.label, track);
        const rgba tc = !b.enabled ? c_dim : is_play ? c_text
                      : (b.style == btn_style::gold ? c_gold_bright : lerp_c(c_sub, c_text, anim.hover));
        const float tx = r.x + (r.w - tw) * 0.5f;
        const float ty = r.y + (r.h - f.size / s) * 0.5f - 1.f;
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
            kbd_nav = false;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            pressed = e.button.button == SDL_BUTTON_LEFT;
            kbd_nav = false;
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
        case SDL_EVENT_DROP_BEGIN:
            dragging = true;
            break;
        case SDL_EVENT_DROP_COMPLETE:
            dragging = false;
            break;
        case SDL_EVENT_DROP_FILE: {
            std::lock_guard<std::mutex> hold(picks->lock);
            picks->items.emplace_back(path_from_utf8(e.drop.data), game_add_mode::any);
            break;
        }
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_ESCAPE) return false;
            kbd_nav = true;
            if (e.key.key == SDLK_UP || e.key.key == SDLK_LEFT || (e.key.key == SDLK_TAB && (e.key.mod & SDL_KMOD_SHIFT))) move(-1);
            else if (e.key.key == SDLK_DOWN || e.key.key == SDLK_RIGHT || e.key.key == SDLK_TAB) move(1);
            else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_SPACE || e.key.key == SDLK_KP_ENTER) press_focus();
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (SDL_Gamepad* g = SDL_OpenGamepad(e.gdevice.which)) gamepads.push_back(g);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            for (auto it = gamepads.begin(); it != gamepads.end(); ++it) {
                if (SDL_GetGamepadID(*it) == e.gdevice.which) {
                    SDL_CloseGamepad(*it);
                    gamepads.erase(it);
                    break;
                }
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            kbd_nav = true;
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
        std::unique_ptr<pick_req> req(static_cast<pick_req*>(user));
        if (!req || !list || !*list) return;
        std::lock_guard<std::mutex> hold(req->state->lock);
        if (!req->state->alive) return;
        for (; *list; list++)
            req->state->items.emplace_back(path_from_utf8(*list), req->mode);
    }

    void activate(action id)
    {
        static const SDL_DialogFileFilter f_iso[] = { { "PS3 ISO Image", "iso;ISO" }, { "All Files", "*" } };
        static const SDL_DialogFileFilter f_dlc[] = { { "PS3 DLC / License", "pkg;PKG;rap;RAP" }, { "All Files", "*" } };
        static const SDL_DialogFileFilter f_rap[] = { { "PS3 License (.rap)", "rap;RAP" }, { "All Files", "*" } };
        switch (id) {
        case a_game_folder: SDL_ShowOpenFolderDialog(on_pick, new pick_req{ picks, game_add_mode::any }, window, nullptr, false); break;
        case a_game_iso: SDL_ShowOpenFileDialog(on_pick, new pick_req{ picks, game_add_mode::any }, window, f_iso, 2, nullptr, false); break;
        case a_update_download: start_update_job(); break;
        case a_dlc_add: SDL_ShowOpenFileDialog(on_pick, new pick_req{ picks, game_add_mode::dlc }, window, f_dlc, 2, nullptr, true); break;
        case a_dlc_clear:
            paths.dlcs.clear();
            dlc_notice.clear();
            rap_notice.clear();
            recheck();
            break;
        case a_rap_add: SDL_ShowOpenFileDialog(on_pick, new pick_req{ picks, game_add_mode::rap }, window, f_rap, 2, nullptr, true); break;
        case a_download: start_job({}); break;
        case a_play: play = true; break;
        }
    }

    void apply_picks()
    {
        std::vector<std::pair<std::filesystem::path, game_add_mode>> items;
        {
            std::lock_guard<std::mutex> hold(picks->lock);
            items.swap(picks->items);
        }
        if (items.empty()) return;
        dlc_notice.clear();
        rap_notice.clear();
        draw();
        std::filesystem::path pup;
        for (const auto& [item, mode] : items) {
            const game_add_result r = add_game_items(exe, paths, { item }, mode);
            if (!r.pup.empty()) pup = r.pup;
            if (!r.dlc_notice.empty()) dlc_notice = r.dlc_notice;
            if (!r.rap_notice.empty()) rap_notice = r.rap_notice;
        }
        if (!pup.empty() && job.phase != 1 && job.phase != 2) start_job(pup);
        recheck();
    }

    void recheck()
    {
        draw();
        check = check_game_paths(exe, paths);
    }

    void start_update_job()
    {
        if (job.worker.joinable()) job.worker.join();
        const std::string tid = check.title_id.empty() ? "BLES01227" : check.title_id;
        const std::filesystem::path pkg = default_update_pkg(exe, tid);
        job.row = 1;
        job.progress = 0;
        job.cancel = false;
        job.phase = 1;
        job.worker = std::thread([this, pkg, tid] {
            std::string error;
            const bool ok = update_download(pkg, job.progress, error, &job.cancel, tid);
            std::lock_guard<std::mutex> hold(job.lock);
            job.error = error;
            job.result = ok ? pkg : std::filesystem::path{};
            job.phase = ok ? 4 : 3;
        });
    }

    void start_job(std::filesystem::path pup)
    {
        if (job.worker.joinable()) job.worker.join();
        paths.firmware.clear();
        const std::filesystem::path flash = firmware_dir(exe, paths);
        const bool download = pup.empty();
        if (download) pup = exe / "user" / "PS3UPDAT.PUP";
        job.row = 4;
        job.progress = 0;
        job.cancel = false;
        job.phase = download ? 1 : 2;
        job.worker = std::thread([this, pup, flash, download] {
            std::string error;
            bool ok = !download || firmware_download(pup, job.progress, error, &job.cancel);
            if (ok) {
                job.progress = 0;
                job.phase = 2;
                ok = firmware_extract(pup, flash, job.progress, error, &job.cancel);
            }
            if (download) {
                std::error_code ec;
                std::filesystem::remove(pup, ec);
            }
            std::lock_guard<std::mutex> hold(job.lock);
            job.error = error;
            job.result.clear();
            job.phase = ok ? 4 : 3;
        });
    }

    void finish_job()
    {
        if (job.phase < 3 || !job.worker.joinable()) return;
        job.worker.join();
        const int target = job.row.load();
        const bool ok = job.phase == 4;
        std::filesystem::path result;
        {
            std::lock_guard<std::mutex> hold(job.lock);
            job_error = job.error;
            result = std::move(job.result);
        }
        if (ok) {
            job.phase = 0;
            if (target == 1 && !result.empty()) {
                paths.update = result;
                recheck();
            } else if (target == 4) {
                check.firmware = firmware_installed(firmware_dir(exe, paths)) ? item_status{ item_state::ok, "Installed" } : check.firmware;
            }
        }
    }

    std::filesystem::path exe;
    game_paths& paths;
    game_check check;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Cursor* cur_default = nullptr;
    SDL_Cursor* cur_pointer = nullptr;
    SDL_Texture* art_tex = nullptr;
    SDL_Texture* pill_fill = nullptr;
    SDL_Texture* pill_border = nullptr;
    SDL_Texture* pill_shadow = nullptr;
    SDL_Texture* glow_dot = nullptr;
    SDL_Texture* status_tex[4]{};
    int pill_r_px = 10, pill_sz = 32, shadow_sz = 96;
    float pill_corner = 12.f, shadow_corner = 32.f, shadow_pad = 18.f;
    int art_w = 0, art_h = 0;
    bg_job job;
    font regular, semibold, black;
    face hero_title, hero_sub, play_font, heading, body, btn_font;
    float s = 1, mouse_x = -1, mouse_y = -1;
    bool pressed = false, play = false, is_pointer = false, kbd_nav = false, dragging = false;
    float drop_alpha = 0.f;
    int focus = 0;
    std::vector<button> buttons;
    std::array<btn_anim, 8> anims{};
    float row_hover[5]{};
    std::string job_error, dlc_notice, rap_notice;
    std::shared_ptr<pick_state> picks = std::make_shared<pick_state>();
    std::vector<SDL_Gamepad*> gamepads;
    std::array<ember, 26> embers{};
    std::chrono::steady_clock::time_point t0, last_t;
};

}

bool run_setup(const std::filesystem::path& exe, const std::filesystem::path& icon, game_paths& paths, game_check check)
{
    setup_ui ui(exe, paths, std::move(check));
    return ui.open(icon) && ui.run();
}
