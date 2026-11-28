#include "game_files.h"

#include "firmware.h"
#include "setup_window.h"
#include "../ps3recomp/libs/filesystem/npdrm.h"
#include "../ps3recomp/libs/filesystem/ps3_self.h"
#include "../ps3recomp/libs/filesystem/ps3_vfs.h"
#include "../ps3recomp/libs/system/cellSysutil.h"
#include "../ps3recomp/runtime/platform/ps3_utf8_fs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <thread>

namespace {

constexpr const char* k_title_id_eu = "BLES01227";
constexpr const char* k_title_id_us = "BLUS30721";
constexpr const char* k_eboot_sha256_us = "065c1f0c5f6203c4007730dbbd53edcf9f80d242ee78ab91d8e69198b723a342";
constexpr const char* k_update_version = "01.02";
constexpr const char* k_dash = " \xE2\x80\x94 ";

bool is_supported_title(const std::string& tid) { return tid == k_title_id_eu || tid == k_title_id_us; }

const char* eboot_sha256_for(const std::string& tid)
{
    if (tid == k_title_id_eu) return PORT_EBOOT_SHA256;
    if (tid == k_title_id_us) return k_eboot_sha256_us;
    return "";
}

std::string region_of(const std::string& tid) { return tid == k_title_id_eu ? "EU" : tid == k_title_id_us ? "US" : tid; }

std::string utf8(const std::filesystem::path& p, bool generic = false) { return utf8_from_path(p, generic); }

std::string name_of(const std::filesystem::path& p) { return utf8(p.filename()); }

std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n\"");
    const size_t b = s.find_last_not_of(" \t\r\n\"");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::array<uint8_t, 32> sha256(const uint8_t* data, size_t size)
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    auto rotr = [](uint32_t x, int n) { return x >> n | x << (32 - n); };
    auto block = [&](const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    };
    size_t i = 0;
    for (; i + 64 <= size; i += 64) block(data + i);
    uint8_t tail[128] = {};
    const size_t rest = size - i;
    std::memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    const size_t len = rest < 56 ? 64 : 128;
    const uint64_t bits = (uint64_t)size * 8;
    for (int b = 0; b < 8; b++) tail[len - 1 - b] = (uint8_t)(bits >> (8 * b));
    block(tail);
    if (len == 128) block(tail + 64);
    std::array<uint8_t, 32> out;
    for (int j = 0; j < 8; j++)
        for (int b = 0; b < 4; b++) out[j * 4 + b] = (uint8_t)(h[j] >> (24 - 8 * b));
    return out;
}

std::string hex(const std::array<uint8_t, 32>& d)
{
    static const char digits[] = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

bool read_all(const std::filesystem::path& path, std::vector<uint8_t>& out)
{
    ps3_vfile* f = ps3_vfile_open(utf8(path, true).c_str(), "rb");
    if (!f) return false;
    ps3_vstat st{};
    if (ps3_vfile_fstat(f, &st) != 0) {
        ps3_vfile_close(f);
        return false;
    }
    out.resize((size_t)st.size);
    const bool ok = ps3_vfile_read(out.data(), out.size(), f) == out.size();
    ps3_vfile_close(f);
    return ok;
}

bool starts_with(const std::filesystem::path& path, const char* magic, size_t n)
{
    ps3_vfile* f = ps3_vfile_open(utf8(path, true).c_str(), "rb");
    if (!f) return false;
    char head[8] = {};
    const bool ok = n <= sizeof head && ps3_vfile_read(head, n, f) == n && !std::memcmp(head, magic, n);
    ps3_vfile_close(f);
    return ok;
}

std::string sfo_string(const std::filesystem::path& path, const char* key)
{
    std::vector<uint8_t> d;
    if (!read_all(path, d) || d.size() < 0x14 || std::memcmp(d.data(), "\0PSF", 4)) return {};
    auto le32 = [&](size_t o) { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24; };
    const uint32_t keys = le32(8), values = le32(12), count = le32(16);
    const size_t key_len = std::strlen(key);
    for (uint32_t i = 0; i < count && 0x14 + (size_t)i * 16 + 16 <= d.size(); i++) {
        const size_t e = 0x14 + (size_t)i * 16;
        const size_t name = keys + (size_t)(d[e] | d[e + 1] << 8), value = values + le32(e + 12), len = le32(e + 4);
        if (name >= d.size() || value > d.size() || len > d.size() - value) break;
        const size_t avail = d.size() - name;
        const char* kptr = reinterpret_cast<const char*>(d.data() + name);
        if (strnlen(kptr, avail) == key_len && !std::memcmp(kptr, key, key_len))
            return std::string(reinterpret_cast<const char*>(d.data() + value), strnlen(reinterpret_cast<const char*>(d.data() + value), len));
    }
    return {};
}

std::filesystem::path path_of(const std::string& value) { return path_from_utf8(value); }

bool same_file(const std::filesystem::path& a, const std::filesystem::path& b)
{
    if (a == b) return true;
    std::error_code ec;
    return std::filesystem::equivalent(a, b, ec) && !ec;
}

void add_unique(std::vector<std::filesystem::path>& list, const std::filesystem::path& file)
{
    if (std::none_of(list.begin(), list.end(), [&](const std::filesystem::path& existing) { return same_file(existing, file); }))
        list.push_back(file);
}

game_paths read_paths(const std::filesystem::path& exe, bool launcher)
{
    game_paths p;
    std::ifstream ini(exe / "asura_paths.ini");
    for (std::string line; std::getline(ini, line);) {
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (value.empty()) continue;
        if (key == "game") p.game = path_of(value);
        else if (key == "update") p.update = path_of(value);
        else if (key == "setup_dlc" || (key == "dlc" && !launcher)) p.dlcs.push_back(path_of(value));
        else if (key == "firmware") p.firmware = path_of(value);
    }
    return p;
}

void write_paths(const std::filesystem::path& exe, const game_paths& p)
{
    std::string ini;
    auto line = [&](const char* key, const std::filesystem::path& v) {
        if (!v.empty()) ini += std::string(key) + '=' + utf8(v) + '\n';
    };
    line("game", p.game);
    line("update", p.update);
    for (const auto& d : p.dlcs) line("setup_dlc", d);
    line("firmware", p.firmware);
    write_file_atomic(exe / "asura_paths.ini", ini);
}

struct mounted {
    game_check check;
    std::filesystem::path bdvd;
    std::vector<uint8_t> elf;
};

std::string short_id(const std::string& cid) { return cid.size() > 20 ? cid.substr(20) : cid; }

std::filesystem::path exdata_dir(const std::filesystem::path& exe) { return exe / "user" / "dev_hdd0" / "home" / "00000001" / "exdata"; }

bool is_game_folder(const std::filesystem::path& dir)
{
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / "PS3_GAME" / "PARAM.SFO", ec);
}

std::filesystem::path strip_trailing_sep(std::filesystem::path p)
{
    while (!p.empty() && p.filename().empty() && p.has_parent_path() && p != p.root_path())
        p = p.parent_path();
    return p;
}

