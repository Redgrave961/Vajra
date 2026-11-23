#include "prompt_art.h"
#include "font.h"
#include "raster.h"

#include <array>
#include <cstring>
#include <functional>

namespace {

using field = std::function<float(float, float)>;

struct skin_def {
    float sy, depth, curve;
    rgb top, bottom, side_top, side_bottom, rim;
    float rim_lo, rim_hi, rim_a, grain;
    rgb glow;
    float glow_sigma, glow_a, glow_drop;
    rgb ink, ink_glow;
    float ink_sigma, ink_glow_a;
};

constexpr rgb c(int r, int g, int b)
{
    return { r / 255.f, g / 255.f, b / 255.f };
}

const skin_def k_skins[] = {
    { 1, 0, 1, c(63, 63, 63), c(56, 56, 56), {}, {}, c(224, 224, 224), -1.45f, 0, 1, 0, {}, 0, 0, 0, c(246, 246, 246), {}, 0, 0 },
    { 0.72f, 11, 2, c(14, 14, 14), c(150, 60, 40), c(41, 40, 41), c(60, 62, 60), c(246, 101, 49), 0, 2, 0.75f, 0.12f, c(246, 133, 0), 6.5f, 1.3f, 2,
      c(255, 255, 255), c(222, 137, 65), 3.2f, 1.35f },
    { 0.72f, 7, 1.3f, c(255, 12, 0), c(255, 222, 0), c(255, 4, 0), c(214, 0, 0), c(246, 160, 0), 0, 2, 0.8f, 0.1f, c(246, 152, 0), 7, 1.7f, 2,
      c(255, 255, 255), c(255, 70, 20), 2, 0.6f },
    { 1, 0, 2, c(16, 15, 14), c(156, 63, 43), {}, {}, c(238, 85, 82), 0, 2, 0.55f, 0.12f, {}, 0, 0, 0, c(255, 255, 255), c(222, 137, 65), 3.2f, 1.35f },
    { 1, 0, 1.2f, c(255, 4, 0), c(255, 236, 0), {}, {}, c(243, 82, 82), 0, 2, 0.55f, 0.1f, {}, 0, 0, 0, c(255, 255, 255), c(255, 70, 20), 2, 0.6f },
    { 1, 0, 1.6f, c(10, 10, 10), c(184, 62, 20), {}, {}, c(246, 95, 60), 0, 2, 0.8f, 0.12f, c(246, 127, 0), 5, 0.9f, 0, c(255, 255, 255), c(238, 125, 90),
      3, 1.35f },
    { 1, 0, 1.6f, c(255, 0, 0), c(255, 248, 0), {}, {}, c(246, 121, 0), 0, 2, 0.7f, 0.1f, c(246, 125, 0), 5, 0.9f, 0, c(255, 255, 255), c(255, 70, 20), 2,
      0.6f },
    { 1, 0, 1, {}, {}, {}, {}, c(255, 255, 255), -2.6f, 0, 1, 0, {}, 0, 0, 0, c(255, 255, 255), c(246, 52, 0), 2.6f, 1.5f },
    { 1, 0, 1, c(96, 96, 96), c(36, 36, 36), {}, {}, c(242, 242, 242), 0, 2.4f, 1, 0.03f, {}, 0, 0, 0, c(150, 150, 150), {}, 0, 0 },
    { 1, 0, 1, {}, {}, {}, {}, {}, 0, 0, 0, 0, {}, 0, 0, 0, c(240, 232, 176), c(20, 14, 6), 1.6f, 1.4f },
};

const font* load(font_weight weight)
{
    static font fonts[2];
    static const bool ok[2] = { load_font(fonts[0], font_weight::bold), load_font(fonts[1], font_weight::black) };
    const int i = weight == font_weight::black;
    return ok[i] ? &fonts[i] : nullptr;
}

struct painter {
    const font& f;
    float sy, cy;
    std::vector<field> body, ink, seam, faint;
    grid text_ink;

    painter(const font& f, float sy, float cy, int size) : f(f), sy(sy), cy(cy), text_ink(size, size, 0) {}

    template <class F> field face(F sd) const
    {
        return [sd, s = sy, c = cy](float x, float y) { return sd(x, c + (y - c) / s) * s; };
    }

