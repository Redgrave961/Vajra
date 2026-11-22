#define STB_TRUETYPE_IMPLEMENTATION
#include "font.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <fontconfig/fontconfig.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <initializer_list>

namespace {

bool read_font(font& f, const std::filesystem::path& file, int index)
{
#ifdef _WIN32
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    const bool ok_sz = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart <= (64ll << 20);
    bool ok = false;
    if (ok_sz) {
        f.ttf.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = ReadFile(h, f.ttf.data(), (DWORD)f.ttf.size(), &got, nullptr) && got == f.ttf.size();
    }
    CloseHandle(h);
    if (!ok) return false;
#else
    const int fd = open(file.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st{};
    bool ok = false;
    if (!fstat(fd, &st) && st.st_size > 0 && st.st_size <= (64ll << 20)) {
        f.ttf.resize((size_t)st.st_size);
        size_t off = 0;
        while (off < f.ttf.size()) {
            const ssize_t n = read(fd, f.ttf.data() + off, f.ttf.size() - off);
            if (n <= 0) break;
            off += (size_t)n;
        }
        ok = off == f.ttf.size();
    }
    close(fd);
    if (!ok) return false;
#endif
    const int offset = stbtt_GetFontOffsetForIndex(f.ttf.data(), index);
    return offset >= 0 && stbtt_InitFont(&f.info, f.ttf.data(), offset);
}

}

#ifdef _WIN32
bool load_font(font& f, font_weight weight)
{
    const char* windir = std::getenv("WINDIR");
    const std::filesystem::path dir = std::filesystem::path(windir ? windir : "C:\\Windows") / "Fonts";
    const std::initializer_list<const char*> names = weight == font_weight::black      ? std::initializer_list<const char*>{ "seguibl.ttf", "segoeuib.ttf", "segoeui.ttf" }
                                                     : weight == font_weight::bold     ? std::initializer_list<const char*>{ "segoeuib.ttf", "segoeui.ttf" }
                                                     : weight == font_weight::semibold ? std::initializer_list<const char*>{ "seguisb.ttf", "segoeuib.ttf", "segoeui.ttf" }
                                                                                       : std::initializer_list<const char*>{ "segoeui.ttf" };
    for (const char* name : names)
        if (read_font(f, dir / name, 0)) return true;
    return false;
}
#else
bool load_font(font& f, font_weight weight)
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
    const char* query = weight == font_weight::black      ? "sans-serif:weight=210:variable=false"
                        : weight == font_weight::bold     ? "sans-serif:weight=200:variable=false"
                        : weight == font_weight::semibold ? "sans-serif:weight=180:variable=false"
                                                          : "sans-serif:weight=80:variable=false";
    FcPattern* pattern = parse(reinterpret_cast<const FcChar8*>(query));
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

uint32_t next_cp(const std::string& s, size_t& i)
{
    const uint8_t c = (uint8_t)s[i++];
    if (c < 0x80) return c;
    const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    uint32_t cp = c & (0x3F >> extra);
    for (int k = 0; k < extra && i < s.size(); k++) cp = cp << 6 | ((uint8_t)s[i++] & 0x3F);
    return cp;
}