std::string upper_ascii(std::string s)
{
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

std::string lower_ext(const std::filesystem::path& file)
{
    std::string ext = utf8(file.extension());
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext;
}

std::filesystem::path normalize_game_dir(std::filesystem::path p)
{
    p = strip_trailing_sep(std::move(p));
    if (upper_ascii(utf8(p.filename())) == "PS3_GAME") p = p.parent_path();
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec) && !is_game_folder(p)) {
        for (auto it = std::filesystem::directory_iterator(p, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (it->is_directory(ec) && is_game_folder(it->path())) return it->path();
        }
    }
    return p;
}

std::string detect_game_title(const std::filesystem::path& exe, const std::filesystem::path& game, bool* out_encrypted = nullptr)
{
    if (out_encrypted) *out_encrypted = false;
    if (game.empty()) return {};
    std::error_code ec;
    if (std::filesystem::is_directory(game, ec) && !ec) {
        const std::string tid = sfo_string(game / "PS3_GAME" / "PARAM.SFO", "TITLE_ID");
        if (out_encrypted && !tid.empty() && !starts_with(game / "PS3_GAME" / "USRDIR" / "EBOOT.BIN", "SCE", 4))
            *out_encrypted = true;
        return tid;
    }
    const std::filesystem::path probe = exe / "user" / "check" / "probe_tid";
    ps3_vfs_unmount_all();
    std::string tid;
    if (!ps3_vfs_mount_iso(utf8(probe, true).c_str(), utf8(game).c_str())) {
        tid = sfo_string(probe / "PS3_GAME" / "PARAM.SFO", "TITLE_ID");
        if (out_encrypted && !tid.empty() && !starts_with(probe / "PS3_GAME" / "USRDIR" / "EBOOT.BIN", "SCE", 4))
            *out_encrypted = true;
    }
    ps3_vfs_unmount_all();
    return tid;
}

bool write_rap_file(const std::filesystem::path& exdata, const std::string& filename, const uint8_t rap[16])
{
    std::error_code ec;
    std::filesystem::create_directories(exdata, ec);
    const std::filesystem::path dst = exdata / path_of(filename);
    if (std::filesystem::exists(dst, ec))
        std::filesystem::permissions(dst, std::filesystem::perms::owner_write, std::filesystem::perm_options::add, ec);
    return write_file_atomic(dst, std::string_view(reinterpret_cast<const char*>(rap), 16));
}

std::string pkg_title_id(const char cid[37])
{
    if (std::strlen(cid) < 16) return {};
    return upper_ascii(std::string(cid + 7, 9));
}

std::string extract_content_id(const std::string& text)
{
    auto is_alnum = [](char c) { return std::isalnum((unsigned char)c) != 0; };
    auto is_digit = [](char c) { return std::isdigit((unsigned char)c) != 0; };
    auto is_prod = [](char c) { return std::isalnum((unsigned char)c) != 0 || c == '_' || c == '-'; };
    for (size_t i = 0; i + 36 <= text.size(); i++) {
        if (text[i + 6] != '-' || text[i + 16] != '_' || text[i + 19] != '-') continue;
        bool ok = is_alnum(text[i]) && is_alnum(text[i + 1]);
        for (size_t k = 2; k < 6 && ok; k++) ok = is_digit(text[i + k]);
        for (size_t k = 7; k < 16 && ok; k++) ok = is_alnum(text[i + k]);
        ok = ok && is_digit(text[i + 17]) && is_digit(text[i + 18]);
        for (size_t k = 20; k < 36 && ok; k++) ok = is_prod(text[i + k]);
        if (ok) return upper_ascii(text.substr(i, 36));
    }
    return {};
}

bool read_rap_bytes(const std::filesystem::path& file, std::array<uint8_t, 16>& out)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    uint8_t buf[96];
    in.read(reinterpret_cast<char*>(buf), sizeof buf);
    const size_t n = (size_t)in.gcount();
    if (n == 16) {
        std::memcpy(out.data(), buf, 16);
        return true;
    }
    auto hex_nibble = [](uint8_t c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    size_t start = (n >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) ? 3 : 0;
    while (start < n && std::isspace(buf[start])) start++;
    if (start + 2 <= n && buf[start] == '0' && (buf[start + 1] == 'x' || buf[start + 1] == 'X')) start += 2;
    uint8_t digits[32];
    size_t dn = 0;
    for (size_t i = start; i < n; i++) {
        if (std::isspace(buf[i])) continue;
        if (dn >= 32 || hex_nibble(buf[i]) < 0) return false;
        digits[dn++] = buf[i];
    }
    if (dn != 32) return false;
    for (size_t i = 0; i < 16; i++)
        out[i] = (uint8_t)((hex_nibble(digits[i * 2]) << 4) | hex_nibble(digits[i * 2 + 1]));
    return true;
}

struct update_probe {
    std::string version;
    bool valid = false;
};

update_probe probe_update_pkg(const std::filesystem::path& exe, const std::filesystem::path& file)
{
    const std::filesystem::path probe = exe / "user" / "check" / "probe_upd";
    ps3_vfs_unmount_all();
    char cid[37] = {};
    update_probe u;
    if (!ps3_vfs_mount_pkg(utf8(probe, true).c_str(), utf8(file).c_str(), cid)) {
        const std::string tid = pkg_title_id(cid);
        u.version = sfo_string(probe / tid / "PARAM.SFO", "APP_VER");
        std::vector<uint8_t> self;
        uint8_t* elf = nullptr;
        size_t elf_size = 0;
        if (read_all(probe / tid / "USRDIR" / "EBOOT.BIN", self) && !ps3_self_decrypt(self.data(), self.size(), &elf, &elf_size)) {
            u.valid = hex(sha256(elf, elf_size)) == eboot_sha256_for(tid);
            std::free(elf);
        }
    }
    ps3_vfs_unmount_all();
    return u;
}

std::string describe(const dlc_status& d)
{
    const std::string head = d.name + (d.region.empty() ? std::string() : " (" + d.region + ")") + k_dash;
    switch (d.state) {
    case dlc_state::ok:
        return head + "OK" + k_dash + (!d.needs_rap ? std::string("free, no licence needed") : d.rap.empty() ? std::string("unlocked") : "unlocked by " + utf8(d.rap));
    case dlc_state::missing_rap:
        if (!d.rap.empty())
            return head + "WILL NOT WORK: " + name_of(d.rap) + " is the licence for the other region's copy" + k_dash + "put the " + d.region +
                   " .rap in " + utf8(d.file.parent_path());
        return head + "WILL NOT WORK: its .rap licence is missing" + k_dash + "put its .rap in " + utf8(d.file.parent_path());
    case dlc_state::wrong_rap:
        return head + "WILL NOT WORK: wrong licence" + k_dash + utf8(d.rap) + " does not unlock it";
    case dlc_state::wrong_game:
        return head + "WRONG GAME" + k_dash + d.detail;
    case dlc_state::unreadable:
        return head + "UNREADABLE" + k_dash + "not a PS3 package, or damaged";
    case dlc_state::duplicate:
        return head + "DUPLICATE" + k_dash + d.detail;
    }
    return head;
}

