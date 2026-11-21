#include "setup_window.h"

#include "firmware.h"

#include <SDL3/SDL.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../ps3recomp/third_party/stb/stb_truetype.h"

#ifndef _WIN32
#include <dlfcn.h>
#include <fontconfig/fontconfig.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <mutex>
#include <thread>

namespace {

constexpr float k_w = 760, k_h = 640, k_pad = 28, k_card_h = 78, k_card_gap = 10, k_cards_y = 112;

struct rgba {
    uint8_t r, g, b, a = 255;
};

constexpr rgba c_bg{ 13, 10, 9 }, c_card{ 26, 21, 19 }, c_card_line{ 44, 34, 30 }, c_text{ 237, 230, 223 }, c_muted{ 156, 145, 138 },
    c_gold{ 214, 168, 82 }, c_red{ 178, 38, 30 }, c_red_hot{ 204, 52, 42 }, c_red_down{ 140, 28, 22 }, c_ok{ 96, 180, 106 },
    c_warn{ 226, 166, 60 }, c_bad{ 226, 88, 74 }, c_missing{ 112, 99, 92 }, c_button{ 44, 36, 33 }, c_button_hot{ 62, 50, 46 },
    c_disabled{ 38, 33, 31 }, c_focus{ 214, 168, 82 };

enum action { a_add, a_files, a_folder, a_drop, a_dlc_clear, a_download, a_play };

struct font {
    std::vector<uint8_t> ttf;
    stbtt_fontinfo info{};
};

struct face {
    SDL_Texture* tex = nullptr;
    std::array<stbtt_packedchar, 96 + 96 + 2> chars{};
    float size = 0, ascent = 0;
    int aw = 0, ah = 0;
};

int glyph_index(uint32_t cp)
{
    if (cp >= 32 && cp < 128) return (int)cp - 32;
    if (cp >= 160 && cp < 256) return 96 + (int)cp - 160;
    if (cp == 0x2013) return 192;
    if (cp == 0x2026) return 193;
    return '?' - 32;
}

uint32_t next_cp(const std::string& s, size_t& i)
{
    const uint8_t c = (uint8_t)s[i++];
    if (c < 0x80) return c;
    const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    uint32_t cp = c & (0x3F >> extra);
    for (int k = 0; k < extra && i < s.size(); k++) cp = cp << 6 | ((uint8_t)s[i++] & 0x3F);
    return cp;
}

bool read_font(font& f, const std::filesystem::path& file, int index)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    f.ttf.assign(std::istreambuf_iterator<char>(in), {});
    const int offset = stbtt_GetFontOffsetForIndex(f.ttf.data(), index);
    return offset >= 0 && stbtt_InitFont(&f.info, f.ttf.data(), offset);
}

#ifdef _WIN32
bool load_font(font& f, bool bold)
{
    const char* windir = std::getenv("WINDIR");
    const std::filesystem::path dir = std::filesystem::path(windir ? windir : "C:\\Windows") / "Fonts";
    for (const char* name : bold ? std::initializer_list<const char*>{ "seguisb.ttf", "segoeuib.ttf", "segoeui.ttf" } : std::initializer_list<const char*>{ "segoeui.ttf" })
        if (read_font(f, dir / name, 0)) return true;
    return false;
}
#else
bool load_font(font& f, bool bold)
{
    static void* const fc = dlopen("libfontconfig.so.1", RTLD_NOW);
    if (!fc) return false;
    const auto init = reinterpret_cast<decltype(&FcInitLoadConfigAndFonts)>(dlsym(fc, "FcInitLoadConfigAndFonts"));
    const auto parse = reinterpret_cast<decltype(&FcNameParse)>(dlsym(fc, "FcNameParse"));
    const auto substitute = reinterpret_cast<decltype(&FcConfigSubstitute)>(dlsym(fc, "FcConfigSubstitute"));
    const auto defaults = reinterpret_cast<decltype(&FcDefaultSubstitute)>(dlsym(fc, "FcDefaultSubstitute"));
    const auto match = reinterpret_cast<decltype(&FcFontMatch)>(dlsym(fc, "FcFontMatch"));
    const auto get_string = reinterpret_cast<decltype(&FcPatternGetString)>(dlsym(fc, "FcPatternGetString"));
    const auto get_integer = reinterpret_cast<decltype(&FcPatternGetInteger)>(dlsym(fc, "FcPatternGetInteger"));
    const auto destroy = reinterpret_cast<decltype(&FcPatternDestroy)>(dlsym(fc, "FcPatternDestroy"));
    if (!init || !parse || !substitute || !defaults || !match || !get_string || !get_integer || !destroy) return false;
    static FcConfig* const config = init();
    FcPattern* pattern = parse(reinterpret_cast<const FcChar8*>(bold ? "sans-serif:weight=180:variable=false" : "sans-serif:weight=80:variable=false"));
    if (!config || !pattern) return false;
    substitute(config, pattern, FcMatchPattern);
    defaults(pattern);
    FcResult result;
    FcPattern* found = match(config, pattern, &result);
    destroy(pattern);
    if (!found) return false;
    FcChar8* file = nullptr;
    int index = 0;
    get_integer(found, FC_INDEX, 0, &index);
    const bool ok = get_string(found, FC_FILE, 0, &file) == FcResultMatch && read_font(f, reinterpret_cast<const char*>(file), index & 0xFFFF);
    destroy(found);
    return ok;
}
#endif
struct button {
    SDL_FRect r;
    std::string label;
    action id;
    bool enabled, accent;
};