    float units_width(const std::string& s, float& x0) const
    {
        float pen = 0, lo = 1e9f, hi = -1e9f;
        for (size_t i = 0; i < s.size();) {
            const uint32_t cp = next_cp(s, i);
            int adv, lsb, bx0, by0, bx1, by1;
            stbtt_GetCodepointHMetrics(&f.info, (int)cp, &adv, &lsb);
            if (stbtt_GetCodepointBox(&f.info, (int)cp, &bx0, &by0, &bx1, &by1)) {
                lo = std::min(lo, pen + bx0);
                hi = std::max(hi, pen + bx1);
            }
            size_t j = i;
            pen += adv + (j < s.size() ? stbtt_GetCodepointKernAdvance(&f.info, (int)cp, (int)next_cp(s, j)) : 0);
        }
        x0 = lo > hi ? 0 : lo;
        return lo > hi ? 0 : hi - lo;
    }

    float cap_units() const
    {
        int x0, y0, x1, y1;
        return stbtt_GetCodepointBox(&f.info, 'H', &x0, &y0, &x1, &y1) ? (float)y1 : 1.f;
    }

    float text_width(const std::string& s, float cap) const
    {
        float x0;
        return units_width(s, x0) * cap / cap_units();
    }

    void text(const std::string& s, float cx, float y, float cap, float max_w = 1e9f)
    {
        const float w = text_width(s, cap);
        if (w > max_w) cap *= max_w / w;
        float x0;
        const float scale = cap / cap_units(), width = units_width(s, x0) * scale;
        const float base = cy + (y - cy) * sy + cap * sy / 2;
        float pen = cx - width / 2 - x0 * scale;
        std::vector<uint8_t> bits;
        for (size_t i = 0; i < s.size();) {
            const uint32_t cp = next_cp(s, i);
            const float ix = std::floor(pen), iy = std::floor(base), fx = pen - ix, fy = base - iy;
            int bx0, by0, bx1, by1;
            stbtt_GetCodepointBitmapBoxSubpixel(&f.info, (int)cp, scale, scale * sy, fx, fy, &bx0, &by0, &bx1, &by1);
            const int bw = bx1 - bx0, bh = by1 - by0;
            if (bw > 0 && bh > 0) {
                bits.assign((size_t)bw * bh, 0);
                stbtt_MakeCodepointBitmapSubpixel(&f.info, bits.data(), bw, bh, bw, scale, scale * sy, fx, fy, (int)cp);
                for (int v = 0; v < bh; v++)
                    for (int u = 0; u < bw; u++) {
                        const int px = (int)ix + bx0 + u, py = (int)iy + by0 + v;
                        if (px >= 0 && py >= 0 && px < text_ink.w && py < text_ink.h)
                            text_ink.at(px, py) = std::max(text_ink.at(px, py), bits[(size_t)v * bw + u] / 255.f);
                    }
            }
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&f.info, (int)cp, &adv, &lsb);
            size_t j = i;
            pen += (adv + (j < s.size() ? stbtt_GetCodepointKernAdvance(&f.info, (int)cp, (int)next_cp(s, j)) : 0)) * scale;
        }
    }

    void arrow(float x, float y, float s, int dir)
    {
        static const float base[6] = { 0, -0.8f, -1, 0.55f, 1, 0.55f };
        std::array<float, 6> p;
        for (int i = 0; i < 3; i++) {
            const float u = base[2 * i] * s, v = base[2 * i + 1] * s;
            p[2 * i] = x + (dir == 0 ? u : dir == 2 ? -u : dir == 1 ? v : -v);
            p[2 * i + 1] = y + (dir == 0 ? v : dir == 2 ? -v : dir == 1 ? -u : u);
        }
        ink.push_back(face([p](float px, float py) { return sd_polygon(px, py, p.data(), 3); }));
    }

    void cap(float x, float y, float hw, float hh, float r)
    {
        body.push_back(face([=](float px, float py) { return sd_round_rect(px, py, x, y, hw, hh, r); }));
    }

    void key(const std::string& label, float x, float y, float min_w, float max_w, float h, bool bare)
    {
        const float pad = 0.2f * h, nominal = (bare ? 0.92f : 0.46f) * h;
        const float w = std::clamp(text_width(label, nominal) + 2 * pad, min_w, max_w);
        if (!bare) cap(x, y, w / 2, h / 2, 0.22f * h);
        text(label, x, y, nominal, w - 1.3f * pad);
    }