dlc_status rejected_pkg(const std::filesystem::path& file, const char cid[37], bool readable)
{
    dlc_status d;
    d.file = file;
    d.name = name_of(file);
    d.content_id = cid;
    d.state = readable ? dlc_state::wrong_game : dlc_state::unreadable;
    const std::string tid = pkg_title_id(cid);
    d.detail = "a package for " + (tid.empty() ? std::string("another game") : tid) + ", needs BLES01227 or BLUS30721";
    return d;
}

struct add_context {
    const std::filesystem::path& exe;
    game_paths& paths;
    game_add_result& res;
    game_add_target target;
    std::string game_tid{};
    bool game_tid_known = false;
    size_t skipped = 0;
};

void note(add_context& a, std::string text, bool problem) { a.res.notes.push_back({ std::move(text), problem }); }

const std::string& game_title(add_context& a)
{
    if (!a.game_tid_known) {
        a.game_tid = detect_game_title(a.exe, a.paths.game);
        a.game_tid_known = true;
    }
    return a.game_tid;
}

void use_game(add_context& a, const std::filesystem::path& game)
{
    if (a.target.game_fixed && !same_file(a.paths.game, game)) {
        note(a, name_of(game) + " is a game copy, but the game is already set" + k_dash + "ignored", false);
        return;
    }
    a.paths.game = game;
    a.game_tid_known = false;
}

void use_pkg(add_context& a, const std::filesystem::path& file)
{
    char cid[37] = {};
    int has_eboot = 0;
    if (ps3_vfs_pkg_info(utf8(file).c_str(), cid, &has_eboot)) {
        note(a, describe(rejected_pkg(file, cid, false)) + k_dash + "ignored", true);
        return;
    }
    const std::string tid = pkg_title_id(cid);
    if (!is_supported_title(tid)) {
        note(a, describe(rejected_pkg(file, cid, true)) + k_dash + "ignored", true);
        return;
    }
    if (!has_eboot) {
        add_unique(a.target.launch ? a.paths.launch_dlcs : a.paths.dlcs, file);
        return;
    }
    const update_probe u = probe_update_pkg(a.exe, file);
    const std::string what = name_of(file) + k_dash + "GAME UPDATE (" + (u.version.empty() ? std::string("unknown version") : "version " + u.version) + ", " +
                             region_of(tid) + ")" + k_dash;
    const std::string& game_tid = game_title(a);
    if (!u.valid) note(a, what + "not DLC and not the " + k_update_version + " update" + k_dash + "ignored", false);
    else if (is_supported_title(game_tid) && tid != game_tid) note(a, what + "not DLC, and the game is " + region_of(game_tid) + k_dash + "ignored", false);
    else if (a.target.update_fixed && !same_file(a.paths.update, file)) note(a, what + "not DLC, and an update is already set" + k_dash + "ignored", false);
    else {
        a.paths.update = file;
        note(a, what + "used as the update", false);
    }
}

void use_rap(add_context& a, const std::filesystem::path& file, const std::array<uint8_t, 16>& rap)
{
    const std::string stem = utf8(file.stem());
    const std::string embedded_cid = extract_content_id(stem);
    const std::string target_name = !embedded_cid.empty()
                                        ? (embedded_cid + ".rap")
                                        : ((stem.empty() ? std::string("rap") : stem) + "_" + hex(sha256(rap.data(), rap.size())).substr(0, 16) + ".rap");
    if (write_rap_file(exdata_dir(a.exe), target_name, rap.data())) note(a, name_of(file) + k_dash + "licence added as " + target_name, false);
    else note(a, name_of(file) + k_dash + "licence could not be saved to the exdata folder", true);
}

void sort_file(add_context& a, const std::filesystem::path& file, bool direct)
{
    const std::string ext = lower_ext(file);
    std::array<uint8_t, 16> rap{};
    if (ext == ".iso") {
        if (is_supported_title(detect_game_title(a.exe, file))) use_game(a, file);
        else note(a, name_of(file) + " is not a BLES01227 or BLUS30721 disc image" + k_dash + "ignored", false);
    } else if (ext == ".pkg") {
        use_pkg(a, file);
    } else if (ext == ".pup") {
        a.res.pup = file;
    } else if (ext == ".rap") {
        if (read_rap_bytes(file, rap)) use_rap(a, file, rap);
        else note(a, name_of(file) + k_dash + "INVALID RAP" + k_dash + "expected 16 bytes or 32 hex digits" + k_dash + "ignored", true);
    } else if (direct && read_rap_bytes(file, rap)) {
        use_rap(a, file, rap);
    } else if (direct) {
        note(a, name_of(file) + " is not a game, update, DLC or licence file" + k_dash + "ignored", false);
    } else {
        a.skipped++;
    }
}

void append_utf8(std::string& out, uint32_t c)
{
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) out += { (char)(0xC0 | c >> 6), (char)(0x80 | (c & 0x3F)) };
    else if (c < 0x10000) out += { (char)(0xE0 | c >> 12), (char)(0x80 | (c >> 6 & 0x3F)), (char)(0x80 | (c & 0x3F)) };
    else out += { (char)(0xF0 | c >> 18), (char)(0x80 | (c >> 12 & 0x3F)), (char)(0x80 | (c >> 6 & 0x3F)), (char)(0x80 | (c & 0x3F)) };
}

std::string utf8_from_utf16(const uint8_t* p, size_t units)
{
    std::string out;
    for (size_t i = 0; i < units; i++) {
        uint32_t c = p[i * 2] | (uint32_t)p[i * 2 + 1] << 8;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < units) {
            const uint32_t low = p[i * 2 + 2] | (uint32_t)p[i * 2 + 3] << 8;
            if (low >= 0xDC00 && low < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                i++;
            }
        }
        if (!c) break;
        append_utf8(out, c);
    }
    return out;
}

std::string utf8_from_latin1(const uint8_t* p, size_t n)
{
    std::string out;
    for (size_t i = 0; i < n && p[i]; i++) append_utf8(out, p[i]);
    return out;
}

std::string decode_text(const std::vector<uint8_t>& d)
{
    if (d.size() >= 2 && d[0] == 0xFF && d[1] == 0xFE) return utf8_from_utf16(d.data() + 2, (d.size() - 2) / 2);
    return utf8_from_latin1(d.data(), d.size());
}

struct ini_entry {
    std::string section, key, value;
};

std::vector<ini_entry> parse_ini(const std::string& text)
{
    std::vector<ini_entry> out;
    std::string section;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = trim(text.substr(at, end - at));
        at = end + 1;
        if (line.empty() || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq != std::string::npos) out.push_back({ section, trim(line.substr(0, eq)), trim(line.substr(eq + 1)) });
    }
    return out;
}

struct coalesced_reader {
    const std::vector<uint8_t>& d;
    size_t at = 0;
    bool ok = true;

    int32_t count()
    {
        if (d.size() - at < 4) {
            ok = false;
            return 0;
        }
        const uint32_t v = (uint32_t)d[at] << 24 | (uint32_t)d[at + 1] << 16 | (uint32_t)d[at + 2] << 8 | d[at + 3];
        at += 4;
        return (int32_t)v;
    }

    std::string text()
    {
        const int64_t n = count();
        const uint64_t bytes = n < 0 ? (uint64_t)-n * 2 : (uint64_t)n;
        if (!ok || bytes > d.size() - at) {
            ok = false;
            return {};
        }
        std::string s = n < 0 ? utf8_from_utf16(d.data() + at, (size_t)bytes / 2) : utf8_from_latin1(d.data() + at, (size_t)bytes);
        at += (size_t)bytes;
        return s;
    }
};

