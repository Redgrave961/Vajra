#pragma once

#include "../ps3recomp/third_party/stb/stb_truetype.h"

#include <cstdint>
#include <string>
#include <vector>

enum class font_weight { regular, semibold, bold, black };

struct font {
    std::vector<uint8_t> ttf;
    stbtt_fontinfo info{};
};

bool load_font(font& f, font_weight weight);
uint32_t next_cp(const std::string& s, size_t& i);