    void mouse(float x, float y, float m, int button, int wheel, bool plain = false)
    {
        const float hw = 0.34f * m, lw = std::max(0.9f, 0.05f * m), split = y + 0.02f * m;
        auto outer = [=](float px, float py) { return sd_round_rect(px, py, x, y, hw, 0.5f * m, 0.33f * m); };
        auto roller = [=](float px, float py) { return sd_round_rect(px, py, x, y - 0.24f * m, 0.055f * m, 0.1f * m, 0.055f * m); };
        body.push_back(face(outer));
        seam.push_back(face([=](float px, float py) {
            const float divider = py < split ? std::fabs(px - x) : 1e9f;
            const float line = std::max((plain ? divider : std::min(std::fabs(py - split), divider)) - lw / 2, outer(px, py));
            return plain ? line : std::min(line, roller(px, py));
        }));
        if (button == 1 || button == 3) {
            const float side = button == 1 ? -1.f : 1.f;
            ink.push_back(face([=](float px, float py) {
                return std::max({ outer(px, py) + 1.2f * lw, py - split + 1.1f * lw, side * (x - px) + 1.1f * lw, -roller(px, py) + 1.1f * lw });
            }));
        }
        if (button == 2 || wheel) ink.push_back(face([=](float px, float py) { return roller(px, py) + 0.15f * lw; }));
        if (button == 4 || button == 5)
            ink.push_back(face([=](float px, float py) {
                return sd_round_rect(px, py, x - hw - 1.3f * lw, y + (button == 4 ? 0.16f : -0.02f) * m, 0.8f * lw, 0.08f * m, 0.8f * lw);
            }));
        if (wheel) arrow(x + hw + 0.24f * m, y + wheel * 0.1f * m, 0.15f * m, wheel > 0 ? 2 : 0);
    }

    static bool single(const std::string& s)
    {
        size_t n = 0;
        if (!s.empty()) next_cp(s, n);
        return !s.empty() && n == s.size();
    }

    void cluster(const prompt_glyph& g, float x, float y, float s)
    {
        const float kw = 0.25f * s, kh = 0.31f * s;
        const float step_x = kw + 0.035f * s, step_y = kh + 0.05f * s;
        const float dx[4] = { 0, -step_x, 0, step_x }, dy[4] = { -step_y / 2, step_y / 2, step_y / 2, step_y / 2 };
        if (kw < 15) {
            const float h = 0.94f * s, letter = 0.26f * s, spread = 0.26f * s;
            cap(x, y, h / 2, h / 2, 0.22f * h);
            for (int i = 0; i < 4; i++) {
                const float lx = x + dx[i] / step_x * spread, ly = y + dy[i] / step_y * (spread + 0.04f * s);
                if (single(g.label[i]))
                    text(g.label[i], lx, ly, letter);
                else
                    arrow(lx, ly, 0.13f * s, i);
            }
            return;
        }
        const float r = 0.22f * kw;
        for (int i = 0; i < 4; i++) {
            const float kx = x + dx[i], ky = y + dy[i];
            const float ex = kw / 2 + 0.02f * s, ey = kh / 2 + 0.02f * s;
            cap(kx, ky, kw / 2, kh / 2, r);
            if (g.lit && !(g.lit >> i & 1)) faint.push_back(face([=](float px, float py) { return sd_round_rect(px, py, kx, ky, ex, ey, r); }));
            if (single(g.label[i]))
                text(g.label[i], kx, ky, 0.64f * kw);
            else
                arrow(kx, ky, 0.32f * kw, i);
        }
    }

    static float merged(const std::vector<field>& fs, float x, float y)
    {
        float d = 1e9f;
        for (const field& f : fs) d = std::min(d, f(x, y));
        return d;
    }
};

rgb tint_of(uint32_t t)
{
    return c((int)(t >> 16 & 255), (int)(t >> 8 & 255), (int)(t & 255));
}

struct block_fit {
    uint16_t c0, c1;
    uint32_t bits;
    float err;
};

void expand(uint16_t v, float* o)
{
    o[0] = (float)((v >> 11 << 3) | (v >> 13));
    o[1] = (float)(((v >> 5 & 63) << 2) | (v >> 9 & 3));
    o[2] = (float)(((v & 31) << 3) | (v >> 2 & 7));
}

uint16_t pack565(const float* v)
{
    const int r = std::clamp((int)std::lround(v[0] * 31 / 255), 0, 31), g = std::clamp((int)std::lround(v[1] * 63 / 255), 0, 63),
              b = std::clamp((int)std::lround(v[2] * 31 / 255), 0, 31);
    return (uint16_t)(r << 11 | g << 5 | b);
}