struct localization {
    std::map<std::string, std::string> titles, chapters, numbers;
    std::string episode;
};

std::string game_language()
{
    switch (ps3_sysutil_language()) {
    case CELL_SYSUTIL_LANG_JAPANESE: return "JPN";
    case CELL_SYSUTIL_LANG_FRENCH: return "FRA";
    case CELL_SYSUTIL_LANG_SPANISH: return "ESN";
    case CELL_SYSUTIL_LANG_GERMAN: return "DEU";
    case CELL_SYSUTIL_LANG_ITALIAN: return "ITA";
    default: return "INT";
    }
}

bool read_localization(const std::filesystem::path& bdvd, const std::string& lang, localization& loc)
{
    std::vector<uint8_t> d;
    if (!read_all(bdvd / "PS3_GAME" / "USRDIR" / "BCGAME" / "COOKEDPS3" / ("COALESCED_" + lang + ".BIN"), d)) return false;
    std::string ext = lang;
    for (char& ch : ext) ch = (char)std::tolower((unsigned char)ch);
    const std::string base = "..\\..\\BCGame\\Localization\\" + lang + "\\";
    const std::string game_file = base + "BCGame." + ext, common_file = base + "Common." + ext;
    coalesced_reader r{ d };
    for (int32_t files = r.count(); r.ok && files-- > 0;) {
        const std::string file = r.text();
        for (int32_t sections = r.count(); r.ok && sections-- > 0;) {
            const std::string section = r.text();
            std::map<std::string, std::string>* into = file == game_file && section == "Title"     ? &loc.titles
                                                       : file == game_file && section == "Chapter" ? &loc.chapters
                                                       : file == common_file && section == "Number" ? &loc.numbers
                                                                                                    : nullptr;
            const bool story = file == common_file && section == "Story";
            for (int32_t keys = r.count(); r.ok && keys-- > 0;) {
                const std::string key = r.text(), value = r.text();
                if (into) into->insert_or_assign(key, value);
                else if (story && key == "STORY") loc.episode = value;
            }
        }
    }
    return r.ok && !loc.titles.empty();
}

localization load_localization(const std::filesystem::path& bdvd, const std::string& lang)
{
    localization loc;
    if (!bdvd.empty() && !read_localization(bdvd, lang, loc) && lang != "INT") {
        loc = {};
        read_localization(bdvd, "INT", loc);
    }
    return loc;
}

struct dlc_episode {
    std::string stage, chapter, story;
};

struct dlc_probe {
    dlc_status status;
    std::vector<std::string> stages;
    std::vector<dlc_episode> episodes;
    std::map<std::string, std::string> numbers;
};

std::vector<dlc_episode> episodes_of(const std::string& ini)
{
    std::vector<dlc_episode> out;
    std::string current;
    for (const ini_entry& e : parse_ini(ini)) {
        if (e.section.rfind("MapProvider_", 0) != 0 || e.section.find("BCUIDataProvider_ChapterInfo") == std::string::npos) continue;
        if (e.section != current) {
            current = e.section;
            out.emplace_back();
        }
        if (e.key == "MapName" && e.value.rfind("BC-", 0) == 0) out.back().stage = e.value.substr(3);
        else if (e.key == "ChapterNo") out.back().chapter = e.value;
        else if (e.key == "StoryNo") out.back().story = e.value;
    }
    return out;
}

std::string decrypt_text(const std::string& path)
{
    ps3_vfile* f = ps3_vfile_open(path.c_str(), "rb");
    if (!f) return {};
    npdrm_file* d = npdrm_open(f);
    if (!d) {
        ps3_vfile_close(f);
        return {};
    }
    std::vector<uint8_t> data((size_t)npdrm_size(d));
    const int64_t got = npdrm_read(d, 0, data.data(), data.size());
    npdrm_close(d);
    data.resize(got < 0 ? 0 : (size_t)got);
    return decode_text(data);
}

std::string dlc_name(const dlc_probe& pr, const localization& loc)
{
    auto lookup = [](const std::map<std::string, std::string>& m, const std::string& key) {
        const auto it = m.find(key);
        return it == m.end() ? std::string() : it->second;
    };
    std::string name;
    auto add = [&](const std::string& part) {
        if (!part.empty()) name += (name.empty() ? "" : " / ") + part;
    };
    if (pr.episodes.empty()) {
        for (const std::string& stage : pr.stages) add(lookup(loc.titles, stage));
        return name.empty() ? pr.status.name : name;
    }
    auto number = [&](std::string story) {
        std::replace(story.begin(), story.end(), '.', '_');
        const std::string own = lookup(pr.numbers, "N" + story);
        return own.empty() ? lookup(loc.numbers, "N" + story) : own;
    };
    auto episode = [&](const std::string& first, const std::string& tail) {
        const size_t star = loc.episode.find('*');
        if (star == std::string::npos || first.empty() || first.find_first_not_of("0123456789.") != std::string::npos) return first + tail;
        return loc.episode.substr(0, star) + first + tail + loc.episode.substr(star + 1);
    };
    const dlc_episode& front = pr.episodes.front();
    const std::string chapter = lookup(loc.chapters, "C" + front.chapter);
    const bool one_chapter = std::all_of(pr.episodes.begin(), pr.episodes.end(), [&](const dlc_episode& e) { return e.chapter == front.chapter; });
    if (pr.episodes.size() > 1 && one_chapter && !chapter.empty())
        return chapter + k_dash + episode(number(front.story), "\xE2\x80\x93" + number(pr.episodes.back().story));
    for (const dlc_episode& e : pr.episodes) {
        const std::string label = episode(number(e.story), ""), title = lookup(loc.titles, e.stage);
        add(label.empty() ? title : title.empty() ? label : label + ": " + title);
    }
    return name.empty() ? pr.status.name : name;
}

struct dlc_scan {
    std::string exdata;
    std::string language;
    std::string rap_id;
    std::string first;
    std::vector<std::string> locked;
    std::vector<std::string> stages;
    std::string ini;
    std::string common;
};

void scan_dlc(const char* full_path, void* user)
{
    auto& s = *static_cast<dlc_scan*>(user);
    const char* slash = std::strrchr(full_path, '/');
    const std::string name = upper_ascii(slash ? slash + 1 : full_path);
    if (name.size() > 7 && name.rfind("BC-S", 0) == 0 && std::isdigit((unsigned char)name[4]) && std::isdigit((unsigned char)name[5]) &&
        std::isdigit((unsigned char)name[6]) && std::find(s.stages.begin(), s.stages.end(), name.substr(3, 4)) == s.stages.end())
        s.stages.push_back(name.substr(3, 4));
    if (name == "PS3-BCGAME.INI.EDAT") s.ini = full_path;
    if (name == "COMMON." + s.language || (name == "COMMON.INT" && s.common.empty())) s.common = full_path;
    ps3_vfile* f = ps3_vfile_open(full_path, "rb");
    if (!f) return;
    char cid[49] = {};
    const int locked = npdrm_check_vfile(f, s.exdata.c_str(), cid);
    ps3_vfile_close(f);
    if (!cid[0]) return;
    if (s.rap_id.empty()) {
        s.rap_id = cid;
        s.first = full_path;
    }
    if (locked) s.locked.emplace_back(full_path);
}