struct firmware_job {
    std::thread worker;
    std::atomic<int> phase{ 0 };
    std::atomic<float> progress{ 0 };
    std::mutex lock;
    std::string error;
};

struct picked {
    action id;
    std::vector<std::filesystem::path> files;
};

class setup_ui {
public:
    setup_ui(const std::filesystem::path& exe, game_paths& paths, game_check check) : exe(exe), paths(paths), check(std::move(check)) {}

    bool open(const std::filesystem::path& icon)
    {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return false;
        if (!load_font(regular, false) || !load_font(bold, true)) return false;
        const float ds = std::max(1.f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
        window = SDL_CreateWindow("Asura's Wrath", (int)(k_w * ds), (int)(k_h * ds), SDL_WINDOW_HIGH_PIXEL_DENSITY);
        renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
        if (!renderer) return false;
        if (SDL_Surface* image = icon.empty() ? nullptr : SDL_LoadPNG(icon.string().c_str())) {
            SDL_SetWindowIcon(window, image);
            SDL_DestroySurface(image);
        }
        SDL_SetRenderVSync(renderer, 1);
        int pw = 0, ph = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &pw, &ph);
        s = (float)pw / k_w;
        bake(title, bold, 30);
        bake(heading, bold, 16);
        bake(body, regular, 15);
        bake(small, regular, 14);
        return true;
    }