block_fit fit_indices(const float (*px)[3], const float* w, uint16_t c0, uint16_t c1)
{
    if (c0 < c1) std::swap(c0, c1);
    block_fit f{ c0, c1, 0, 0 };
    float pal[4][3];
    expand(c0, pal[0]);
    expand(c1, pal[1]);
    for (int ch = 0; ch < 3; ch++) {
        pal[2][ch] = (2 * pal[0][ch] + pal[1][ch]) / 3;
        pal[3][ch] = (pal[0][ch] + 2 * pal[1][ch]) / 3;
    }
    const int count = c0 == c1 ? 1 : 4;
    for (int i = 0; i < 16; i++) {
        int best = 0;
        float best_d = 1e30f;
        for (int k = 0; k < count; k++) {
            const float dr = pal[k][0] - px[i][0], dg = pal[k][1] - px[i][1], db = pal[k][2] - px[i][2];
            const float d = dr * dr + dg * dg + db * db;
            if (d < best_d) {
                best_d = d;
                best = k;
            }
        }
        f.bits |= (uint32_t)best << (2 * i);
        f.err += w[i] * best_d;
    }
    return f;
}

void encode_alpha(const uint8_t* a, uint8_t* out)
{
    auto build = [&](int a0, int a1, bool six, uint64_t& bits) {
        int pal[8] = { a0, a1 };
        if (six) {
            for (int k = 1; k < 5; k++) pal[k + 1] = ((5 - k) * a0 + k * a1) / 5;
            pal[6] = 0;
            pal[7] = 255;
        } else {
            for (int k = 1; k < 7; k++) pal[k + 1] = ((7 - k) * a0 + k * a1) / 7;
        }
        int err = 0;
        bits = 0;
        for (int i = 0; i < 16; i++) {
            int best = 0;
            for (int k = 1; k < 8; k++)
                if (std::abs(pal[k] - a[i]) < std::abs(pal[best] - a[i])) best = k;
            err += (pal[best] - a[i]) * (pal[best] - a[i]);
            bits |= (uint64_t)best << (3 * i);
        }
        return err;
    };
    int lo = 255, hi = 0, mid_lo = 255, mid_hi = 0;
    for (int i = 0; i < 16; i++) {
        lo = std::min(lo, (int)a[i]);
        hi = std::max(hi, (int)a[i]);
        if (a[i] != 0 && a[i] != 255) {
            mid_lo = std::min(mid_lo, (int)a[i]);
            mid_hi = std::max(mid_hi, (int)a[i]);
        }
    }
    uint64_t eight_bits = 0, six_bits = 0;
    const int eight = hi > lo ? build(hi, lo, false, eight_bits) : 0;
    const int six = mid_lo <= mid_hi && hi > lo ? build(mid_lo, mid_hi == mid_lo ? std::min(mid_lo + 1, 255) : mid_hi, true, six_bits) : 1 << 30;
    const bool use_six = six < eight;
    out[0] = (uint8_t)(use_six ? mid_lo : hi);
    out[1] = (uint8_t)(use_six ? (mid_hi == mid_lo ? std::min(mid_lo + 1, 255) : mid_hi) : lo);
    const uint64_t bits = use_six ? six_bits : eight_bits;
    for (int k = 0; k < 6; k++) out[2 + k] = (uint8_t)(bits >> (8 * k));
}

