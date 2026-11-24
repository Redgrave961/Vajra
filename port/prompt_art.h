#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

enum class prompt_skin : uint8_t { hud, tilt_dark, tilt_fire, flat_dark, flat_fire, cap_dark, cap_fire, label, sync, gauge };

enum class prompt_kind : uint8_t { key, face, mouse, wheel_up, wheel_down, keys_wasd, mouse_move };

struct prompt_frame {
    float cx, cy, min_w, max_w, h;
};

struct prompt_glyph {
    prompt_kind kind = prompt_kind::key;
    uint32_t tint = 0;
    int button = 0;
    uint8_t lit = 0;
    std::string label[4];
};

struct prompt_mark {
    float x, y, w, h;
    bool round;
};

bool prompt_render(prompt_skin skin, const prompt_frame& frame, const prompt_glyph& glyph, int size, uint8_t* rgba);
void prompt_glow(uint8_t* rgba, int size);
void prompt_mark_dxt1(uint8_t* dxt1, int width, int height, const prompt_mark& mark, prompt_skin skin, const prompt_glyph& glyph);
void prompt_bc3(const uint8_t* rgba, int w, int h, uint8_t* out, size_t row_pitch);