std::vector<std::filesystem::path> rap_dirs(const std::filesystem::path& exe, const game_paths& p)
{
    std::error_code ec;
    std::vector<std::filesystem::path> dirs;
    auto push_dir = [&](const std::filesystem::path& d) {
        if (!d.empty() && std::filesystem::is_directory(d, ec)) add_unique(dirs, d);
    };
    push_dir(exe / "exdata");
    for (const auto* list : { &p.dlcs, &p.launch_dlcs })
        for (const auto& d : *list) {
            const std::filesystem::path par = d.parent_path();
            push_dir(par);
            for (const std::filesystem::path& base : { par, par.parent_path() }) {
                push_dir(base / "exdata");
                push_dir(base / "rap");
                push_dir(base / "raps");
            }
        }
    if (!p.game.empty()) {
        push_dir(p.game.parent_path() / "exdata");
        if (std::filesystem::is_directory(p.game, ec)) push_dir(p.game / "exdata");
    }
    return dirs;
}

std::filesystem::path rap_file_with(const std::filesystem::path& dir, const std::string& rap_id, const uint8_t rap[16])
{
    std::array<uint8_t, 16> got{};
    const std::filesystem::path named = dir / path_of(rap_id + ".rap");
    if (read_rap_bytes(named, got) && !std::memcmp(got.data(), rap, 16)) return named;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (lower_ext(it->path()) == ".rap" && read_rap_bytes(it->path(), got) && !std::memcmp(got.data(), rap, 16)) return it->path();
    }
    return {};
}

std::filesystem::path sibling_rap(const std::vector<std::filesystem::path>& dirs, const std::string& rap_id)
{
    const std::string id = upper_ascii(rap_id);
    for (const auto& dir : dirs) {
        std::error_code ec;
        for (auto it = std::filesystem::directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (lower_ext(it->path()) != ".rap") continue;
            const std::string stem = upper_ascii(utf8(it->path().stem()));
            if (stem.size() == id.size() && stem != id && short_id(stem) == short_id(id)) return it->path();
        }
    }
    return {};
}

bool still_locked(const std::string& full_path, const std::string& exdata)
{
    ps3_vfile* f = ps3_vfile_open(full_path.c_str(), "rb");
    if (!f) return true;
    const bool locked = npdrm_check_vfile(f, exdata.c_str(), nullptr) != 0;
    ps3_vfile_close(f);
    return locked;
}

void settle_licence(dlc_status& d, const dlc_scan& s, const std::vector<std::filesystem::path>& dirs, const std::filesystem::path& exdata)
{
    uint8_t rap[16];
    char found[49] = {};
    if (ps3_vfile* f = ps3_vfile_open((s.locked.empty() ? s.first : s.locked.front()).c_str(), "rb")) {
        if (s.locked.empty()) {
            if (npdrm_find_rap_in_dir(f, s.exdata.c_str(), rap, found)) d.rap = rap_file_with(exdata, d.rap_id, rap);
        } else {
            for (const auto& dir : dirs) {
                if (!npdrm_find_rap_in_dir(f, utf8(dir, true).c_str(), rap, found)) continue;
                write_rap_file(exdata, (found[0] ? std::string(found) : d.rap_id) + ".rap", rap);
                d.rap = rap_file_with(dir, d.rap_id, rap);
                break;
            }
        }
        ps3_vfile_close(f);
    }
    if (std::none_of(s.locked.begin(), s.locked.end(), [&](const std::string& path) { return still_locked(path, s.exdata); })) return;
    std::vector<std::filesystem::path> where{ exdata };
    where.insert(where.end(), dirs.begin(), dirs.end());
    d.rap.clear();
    for (const auto& dir : where) {
        std::error_code ec;
        const std::filesystem::path named = dir / path_of(d.rap_id + ".rap");
        if (!std::filesystem::exists(named, ec)) continue;
        d.rap = named;
        break;
    }
    d.state = d.rap.empty() ? dlc_state::missing_rap : dlc_state::wrong_rap;
    if (d.rap.empty()) d.rap = sibling_rap(where, d.rap_id);
}

std::vector<dlc_probe> inspect_dlcs(const std::filesystem::path& exe, const game_paths& p, const std::filesystem::path& probe, const std::string& language)
{
    std::vector<std::filesystem::path> files = p.dlcs;
    for (const auto& d : p.launch_dlcs) add_unique(files, d);
    const std::vector<std::filesystem::path> dirs = rap_dirs(exe, p);
    const std::filesystem::path exdata = exdata_dir(exe);
    const std::string probe_dir = utf8(probe, true);
    std::vector<dlc_probe> out;
    ps3_vfs_unmount_all();
    for (size_t i = 0; i < files.size(); i++) {
        const std::string slot = "dlc" + std::to_string(i);
        char cid[37] = {};
        const int rc = ps3_vfs_mount_pkg_as(probe_dir.c_str(), slot.c_str(), utf8(files[i]).c_str(), cid);
        dlc_probe pr;
        if (rc || !is_supported_title(pkg_title_id(cid))) {
            pr.status = rejected_pkg(files[i], cid, rc == 0);
        } else {
            dlc_status& d = pr.status;
            d.file = files[i];
            d.content_id = cid;
            d.name = name_of(d.file);
            dlc_scan s{ utf8(exdata, true), language, {}, {}, {}, {}, {}, {} };
            ps3_vfs_enum_last_mount(scan_dlc, &s);
            d.needs_rap = !s.rap_id.empty();
            d.rap_id = s.rap_id;
            if (d.needs_rap) settle_licence(d, s, dirs, exdata);
            pr.stages = s.stages;
            std::vector<uint8_t> common;
            if (!s.common.empty() && read_all(path_of(s.common), common))
                for (const ini_entry& e : parse_ini(decode_text(common)))
                    if (e.section == "Number") pr.numbers.insert_or_assign(e.key, e.value);
            if (d.state == dlc_state::ok && !s.ini.empty()) pr.episodes = episodes_of(decrypt_text(s.ini));
        }
        pr.status.given = std::any_of(p.launch_dlcs.begin(), p.launch_dlcs.end(), [&](const std::filesystem::path& g) { return same_file(g, files[i]); });
        out.push_back(std::move(pr));
    }
    ps3_vfs_unmount_all();
    return out;
}

bool mountable(const dlc_status& d) { return d.state == dlc_state::ok || d.state == dlc_state::missing_rap || d.state == dlc_state::wrong_rap; }