void decode_colors(const uint8_t* b, bool dxt1, float (*out)[3])
{
    const uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    const bool three = dxt1 && c0 <= c1;
    float pal[4][3];
    expand(c0, pal[0]);
    expand(c1, pal[1]);
    for (int ch = 0; ch < 3; ch++) {
        pal[2][ch] = three ? (pal[0][ch] + pal[1][ch]) / 2 : (2 * pal[0][ch] + pal[1][ch]) / 3;
        pal[3][ch] = three ? 0 : (pal[0][ch] + 2 * pal[1][ch]) / 3;
    }
    const uint32_t bits = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; i++) std::memcpy(out[i], pal[bits >> (2 * i) & 3], sizeof out[i]);
}
void encode_color_block(const uint8_t* src, uint8_t* out)
{
    float px[16][3], w[16], wsum = 0, mean[3] = {};
    for (int i = 0; i < 16; i++) {
        w[i] = src[i * 4 + 3] / 255.f + 0.02f;
        for (int ch = 0; ch < 3; ch++) {
            px[i][ch] = src[i * 4 + ch];
            mean[ch] += w[i] * px[i][ch];
        }
        wsum += w[i];
    }
    for (float& m : mean) m /= wsum;
    float cov[6] = {};
    for (int i = 0; i < 16; i++) {
        const float d[3] = { px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2] };
        cov[0] += w[i] * d[0] * d[0];
        cov[1] += w[i] * d[0] * d[1];
        cov[2] += w[i] * d[0] * d[2];
        cov[3] += w[i] * d[1] * d[1];
        cov[4] += w[i] * d[1] * d[2];
        cov[5] += w[i] * d[2] * d[2];
    }
    float axis[3] = { 0.577f, 0.577f, 0.577f };
    for (int it = 0; it < 8; it++) {
        const float n[3] = { cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2], cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                             cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2] };
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len < 1e-6f) break;
        for (int ch = 0; ch < 3; ch++) axis[ch] = n[ch] / len;
    }
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 16; i++) {
        if (w[i] < 0.03f) continue;
        const float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] + (px[i][2] - mean[2]) * axis[2];
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    if (lo > hi) lo = hi = 0;
    float e0[3], e1[3];
    for (int ch = 0; ch < 3; ch++) {
        e0[ch] = mean[ch] + axis[ch] * hi;
        e1[ch] = mean[ch] + axis[ch] * lo;
    }
    block_fit best = fit_indices(px, w, pack565(e0), pack565(e1));
    for (int it = 0; it < 3 && best.c0 != best.c1; it++) {
        static const float weight[4] = { 1, 0, 2 / 3.f, 1 / 3.f };
        float aa = 0, bb = 0, ab = 0, ax[3] = {}, bx[3] = {};
        for (int i = 0; i < 16; i++) {
            const float al = weight[best.bits >> (2 * i) & 3], be = 1 - al;
            aa += w[i] * al * al;
            bb += w[i] * be * be;
            ab += w[i] * al * be;
            for (int ch = 0; ch < 3; ch++) {
                ax[ch] += w[i] * al * px[i][ch];
                bx[ch] += w[i] * be * px[i][ch];
            }
        }
        const float det = aa * bb - ab * ab;
        if (std::fabs(det) < 1e-6f) break;
        for (int ch = 0; ch < 3; ch++) {
            e0[ch] = std::clamp((ax[ch] * bb - bx[ch] * ab) / det, 0.f, 255.f);
            e1[ch] = std::clamp((bx[ch] * aa - ax[ch] * ab) / det, 0.f, 255.f);
        }
        const block_fit next = fit_indices(px, w, pack565(e0), pack565(e1));
        if (next.err >= best.err) break;
        best = next;
    }
    out[0] = (uint8_t)best.c0;
    out[1] = (uint8_t)(best.c0 >> 8);
    out[2] = (uint8_t)best.c1;
    out[3] = (uint8_t)(best.c1 >> 8);
    std::memcpy(out + 4, &best.bits, 4);
}

void encode_block(const uint8_t* src, uint8_t* out)
{
    uint8_t a[16];
    for (int i = 0; i < 16; i++) a[i] = src[i * 4 + 3];
    encode_alpha(a, out);
    encode_color_block(src, out + 8);
}

}