    ~setup_ui()
    {
        if (job.worker.joinable()) job.worker.join();
        for (face* f : { &title, &heading, &body, &small })
            if (f->tex) SDL_DestroyTexture(f->tex);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    bool run()
    {
        for (;;) {
            SDL_Event e;
            const bool busy = job.phase == 1 || job.phase == 2;
            if (SDL_WaitEventTimeout(&e, busy ? 50 : 500)) {
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
        set(c_bg);
        SDL_RenderClear(renderer);
        layout();
        text(title, "Asura's Wrath", k_pad, 30, c_gold);
        text(body, "Setup", k_pad, 72, c_muted);
        const char* names[] = { "Game", "Update", "DLC", "Licences", "Firmware" };
        const item_status* items[] = { &check.game, &check.update, &check.dlc, &check.licences, &check.firmware };
        for (int i = 0; i < 5; i++) card(i, names[i], *items[i]);
        if (menu) {
            const SDL_FRect r = buttons[1].r;
            round_rect({ r.x - 8, r.y - 8, r.w + 16, 2 * r.h + 24 }, 12, c_card_line);
        }
        for (size_t i = 0; i < buttons.size(); i++) draw_button(buttons[i], (int)i == focus);
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
        int special[2] = { 0x2013, 0x2026 };
        ranges[2] = { f.size, 0, special, 2, f.chars.data() + 192, 0, 0 };
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

    void set(rgba c) { SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a); }

    void round_rect(SDL_FRect r, float radius, rgba c)
    {
        r = { r.x * s, r.y * s, r.w * s, r.h * s };
        radius = std::min(radius * s, std::min(r.w, r.h) / 2);
        const SDL_FColor col{ c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f };
        std::vector<SDL_Vertex> v{ { { r.x + r.w / 2, r.y + r.h / 2 }, col, {} } };
        const float cx[4] = { r.x + r.w - radius, r.x + radius, r.x + radius, r.x + r.w - radius };
        const float cy[4] = { r.y + radius, r.y + radius, r.y + r.h - radius, r.y + r.h - radius };
        for (int k = 0; k < 4; k++)
            for (int j = 0; j <= 8; j++) {
                const float a = (float)(k * 90 + j * 90 / 8) * 3.14159265f / 180;
                v.push_back({ { cx[k] + std::cos(a) * radius, cy[k] - std::sin(a) * radius }, col, {} });
            }
        std::vector<int> idx;
        for (int i = 1; i < (int)v.size(); i++) idx.insert(idx.end(), { 0, i, i + 1 < (int)v.size() ? i + 1 : 1 });
        SDL_RenderGeometry(renderer, nullptr, v.data(), (int)v.size(), idx.data(), (int)idx.size());
    }

    void line(float x0, float y0, float x1, float y1, float w, rgba c)
    {
        const float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy), nx = -dy / len * w / 2 * s, ny = dx / len * w / 2 * s;
        const SDL_FColor col{ c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f };
        const SDL_Vertex v[4] = { { { x0 * s + nx, y0 * s + ny }, col, {} }, { { x1 * s + nx, y1 * s + ny }, col, {} },
                                  { { x1 * s - nx, y1 * s - ny }, col, {} }, { { x0 * s - nx, y0 * s - ny }, col, {} } };
        const int idx[6] = { 0, 1, 2, 0, 2, 3 };
        SDL_RenderGeometry(renderer, nullptr, v, 4, idx, 6);
    }

    void icon(float cx, float cy, item_state st)
    {
        const rgba c = st == item_state::ok ? c_ok : st == item_state::warn ? c_warn : st == item_state::bad ? c_bad : c_missing;
        round_rect({ cx - 12, cy - 12, 24, 24 }, 12, c);
        if (st == item_state::ok) {
            line(cx - 5.5f, cy + 0.5f, cx - 1.5f, cy + 4.5f, 2.6f, c_bg);
            line(cx - 1.5f, cy + 4.5f, cx + 6, cy - 4, 2.6f, c_bg);
        } else if (st == item_state::bad) {
            line(cx - 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f, 2.6f, c_bg);
            line(cx + 4.5f, cy - 4.5f, cx - 4.5f, cy + 4.5f, 2.6f, c_bg);
        } else if (st == item_state::warn) {
            line(cx, cy - 6, cx, cy + 1.5f, 2.6f, c_bg);
            round_rect({ cx - 1.6f, cy + 3.6f, 3.2f, 3.2f }, 1.6f, c_bg);
        } else {
            line(cx - 5, cy, cx + 5, cy, 2.6f, c_bg);
        }
    }

    SDL_FRect card_rect(int i) const { return { k_pad, k_cards_y + i * (k_card_h + k_card_gap), k_w - 2 * k_pad, k_card_h }; }

    void layout()
    {
        buttons.clear();
        const bool busy = job.phase == 1 || job.phase == 2;
        auto add = [&](int row, std::initializer_list<std::pair<const char*, action>> list) {
            const SDL_FRect r = card_rect(row);
            float x = r.x + r.w - 18;
            for (auto it = std::rbegin(list); it != std::rend(list); ++it) {
                const float w = std::max(92.f, measure(body, it->first) + 32);
                x -= w;
                buttons.push_back({ { x, r.y + (r.h - 36) / 2, w, 36 }, it->first, it->second, !busy, false });
                x -= 8;
            }
            std::reverse(buttons.end() - (ptrdiff_t)list.size(), buttons.end());
        };
        const SDL_FRect add_r{ k_w - k_pad - 120, 38, 120, 40 };
        buttons.push_back({ add_r, "Add", a_add, !busy, false });
        if (menu) {
            buttons.push_back({ { add_r.x, add_r.y + 48, add_r.w, 36 }, "Files", a_files, !busy, false });
            buttons.push_back({ { add_r.x, add_r.y + 92, add_r.w, 36 }, "Folder", a_folder, !busy, false });
        }
        if (!paths.dlcs.empty()) add(2, { { "Clear", a_dlc_clear } });
        add(4, { { "Download", a_download } });
        buttons.push_back({ { k_pad, k_h - k_pad - 52, k_w - 2 * k_pad, 52 }, "Play", a_play, check.ready() && !busy, true });
        focus = std::clamp(focus, 0, (int)buttons.size() - 1);
    }

    void card(int i, const char* name, const item_status& st)
    {
        const SDL_FRect r = card_rect(i);
        round_rect(r, 10, c_card_line);
        round_rect({ r.x + 1, r.y + 1, r.w - 2, r.h - 2 }, 9, c_card);
        icon(r.x + 30, r.y + r.h / 2, st.state);
        text(heading, name, r.x + 56, r.y + 15, c_text);
        float right = r.x + r.w - 18;
        for (const button& b : buttons)
            if (b.r.y > r.y && b.r.y < r.y + r.h) right = std::min(right, b.r.x - 16);
        const float width = right - (r.x + 56);
        if (i == 4 && (job.phase == 1 || job.phase == 2)) {
            const float p = std::clamp(job.progress.load(), 0.f, 1.f);
            round_rect({ r.x + 56, r.y + 49, width, 8 }, 4, c_button);
            if (p > 0) round_rect({ r.x + 56, r.y + 49, std::max(8.f, width * p), 8 }, 4, c_red_hot);
            const std::string label = std::string(job.phase == 1 ? "Downloading " : "Installing ") + std::to_string((int)(p * 100)) + "%";
            text(small, label, r.x + 56 + width - measure(small, label), r.y + 15, c_muted);
            return;
        }
        const bool failed = i == 4 && job.phase == 3;
        const rgba c = failed || st.state == item_state::bad ? c_bad : st.state == item_state::warn ? c_warn : c_muted;
        text(body, fit(body, failed ? job_error : st.text, width), r.x + 56, r.y + 43, c);
    }

    void draw_button(const button& b, bool focused)
    {
        const bool hot = b.enabled && hit(b.r, mouse_x, mouse_y);
        const bool down = hot && pressed;
        rgba fill = b.accent ? (down ? c_red_down : hot ? c_red_hot : c_red) : (down ? c_card : hot ? c_button_hot : c_button);
        if (!b.enabled) fill = c_disabled;
        if (focused) round_rect({ b.r.x - 3, b.r.y - 3, b.r.w + 6, b.r.h + 6 }, 10, c_focus);
        round_rect(b.r, 8, fill);
        const face& f = b.accent ? title : body;
        const std::string& label = b.label;
        const float tw = measure(f, label);
        text(f, label, b.r.x + (b.r.w - tw) / 2, b.r.y + (b.r.h - f.size / s) / 2 - (b.accent ? 2 : 1), b.enabled ? c_text : c_muted);
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
            const bool was_open = menu;
            menu = false;
            for (size_t i = 0; i < buttons.size(); i++)
                if (buttons[i].enabled && hit(buttons[i].r, e.button.x / s, e.button.y / s)) {
                    focus = (int)i;
                    if (buttons[i].id != a_add || !was_open) activate(buttons[i].id);
                }
            break;
        }
        case SDL_EVENT_DROP_FILE: {
            std::lock_guard<std::mutex> hold(picks_lock);
            picks.push_back({ a_drop, { std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(e.drop.data))) } });
            break;
        }
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_ESCAPE) {
                if (!menu) return false;
                menu = false;
            }
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
        auto* slot = static_cast<std::pair<setup_ui*, action>*>(user);
        if (!list || !*list) return;
        picked p{ slot->second, {} };
        for (; *list; list++) p.files.push_back(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(*list))));
        std::lock_guard<std::mutex> hold(slot->first->picks_lock);
        slot->first->picks.push_back(std::move(p));
    }

    void activate(action id)
    {
        static const SDL_DialogFileFilter files{ "PS3 files", "iso;pkg;rap;PUP;pup" };
        slots[id] = { this, id };
        switch (id) {
        case a_add: menu = !menu; break;
        case a_files: menu = false; SDL_ShowOpenFileDialog(on_pick, &slots[id], window, &files, 1, nullptr, true); break;
        case a_folder: menu = false; SDL_ShowOpenFolderDialog(on_pick, &slots[id], window, nullptr, true); break;
        case a_drop: break;
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
        std::vector<picked> list;
        {
            std::lock_guard<std::mutex> hold(picks_lock);
            list.swap(picks);
        }
        if (list.empty()) return;
        std::vector<std::filesystem::path> items;
        for (picked& p : list) items.insert(items.end(), p.files.begin(), p.files.end());
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
        const bool download = pup.empty();
        if (download) pup = exe / "user" / "PS3UPDAT.PUP";
        job.progress = 0;
        job.phase = download ? 1 : 2;
        job.worker = std::thread([this, pup, download] {
            std::string error;
            bool ok = !download || firmware_download(pup, job.progress, error);
            if (ok) {
                job.progress = 0;
                job.phase = 2;
                ok = firmware_extract(pup, firmware_dir(exe, paths), job.progress, error);
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
    firmware_job job;
    font regular, bold;
    face title, heading, body, small;
    float s = 1, mouse_x = -1, mouse_y = -1;
    bool pressed = false, play = false, menu = false;
    int focus = 0;
    std::vector<button> buttons;
    std::string job_error;
    std::mutex picks_lock;
    std::vector<picked> picks;
    std::array<std::pair<setup_ui*, action>, a_play + 1> slots{};
};

}

bool run_setup(const std::filesystem::path& exe, const std::filesystem::path& icon, game_paths& paths, game_check check)
{
    setup_ui ui(exe, paths, std::move(check));
    return ui.open(icon) && ui.run();
}