void finish_dlcs(std::vector<dlc_status>& dlcs, const std::string& game_tid)
{
    auto tid_of = [](const dlc_status& d) { return pkg_title_id(d.content_id.c_str()); };
    for (dlc_status& d : dlcs) {
        if (!mountable(d)) continue;
        const std::string tid = tid_of(d);
        d.region = region_of(tid) + (is_supported_title(game_tid) && tid != game_tid ? ", game is " + region_of(game_tid) : std::string());
    }
    auto better = [&](const dlc_status& a, const dlc_status& b) {
        if ((a.state == dlc_state::ok) != (b.state == dlc_state::ok)) return a.state == dlc_state::ok;
        return tid_of(a) == game_tid && tid_of(b) != game_tid;
    };
    for (size_t i = 0; i < dlcs.size(); i++) {
        if (!mountable(dlcs[i])) continue;
        const std::string id = short_id(dlcs[i].content_id);
        size_t w = i;
        for (size_t j = i + 1; j < dlcs.size(); j++)
            if (mountable(dlcs[j]) && short_id(dlcs[j].content_id) == id && better(dlcs[j], dlcs[w])) w = j;
        for (size_t j = i; j < dlcs.size(); j++) {
            if (j == w || !mountable(dlcs[j]) || short_id(dlcs[j].content_id) != id) continue;
            dlcs[j].state = dlc_state::duplicate;
            dlcs[j].detail = "the " + region_of(tid_of(dlcs[w])) + " copy " + name_of(dlcs[w].file) + " is used";
        }
    }
}

std::vector<std::filesystem::path> unused_raps(const std::filesystem::path& exe, const std::vector<dlc_status>& dlcs)
{
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(exdata_dir(exe), std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (lower_ext(it->path()) != ".rap") continue;
        const std::string cid = extract_content_id(utf8(it->path().stem()));
        const bool used = std::any_of(dlcs.begin(), dlcs.end(), [&](const dlc_status& d) {
            return (!cid.empty() && upper_ascii(d.rap_id) == cid) || (!d.rap.empty() && same_file(d.rap, it->path()));
        });
        if (!used) out.push_back(it->path());
    }
    return out;
}

std::string dlc_summary(const std::vector<dlc_status>& dlcs)
{
    size_t ready = 0, locked = 0, unusable = 0, duplicate = 0;
    for (const dlc_status& d : dlcs) {
        if (d.state == dlc_state::ok) ready++;
        else if (d.state == dlc_state::missing_rap || d.state == dlc_state::wrong_rap) locked++;
        else if (d.state == dlc_state::duplicate) duplicate++;
        else unusable++;
    }
    std::string text;
    auto part = [&](size_t n, const char* what) {
        if (n) text += (text.empty() ? "" : ", ") + std::to_string(n) + " " + what;
    };
    part(ready, "ready");
    part(locked, "will not work without its licence");
    part(unusable, "unusable");
    part(duplicate, duplicate == 1 ? "duplicate" : "duplicates");
    return text.empty() ? std::string("None") : text;
}

mounted mount(const std::filesystem::path& exe, const game_paths& p, const std::filesystem::path& root)
{
    mounted m;
    game_check& c = m.check;
    const std::string language = game_language();
    std::vector<dlc_probe> probes = inspect_dlcs(exe, p, root / "dlc_probe", language);
    const std::filesystem::path game_root = root / "dev_hdd0" / "game";
    std::error_code ec;
    std::filesystem::create_directories(game_root, ec);
    const std::string game_dir = utf8(game_root, true);

    std::string game_tid;
    if (p.game.empty()) c.game = { item_state::missing, "Not set" };
    else {
        if (std::filesystem::is_directory(p.game, ec) && !ec) m.bdvd = p.game;
        else if (!ps3_vfs_mount_iso(utf8(root / "dev_bdvd", true).c_str(), utf8(p.game).c_str())) m.bdvd = root / "dev_bdvd";
        const std::string title = m.bdvd.empty() ? std::string() : sfo_string(m.bdvd / "PS3_GAME" / "PARAM.SFO", "TITLE_ID");
        if (title.empty()) c.game = { item_state::bad, "No PS3 game found" };
        else if (!is_supported_title(title)) c.game = { item_state::bad, "Wrong game (" + title + ")" };
        else if (!starts_with(m.bdvd / "PS3_GAME" / "USRDIR" / "EBOOT.BIN", "SCE", 4))
            c.game = { item_state::bad, "Encrypted ISO" };
        else {
            game_tid = title;
            c.title_id = title;
            c.game = { item_state::ok, name_of(p.game) + " (" + title + ")" };
        }
    }

    std::string target_dlc_tid = game_tid;
    if (target_dlc_tid.empty() && !p.update.empty()) {
        char ucid[37] = {};
        int has_eboot = 0;
        if (!ps3_vfs_pkg_info(utf8(p.update).c_str(), ucid, &has_eboot)) {
            const std::string utid = pkg_title_id(ucid);
            if (is_supported_title(utid)) target_dlc_tid = utid;
        }
    }

    const localization loc = load_localization(m.bdvd, language);
    for (dlc_probe& pr : probes) {
        if (!loc.titles.empty()) pr.status.name = dlc_name(pr, loc);
        c.dlcs.push_back(std::move(pr.status));
    }
    finish_dlcs(c.dlcs, target_dlc_tid);
    const char* mount_tid = target_dlc_tid.empty() ? nullptr : target_dlc_tid.c_str();
    for (const dlc_status& d : c.dlcs) {
        char cid[37] = {};
        if (mountable(d)) ps3_vfs_mount_pkg_as(game_dir.c_str(), mount_tid, utf8(d.file).c_str(), cid);
    }
    c.unused_raps = unused_raps(exe, c.dlcs);
    const auto broken = std::find_if(c.dlcs.begin(), c.dlcs.end(), [](const dlc_status& d) {
        return d.state == dlc_state::wrong_game || d.state == dlc_state::unreadable;
    });
    std::vector<const dlc_status*> locked;
    bool licensed = false;
    for (const dlc_status& d : c.dlcs) {
        if (!mountable(d) || !d.needs_rap) continue;
        licensed = true;
        if (d.state != dlc_state::ok) locked.push_back(&d);
    }
    if (broken != c.dlcs.end()) c.dlc = { item_state::bad, (broken->state == dlc_state::wrong_game ? "Wrong game: " : "Unreadable: ") + name_of(broken->file) };
    else c.dlc = { locked.empty() ? item_state::ok : item_state::warn, dlc_summary(c.dlcs) };
    if (!licensed) c.licences = { item_state::ok, "Not needed" };
    else if (locked.empty()) c.licences = { item_state::ok, "All unlocked" };
    else c.licences = { item_state::warn, "Missing for " + locked.front()->name + (locked.size() > 1 ? " and " + std::to_string(locked.size() - 1) + " more" : "") };

    if (p.update.empty()) c.update = { item_state::missing, "Not set" };
    else {
        char cid[37] = {};
        std::vector<uint8_t> self;
        uint8_t* elf = nullptr;
        size_t elf_size = 0;
        const std::string tid = !ps3_vfs_mount_pkg(game_dir.c_str(), utf8(p.update).c_str(), cid) ? pkg_title_id(cid) : std::string();
        const std::filesystem::path title_dir = game_root / (tid.empty() ? std::string(k_title_id_eu) : tid);
        if (tid.empty()) c.update = { item_state::bad, "Unreadable package" };
        else if (!is_supported_title(tid)) c.update = { item_state::bad, "Wrong game (" + tid + ")" };
        else if (!game_tid.empty() && tid != game_tid) c.update = { item_state::bad, "Region mismatch (" + tid + ", game is " + game_tid + ")" };
        else if (!read_all(title_dir / "USRDIR" / "EBOOT.BIN", self) || ps3_self_decrypt(self.data(), self.size(), &elf, &elf_size))
            c.update = { item_state::bad, "Needs update " + std::string(k_update_version) };
        else {
            const bool match = hex(sha256(elf, elf_size)) == eboot_sha256_for(tid);
            if (match) m.elf.assign(elf, elf + elf_size);
            std::free(elf);
            if (c.title_id.empty()) c.title_id = tid;
            const std::string found = sfo_string(title_dir / "PARAM.SFO", "APP_VER");
            c.update = match ? item_status{ item_state::ok, name_of(p.update) }
                             : item_status{ item_state::bad, "Version " + found + ", needs " + k_update_version };
        }
    }

    c.firmware = firmware_installed(firmware_dir(exe, p)) ? item_status{ item_state::ok, "Installed" } : item_status{ item_state::missing, "Not installed" };
    return m;
}

}