bool prompt_render(prompt_skin skin, const prompt_frame& fr, const prompt_glyph& g, int size, uint8_t* rgba)
{
    const font* f = load(size >= 64 ? font_weight::black : font_weight::bold);
    if (!f) return false;
    const skin_def& s = k_skins[(int)skin];
    const bool bare = skin == prompt_skin::label || skin == prompt_skin::gauge, hud = skin == prompt_skin::hud;
    painter p(*f, s.sy, fr.cy, size);
    const float m = 0.94f * std::max(fr.h, fr.min_w);
    switch (g.kind) {
    case prompt_kind::key: p.key(g.label[0], fr.cx, fr.cy, fr.min_w, fr.max_w, fr.h, bare); break;
    case prompt_kind::face:
        if (!bare) p.body.push_back(p.face([&](float x, float y) { return std::hypot(x - fr.cx, y - fr.cy) - fr.h / 2; }));
        p.text(g.label[0], fr.cx, fr.cy, 0.48f * fr.h);
        break;
    case prompt_kind::mouse: p.mouse(fr.cx, fr.cy, m, g.button, 0); break;
    case prompt_kind::wheel_up: p.mouse(fr.cx - 0.17f * m, fr.cy, m, 0, -1); break;
    case prompt_kind::wheel_down: p.mouse(fr.cx - 0.17f * m, fr.cy, m, 0, 1); break;
    case prompt_kind::keys_wasd: p.cluster(g, fr.cx, fr.cy, fr.h); break;
    case prompt_kind::mouse_move: {
        const float h = fr.h, gap = std::max(1.2f, 0.05f * h);
        const bool compact = h < 64;
        const float mm = (compact ? 0.44f : 0.46f) * h, at = (compact ? 0.33f : 0.345f) * h, head = (compact ? 0.1f : 0.11f) * h;
        auto outline = [=](float x, float y) { return sd_round_rect(x, y, fr.cx, fr.cy, 0.34f * mm, 0.5f * mm, 0.33f * mm); };
        if (compact) {
            p.cap(fr.cx, fr.cy, 0.47f * h, 0.47f * h, 0.2f * h);
            p.seam.push_back(p.face([=](float x, float y) { return std::fabs(outline(x, y)) - 0.5f; }));
        }
        p.mouse(fr.cx, fr.cy, mm, 0, 0, true);
        p.ink.push_back(p.face([=](float x, float y) {
            const float dx = std::fabs(x - fr.cx), dy = std::fabs(y - fr.cy), half = 0.045f * h, reach = at - 0.02f * h;
            const float shaft = std::min(std::max(dx - half, dy - reach), std::max(dy - half, dx - reach));
            return std::max(shaft, gap - outline(x, y));
        }));
        for (int d = 0; d < 4; d++) {
            p.arrow(fr.cx + (d == 1 ? -at : d == 3 ? at : 0), fr.cy + (d == 0 ? -at : d == 2 ? at : 0), head, d);
            if (g.lit && !(g.lit >> d & 1))
                p.faint.push_back(p.face([=](float x, float y) {
                    const float dx = x - fr.cx, dy = y - fr.cy;
                    const float wedge = d == 0 ? std::fabs(dx) + dy : d == 1 ? std::fabs(dy) + dx : d == 2 ? std::fabs(dx) - dy : std::fabs(dy) - dx;
                    return std::max(wedge, 0.5f * gap - outline(x, y));
                }));
        }
        break;
    }
    }

    const int n = std::clamp(512 / size, 4, 8);
    const float depth = s.depth, face_y = fr.cy;
    auto body_sd = [&](float x, float y) { return painter::merged(p.body, x, y); };
    auto all_sd = [&](float x, float y) { return body_sd(x, y - std::clamp(y - face_y, 0.f, depth)); };
    const grid body = p.body.empty() ? grid(size, size, 0) : mask(size, size, [&](float x, float y) { return area(body_sd, x, y, n); });
    const grid all = depth > 0 && !p.body.empty() ? mask(size, size, [&](float x, float y) { return area(all_sd, x, y, n); }) : body;
    const grid rim = s.rim_a > 0 && !p.body.empty() ? mask(size, size, [&](float x, float y) {
        return std::max(0.f, area([&](float u, float v) { return all_sd(u, v) - s.rim_hi; }, x, y, n) -
                                 area([&](float u, float v) { return all_sd(u, v) - s.rim_lo; }, x, y, n));
    }) : grid();
    const grid seam = p.seam.empty() ? grid() : mask(size, size, [&](float x, float y) { return area([&](float u, float v) { return painter::merged(p.seam, u, v); }, x, y, n); });
    grid ink = p.ink.empty() ? std::move(p.text_ink) : mask(size, size, [&](float x, float y) { return area([&](float u, float v) { return painter::merged(p.ink, u, v); }, x, y, n); });
    if (!p.ink.empty())
        for (size_t i = 0; i < ink.v.size(); i++) ink.v[i] = std::max(ink.v[i], p.text_ink.v[i]);

    int top = size, bottom = 0;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            if (body.at(x, y) > 0) {
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
    const float span = (float)std::max(1, bottom - top);
    const bool tinted = g.kind == prompt_kind::face && g.tint;
    const rgb ink_core = tinted ? mix(s.ink, tint_of(g.tint), hud ? 1.f : skin == prompt_skin::tilt_fire || skin == prompt_skin::flat_fire ? 0.f : 0.3f) : s.ink;
    const rgb ink_glow = tinted && s.ink_glow_a > 0 && (skin == prompt_skin::tilt_dark || skin == prompt_skin::flat_dark) ? tint_of(g.tint) : s.ink_glow;
    auto grained = [&](rgb v, int x, int y) {
        const float k = 1 + s.grain * (0.6f * grain(x / 2, y / 2, 7) + 0.4f * grain(x, y, 3));
        return rgb{ v.r * k, v.g * k, v.b * k };
    };

    canvas out(size, size);
    if (s.glow_a > 0) {
        grid shifted(size, size, 0);
        const int drop = (int)s.glow_drop;
        for (int y = drop; y < size; y++)
            for (int x = 0; x < size; x++) shifted.at(x, y) = all.at(x, y - drop);
        out.paint(blur(shifted, s.glow_sigma), [&](int x, int y) { return std::pair{ s.glow, s.glow_a * (1 + 0.5f * grain(x, y, 11)) }; });
    }
    if (!hud && rim.w) out.paint(rim, [&](int, int) { return std::pair{ s.rim, s.rim_a }; });
    if (depth > 0)
        out.paint(all, [&](int x, int y) {
            return std::pair{ grained(mix(s.side_top, s.side_bottom, std::clamp((y - top) / (span + depth), 0.f, 1.f)), x, y), 1.f };
        });
    if (s.top.r + s.top.g + s.top.b > 0)
        out.paint(body, [&](int x, int y) { return std::pair{ grained(mix(s.top, s.bottom, std::pow(std::clamp((y - top) / span, 0.f, 1.f), s.curve)), x, y), 1.f }; });
    if (hud && rim.w) out.paint(rim, [&](int, int) { return std::pair{ s.rim, s.rim_a }; });
    if (seam.w) out.paint(seam, [&](int, int) { return std::pair{ s.rim, 1.f }; });
    if (s.ink_glow_a > 0) out.paint(blur(ink, s.ink_sigma), [&](int, int) { return std::pair{ ink_glow, s.ink_glow_a }; });
    out.paint(ink, [&](int, int) { return std::pair{ ink_core, 1.f }; });
    if (!p.faint.empty()) {
        const grid faint = mask(size, size, [&](float x, float y) { return area([&](float u, float v) { return painter::merged(p.faint, u, v); }, x, y, n); });
        for (size_t i = 0; i < faint.v.size(); i++)
            for (int ch = 0; ch < 4; ch++) out.px[i * 4 + ch] *= 1 - 0.7f * faint.v[i];
    }
    out.store(rgba);
    return true;
}

void prompt_glow(uint8_t* rgba, int size)
{
    grid solid(size, size, 0);
    for (size_t i = 0; i < solid.v.size(); i++) solid.v[i] = rgba[i * 4 + 3] > 128;
    const grid soft = blur(solid, 1.25f);
    for (size_t i = 0; i < soft.v.size(); i++) {
        std::memset(rgba + i * 4, 255, 3);
        rgba[i * 4 + 3] = (uint8_t)std::lround(std::min(soft.v[i] * 1.5f, 1.f) * 255);
    }
}

void prompt_squircle(const uint8_t* bc3, int size, uint8_t* out)
{
    std::vector<float> src((size_t)size * size * 4);
    for (int by = 0; by < size / 4; by++)
        for (int bx = 0; bx < size / 4; bx++) {
            const uint8_t* b = bc3 + (by * (size / 4) + bx) * 16;
            int a[8] = { b[0], b[1] };
            for (int k = 1; k < 7; k++) a[k + 1] = a[0] > a[1] ? ((7 - k) * a[0] + k * a[1]) / 7 : k < 5 ? ((5 - k) * a[0] + k * a[1]) / 5 : k == 5 ? 0 : 255;
            uint64_t abits = 0;
            for (int k = 0; k < 6; k++) abits |= (uint64_t)b[2 + k] << (8 * k);
            float colors[16][3];
            decode_colors(b + 8, false, colors);
            for (int i = 0; i < 16; i++) {
                float* px = &src[(((size_t)by * 4 + i / 4) * size + bx * 4 + i % 4) * 4];
                std::memcpy(px, colors[i], 3 * sizeof(float));
                px[3] = (float)a[abits >> (3 * i) & 7];
            }
        }
    auto texel = [&](int x, int y, int ch) { return src[((size_t)std::clamp(y, 0, size - 1) * size + std::clamp(x, 0, size - 1)) * 4 + ch]; };
    std::vector<uint8_t> rgba((size_t)size * size * 4);
    const float c = size / 2.f;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            const float dx = x + 0.5f - c, dy = y + 0.5f - c, round = std::hypot(dx, dy);
            const float k = round > 0 ? std::sqrt(std::sqrt(dx * dx * dx * dx + dy * dy * dy * dy)) / round : 0;
            const float u = c + dx * k - 0.5f, v = c + dy * k - 0.5f, fu = u - std::floor(u), fv = v - std::floor(v);
            const int iu = (int)std::floor(u), iv = (int)std::floor(v);
            for (int ch = 0; ch < 4; ch++) {
                const float top = texel(iu, iv, ch) + (texel(iu + 1, iv, ch) - texel(iu, iv, ch)) * fu;
                const float bottom = texel(iu, iv + 1, ch) + (texel(iu + 1, iv + 1, ch) - texel(iu, iv + 1, ch)) * fu;
                rgba[((size_t)y * size + x) * 4 + ch] = (uint8_t)std::lround(std::clamp(top + (bottom - top) * fv, 0.f, 255.f));
            }
        }
    prompt_bc3(rgba.data(), size, size, out, (size_t)size / 4 * 16);
}

void prompt_mark_dxt1(uint8_t* dxt1, int width, int height, const prompt_mark& m, prompt_skin skin, const prompt_glyph& g)
{
    const float max_w = 1.3f * m.w;
    const int size = ((int)std::ceil(std::max(max_w, m.h) + 8) + 3) & ~3;
    const int x0 = std::clamp((int)(m.x - size / 2.f) & ~3, 0, width - size), y0 = std::clamp((int)(m.y - size / 2.f) & ~3, 0, height - size);
    uint8_t* const first = dxt1 + ((size_t)y0 / 4 * (width / 4) + x0 / 4) * 8;
    auto block = [&](int bx, int by) { return first + ((size_t)by * (width / 4) + bx) * 8; };
    std::vector<float> px((size_t)size * size * 3);
    for (int by = 0; by < size / 4; by++)
        for (int bx = 0; bx < size / 4; bx++) {
            float colors[16][3];
            decode_colors(block(bx, by), true, colors);
            for (int i = 0; i < 16; i++) std::memcpy(&px[(((size_t)by * 4 + i / 4) * size + bx * 4 + i % 4) * 3], colors[i], sizeof colors[i]);
        }
    const float fx = m.x - x0, fy = m.y - y0, r = m.round ? std::min(m.w, m.h) / 2 : 3.f;
    auto shape = [&](int x, int y) { return sd_round_rect(x + 0.5f, y + 0.5f, fx, fy, m.w / 2, m.h / 2, r); };
    float mean[3] = {};
    int count = 0;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            if (const float d = shape(x, y); d > 2.5f && d < 5) {
                for (int ch = 0; ch < 3; ch++) mean[ch] += px[((size_t)y * size + x) * 3 + ch];
                count++;
            }
    std::vector<uint8_t> rgba((size_t)size * size * 4);
    prompt_render(skin, { fx, fy, m.w, max_w, m.h }, g, size, rgba.data());
    std::vector<uint8_t> out((size_t)size * size * 4);
    for (int i = 0; i < size * size; i++) {
        const float erase = std::clamp(2 - shape(i % size, i / size), 0.f, 1.f), a = rgba[i * 4 + 3] / 255.f;
        for (int ch = 0; ch < 3; ch++) {
            const float under = px[(size_t)i * 3 + ch] + (mean[ch] / std::max(count, 1) - px[(size_t)i * 3 + ch]) * erase;
            out[(size_t)i * 4 + ch] = (uint8_t)std::lround(std::clamp(under + (rgba[i * 4 + ch] - under) * a, 0.f, 255.f));
        }
        out[(size_t)i * 4 + 3] = 255;
    }
    uint8_t tile[64];
    for (int by = 0; by < size / 4; by++)
        for (int bx = 0; bx < size / 4; bx++) {
            for (int y = 0; y < 4; y++) std::memcpy(tile + y * 16, &out[(((size_t)by * 4 + y) * size + bx * 4) * 4], 16);
            encode_color_block(tile, block(bx, by));
        }
}
void prompt_bc3(const uint8_t* rgba, int w, int h, uint8_t* out, size_t row_pitch)
{
    uint8_t block[64];
    for (int by = 0; by < h / 4; by++)
        for (int bx = 0; bx < w / 4; bx++) {
            for (int y = 0; y < 4; y++) std::memcpy(block + y * 16, rgba + ((size_t)(by * 4 + y) * w + bx * 4) * 4, 16);
            encode_block(block, out + by * row_pitch + bx * 16);
        }
}