bool game_check::ready() const
{
    for (const item_status* s : { &game, &update, &firmware })
        if (s->state != item_state::ok) return false;
    return dlc.state != item_state::bad;
}

bool dlc_blocks(const dlc_status& dlc)
{
    return dlc.state == dlc_state::wrong_game || dlc.state == dlc_state::unreadable ||
           (dlc.given && (dlc.state == dlc_state::missing_rap || dlc.state == dlc_state::wrong_rap));
}

std::vector<report_line> dlc_report(const game_check& check, const std::vector<report_line>& notes)
{
    std::vector<report_line> lines = notes;
    for (const dlc_status& d : check.dlcs)
        lines.push_back({ describe(d), d.state != dlc_state::ok && d.state != dlc_state::duplicate });
    for (const auto& rap : check.unused_raps)
        lines.push_back({ name_of(rap) + k_dash + "UNUSED LICENCE" + k_dash + "matches no added DLC", false });
    return lines;
}

std::filesystem::path firmware_dir(const std::filesystem::path& exe, const game_paths& paths)
{
    return paths.firmware.empty() ? exe / "user" / "dev_flash" : paths.firmware;
}

std::filesystem::path default_update_pkg(const std::filesystem::path& exe, const std::string& title_id)
{
    const char* name = title_id == k_title_id_us ? "UP0102-BLUS30721_00-PATCH02000000000-A0102-V0100-PE.pkg"
                                                 : "EP0102-BLES01227_00-PATCH02000000000-A0102-V0100-PE.pkg";
    return exe / "user" / name;
}

bool write_file_atomic(const std::filesystem::path& path, std::string_view data)
{
    if (std::ifstream in{ path, std::ios::binary }) {
        const std::string current{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        if (!in.bad() && current == data) return true;
    }
    const std::string dst = utf8(path), tmp = dst + ".tmp";
    FILE* f = ps3_utf8_fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = ps3_file_sync(f) == 0 && ok;
    ok = std::fclose(f) == 0 && ok;
    if (ok && ps3_utf8_replace_file(tmp.c_str(), dst.c_str()) == 0) return true;
    ps3_utf8_remove(tmp.c_str());
    return false;
}

game_add_result add_game_items(const std::filesystem::path& exe, game_paths& paths, const std::vector<std::filesystem::path>& items,
                               game_add_target target)
{
    game_add_result res;
    add_context a{ exe, paths, res, target };
    std::error_code ec;
    for (const auto& item : items) {
        if (!std::filesystem::is_directory(item, ec)) {
            sort_file(a, item, true);
            continue;
        }
        const std::filesystem::path root = normalize_game_dir(item);
        if (is_game_folder(root)) {
            use_game(a, root);
            continue;
        }
        a.skipped = 0;
        const auto opts = std::filesystem::directory_options::skip_permission_denied;
        for (auto it = std::filesystem::recursive_directory_iterator(item, opts, ec); !ec && it != std::filesystem::recursive_directory_iterator();
             it.increment(ec)) {
            if (it->is_directory(ec)) {
                if (is_game_folder(it->path())) {
                    use_game(a, it->path());
                    it.disable_recursion_pending();
                }
            } else {
                sort_file(a, it->path(), false);
            }
        }
        if (a.skipped)
            note(a, std::to_string(a.skipped) + (a.skipped == 1 ? " file in " : " files in ") + name_of(strip_trailing_sep(item)) +
                        (a.skipped == 1 ? " is" : " are") + " not a game, update, DLC or licence file" + k_dash + "ignored", false);
    }
    return res;
}

game_check check_game_paths(const std::filesystem::path& exe, const game_paths& paths)
{
    static std::atomic<int> n{ 0 };
    return mount(exe, paths, exe / "user" / "check" / std::to_string(++n)).check;
}

namespace {

bool pkg_matches_title(const std::filesystem::path& pkg, const std::string& tid)
{
    std::error_code ec;
    if (pkg.empty() || !std::filesystem::is_regular_file(pkg, ec)) return false;
    char ucid[37] = {};
    int has_eboot = 0;
    return !ps3_vfs_pkg_info(utf8(pkg).c_str(), ucid, &has_eboot) && has_eboot != 0 && pkg_title_id(ucid) == tid;
}

std::vector<report_line> apply_launch_options(const std::filesystem::path& exe, game_paths& paths, const launch_options& opts,
                                              std::string* out_tid = nullptr, bool* out_encrypted = nullptr)
{
    std::error_code ec;
    if (!opts.game.empty()) paths.game = normalize_game_dir(opts.game);
    if (!opts.update.empty()) paths.update = opts.update;
    std::erase_if(paths.dlcs, [](const std::filesystem::path& d) {
        std::error_code d_ec;
        return !std::filesystem::is_regular_file(d, d_ec);
    });
    const std::filesystem::path chosen_update = paths.update;
    game_add_result res = add_game_items(exe, paths, opts.items, { true, !opts.game.empty(), !opts.update.empty() });
    if (!res.pup.empty()) res.notes.push_back({ name_of(res.pup) + " is PS3 firmware, which Vajra downloads itself" + k_dash + "ignored", false });
    const bool need_update_check = paths.update == chosen_update && opts.update.empty() &&
                                   (!opts.game.empty() || (!paths.update.empty() && !std::filesystem::is_regular_file(paths.update, ec)));
    std::string detected_tid;
    if ((need_update_check || out_tid) && !paths.game.empty())
        detected_tid = detect_game_title(exe, paths.game, out_encrypted);
    if (out_tid) *out_tid = detected_tid;
    if (need_update_check) {
        const std::string want_tid = is_supported_title(detected_tid) ? detected_tid : k_title_id_eu;
        if (!pkg_matches_title(paths.update, want_tid)) {
            const std::filesystem::path default_pkg = default_update_pkg(exe, want_tid);
            paths.update = update_pkg_complete(default_pkg) ? default_pkg : std::filesystem::path{};
        }
    }
    return std::move(res.notes);
}

bool run_with_progress(float start_p, float end_p, const char* label, const std::function<bool(std::atomic<float>&, std::string&)>& fn, std::string& error)
{
    std::atomic<float> p{ 0.f };
    std::atomic<bool> done{ false };
    bool ok = false;
    std::thread worker([&] {
        ok = fn(p, error);
        done.store(true, std::memory_order_release);
    });
    int last_pct = -1;
    while (!done.load(std::memory_order_acquire)) {
        const float cur = std::clamp(p.load(std::memory_order_relaxed), 0.f, 1.f);
        const float mapped = start_p + (end_p - start_p) * cur;
        const int pct = (int)(mapped * 100.f);
        if (pct != last_pct) {
            std::printf("progress %.2f %s\n", mapped, label);
            std::fflush(stdout);
            last_pct = pct;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    worker.join();
    if (ok) {
        std::printf("progress %.2f %s\n", end_p, label);
        std::fflush(stdout);
    }
    return ok;
}

}

int prepare_game_files(const std::filesystem::path& exe, const launch_options& opts)
{
    std::error_code ec;
    std::filesystem::create_directories(exe / "user", ec);
    game_paths paths = read_paths(exe, opts.launcher);
    std::string detected_tid;
    bool game_encrypted = false;
    const std::vector<report_line> notes = apply_launch_options(exe, paths, opts, &detected_tid, &game_encrypted);

    std::printf("progress 0.05 Checking game files\n");
    std::fflush(stdout);

    if (paths.game.empty()) {
        std::fprintf(stderr, "No game path provided. Pass --game=<path> to a BLES01227 or BLUS30721 folder or decrypted ISO.\n");
        return 3;
    }

    if (detected_tid.empty()) {
        std::fprintf(stderr, "Game check failed: No PS3 game found at %s\n", utf8(paths.game).c_str());
        return 6;
    }
    if (!is_supported_title(detected_tid)) {
        std::fprintf(stderr, "Game check failed: Wrong game (%s, needs BLES01227 or BLUS30721)\n", detected_tid.c_str());
        return 6;
    }
    if (game_encrypted) {
        std::fprintf(stderr, "Game check failed: Encrypted ISO (needs a decrypted BLES01227 or BLUS30721 dump)\n");
        return 6;
    }

    const std::filesystem::path default_pkg = default_update_pkg(exe, detected_tid);
    const bool update_matches_region = pkg_matches_title(paths.update, detected_tid);
    if (!update_matches_region && !opts.update.empty()) {
        std::fprintf(stderr, "Update 1.02 check failed: Package is not a valid 1.02 update for game region (%s)\n", detected_tid.c_str());
        return 6;
    }
    if (!update_matches_region && opts.update.empty()) {
        if (update_pkg_complete(default_pkg)) {
            paths.update = default_pkg;
        } else {
            std::string error;
            const bool ok = run_with_progress(0.10f, 0.45f, "Downloading official 1.02 update from Sony",
                [&](std::atomic<float>& p, std::string& err) { return update_download(default_pkg, p, err, nullptr, detected_tid); }, error);
            if (!ok) {
                std::fprintf(stderr, "Failed to download 1.02 update: %s\n", error.c_str());
                return 4;
            }
            paths.update = default_pkg;
        }
    }

    const std::filesystem::path flash = firmware_dir(exe, paths);
    if (!firmware_installed(flash)) {
        const std::filesystem::path pup = exe / "user" / "PS3UPDAT.PUP";
        std::string error;
        bool ok = run_with_progress(0.45f, 0.80f, "Downloading PS3 firmware from Sony",
            [&](std::atomic<float>& p, std::string& err) { return firmware_download(pup, p, err); }, error);
        if (ok) {
            ok = run_with_progress(0.80f, 0.94f, "Extracting PS3 audio codecs",
                [&](std::atomic<float>& p, std::string& err) { return firmware_extract(pup, flash, p, err); }, error);
        }
        std::filesystem::remove(pup, ec);
        if (!ok) {
            std::fprintf(stderr, "Failed to install PS3 firmware: %s\n", error.c_str());
            return 5;
        }
    }

    std::printf("progress 0.95 Verifying game, update and DLC\n");
    std::fflush(stdout);
    mounted m = mount(exe, paths, exe / "user");
    const std::vector<report_line> report = dlc_report(m.check, notes);
    for (const bool problems : { false, true })
        for (const report_line& line : report)
            if (line.problem == problems) std::fprintf(stderr, "%s\n", line.text.c_str());
    if (m.check.game.state != item_state::ok || m.check.update.state != item_state::ok || m.check.firmware.state != item_state::ok) {
        if (m.check.game.state != item_state::ok) std::fprintf(stderr, "Game check failed: %s\n", m.check.game.text.c_str());
        if (m.check.update.state != item_state::ok) std::fprintf(stderr, "Update 1.02 check failed: %s\n", m.check.update.text.c_str());
        if (m.check.firmware.state != item_state::ok) std::fprintf(stderr, "Firmware check failed: %s\n", m.check.firmware.text.c_str());
        return 6;
    }
    std::string unusable;
    auto add_unusable = [&](const std::string& what) { unusable += (unusable.empty() ? "" : "; ") + what; };
    for (const report_line& line : notes)
        if (line.problem) add_unusable(line.text.substr(0, line.text.find(k_dash)));
    for (const dlc_status& d : m.check.dlcs)
        if (dlc_blocks(d)) add_unusable(d.name + (d.state == dlc_state::missing_rap || d.state == dlc_state::wrong_rap ? " needs its .rap in " + utf8(d.file.parent_path()) : std::string(" is unusable")));
    if (!unusable.empty()) {
        std::fprintf(stderr, "These DLC will not work: %s. Add the missing .rap files, or remove those DLC, then set up again.\n", unusable.c_str());
        return 7;
    }

    write_paths(exe, paths);
    std::filesystem::remove_all(exe / "user" / "check", ec);
    if (m.check.dlcs.empty()) std::printf("progress 1.00 Ready\n");
    else if (m.check.licences.state == item_state::warn) std::printf("progress 1.00 Ready. DLC: %s. %s\n", m.check.dlc.text.c_str(), m.check.licences.text.c_str());
    else std::printf("progress 1.00 Ready. DLC: %s\n", m.check.dlc.text.c_str());
    std::fflush(stdout);
    return 0;
}

bool load_game_files(const std::filesystem::path& exe, const std::filesystem::path& icon, game_files& out, const launch_options& opts)
{
    game_paths paths = read_paths(exe, opts.launcher);
    const std::vector<report_line> notes = apply_launch_options(exe, paths, opts);
    std::error_code ec;
    out.user = exe / "user";
    out.hdd0 = out.user / "dev_hdd0";
    out.hdd1 = out.user / "dev_hdd1";
    std::filesystem::create_directories(out.hdd1, ec);

    mounted m = mount(exe, paths, out.user);
    if (!m.check.ready() || m.check.licences.state == item_state::warn) {
        const bool ok = run_setup(exe, icon, paths, m.check, notes);
        std::filesystem::remove_all(exe / "user" / "check", ec);
        if (!ok) return false;
        m = mount(exe, paths, out.user);
        if (!m.check.ready()) return false;
    }
    write_paths(exe, paths);
    for (const report_line& line : dlc_report(m.check, notes)) std::fprintf(stderr, "[dlc] %s\n", line.text.c_str());
    out.flash = firmware_dir(exe, paths);
    out.bdvd = m.bdvd;
    out.elf = std::move(m.elf);
    return true;
}
