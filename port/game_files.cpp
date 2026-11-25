#include "game_files.h"

#include "firmware.h"
#include "setup_window.h"
#include "../ps3recomp/libs/filesystem/npdrm.h"
#include "../ps3recomp/libs/filesystem/ps3_self.h"
#include "../ps3recomp/libs/filesystem/ps3_vfs.h"

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
#include <thread>

namespace {

constexpr const char* k_title_id_eu = "BLES01227";
constexpr const char* k_title_id_us = "BLUS30721";
constexpr const char* k_eboot_sha256_us = "065c1f0c5f6203c4007730dbbd53edcf9f80d242ee78ab91d8e69198b723a342";
constexpr const char* k_update_version = "01.02";

bool is_supported_title(const std::string& tid) { return tid == k_title_id_eu || tid == k_title_id_us; }

const char* eboot_sha256_for(const std::string& tid)
{
    if (tid == k_title_id_eu) return PORT_EBOOT_SHA256;
    if (tid == k_title_id_us) return k_eboot_sha256_us;
    return "";
}

std::string utf8(const std::filesystem::path& p, bool generic = false)
{
    const std::u8string s = generic ? p.generic_u8string() : p.u8string();
    return std::string(s.begin(), s.end());
}

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

std::filesystem::path path_of(const std::string& value) { return std::filesystem::path(std::u8string(value.begin(), value.end())); }

game_paths read_paths(const std::filesystem::path& exe)
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
        else if (key == "dlc") p.dlcs.push_back(path_of(value));
        else if (key == "firmware") p.firmware = path_of(value);
    }
    return p;
}

void write_paths(const std::filesystem::path& exe, const game_paths& p)
{
    std::ofstream ini(exe / "asura_paths.ini", std::ios::binary);
    auto line = [&](const char* key, const std::filesystem::path& v) {
        if (!v.empty()) ini << key << '=' << utf8(v) << '\n';
    };
    line("game", p.game);
    line("update", p.update);
    for (const auto& d : p.dlcs) line("dlc", d);
    line("firmware", p.firmware);
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

std::filesystem::path normalize_game_dir(std::filesystem::path p)
{
    p = strip_trailing_sep(std::move(p));
    std::string fn = utf8(p.filename());
    for (char& c : fn) c = (char)std::toupper((unsigned char)c);
    if (fn == "PS3_GAME") p = p.parent_path();
    std::error_code ec;
    if (std::filesystem::is_directory(p, ec) && !is_game_folder(p)) {
        for (auto it = std::filesystem::directory_iterator(p, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            if (it->is_directory(ec) && is_game_folder(it->path())) return it->path();
        }
    }
    return p;
}

bool is_our_iso(const std::filesystem::path& exe, const std::filesystem::path& iso)
{
    const std::filesystem::path probe = exe / "user" / "check" / "probe";
    ps3_vfs_unmount_all();
    const bool ok = !ps3_vfs_mount_iso(utf8(probe, true).c_str(), utf8(iso).c_str()) &&
                    is_supported_title(sfo_string(probe / "PS3_GAME" / "PARAM.SFO", "TITLE_ID"));
    ps3_vfs_unmount_all();
    return ok;
}

std::string detect_game_title(const std::filesystem::path& exe, const std::filesystem::path& game)
{
    if (game.empty()) return {};
    std::error_code ec;
    if (std::filesystem::is_directory(game, ec) && !ec)
        return sfo_string(game / "PS3_GAME" / "PARAM.SFO", "TITLE_ID");
    const std::filesystem::path probe = exe / "user" / "check" / "probe_tid";
    ps3_vfs_unmount_all();
    std::string tid;
    if (!ps3_vfs_mount_iso(utf8(probe, true).c_str(), utf8(game).c_str()))
        tid = sfo_string(probe / "PS3_GAME" / "PARAM.SFO", "TITLE_ID");
    ps3_vfs_unmount_all();
    return tid;
}

std::string upper_ascii(std::string s)
{
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
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

bool is_valid_update_pkg(const std::filesystem::path& exe, const std::filesystem::path& file)
{
    const std::filesystem::path probe = exe / "user" / "check" / "probe_upd";
    ps3_vfs_unmount_all();
    char cid[37] = {};
    bool match = false;
    if (!ps3_vfs_mount_pkg(utf8(probe, true).c_str(), utf8(file).c_str(), cid)) {
        const std::string tid = pkg_title_id(cid);
        if (is_supported_title(tid)) {
            std::vector<uint8_t> self;
            uint8_t* elf = nullptr;
            size_t elf_size = 0;
            if (read_all(probe / tid / "USRDIR" / "EBOOT.BIN", self) && !ps3_self_decrypt(self.data(), self.size(), &elf, &elf_size)) {
                match = hex(sha256(elf, elf_size)) == eboot_sha256_for(tid);
                std::free(elf);
            }
        }
    }
    ps3_vfs_unmount_all();
    return match;
}

void sort_file(const std::filesystem::path& exe, game_paths& paths, const std::filesystem::path& file, game_add_result& res, game_add_mode mode)
{
    std::string ext = utf8(file.extension());
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    std::error_code ec;
    if (ext == ".iso") {
        if (is_our_iso(exe, file)) paths.game = file;
    } else if (ext == ".pkg") {
        char cid[37] = {};
        int has_eboot = 0;
        if (ps3_vfs_pkg_info(utf8(file).c_str(), cid, &has_eboot)) {
            res.dlc_notice = "Unreadable package: " + utf8(file.filename());
            return;
        }
        const std::string tid = pkg_title_id(cid);
        if (!is_supported_title(tid)) {
            res.dlc_notice = "Wrong game (" + (tid.empty() ? std::string("unknown") : tid) + ", needs BLES01227 or BLUS30721): " + utf8(file.filename());
            return;
        }
        if (has_eboot && mode == game_add_mode::any && is_valid_update_pkg(exe, file)) {
            paths.update = file;
        } else {
            auto already = std::find_if(paths.dlcs.begin(), paths.dlcs.end(), [&](const std::filesystem::path& existing) {
                if (existing == file) return true;
                std::error_code eq_ec;
                return std::filesystem::equivalent(existing, file, eq_ec) && !eq_ec;
            });
            if (already == paths.dlcs.end()) paths.dlcs.push_back(file);
        }
    } else if (ext == ".rap" || mode == game_add_mode::rap) {
        std::array<uint8_t, 16> rap{};
        if (!read_rap_bytes(file, rap)) {
            res.rap_notice = "Invalid .rap (expected 16 bytes or 32 hex chars): " + utf8(file.filename());
            return;
        }
        const std::filesystem::path exdata = exdata_dir(exe);
        std::filesystem::create_directories(exdata, ec);
        const std::string embedded_cid = extract_content_id(utf8(file.stem()));
        const std::string tag = hex(sha256(rap.data(), rap.size())).substr(0, 16);
        const std::string stem = utf8(file.stem());
        const std::string target_name = !embedded_cid.empty() ? (embedded_cid + ".rap")
                                                              : ((stem.empty() ? std::string("rap") : stem) + "_" + tag + ".rap");
        const std::filesystem::path dst = exdata / path_of(target_name);
        if (std::filesystem::exists(dst, ec)) {
            std::filesystem::permissions(dst, std::filesystem::perms::owner_write, std::filesystem::perm_options::add, ec);
        }
        std::ofstream out(dst, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(rap.data()), (std::streamsize)rap.size());
        if (!out) res.rap_notice = "Failed to save .rap: " + utf8(file.filename());
    } else if (ext == ".pup") {
        res.pup = file;
    }
}

struct pkg_edat_check {
    std::string exdata;
    std::vector<std::string> missing;
    std::vector<std::pair<std::string, std::string>> locked_files;
};

void check_pkg_entry(const char* full_path, void* user)
{
    auto* ctx = static_cast<pkg_edat_check*>(user);
    ps3_vfile* f = ps3_vfile_open(full_path, "rb");
    if (!f) return;
    char cid[49] = {};
    if (npdrm_check_vfile(f, ctx->exdata.c_str(), cid) && cid[0]) {
        const std::string s(cid);
        if (std::find(ctx->missing.begin(), ctx->missing.end(), s) == ctx->missing.end()) ctx->missing.push_back(s);
        ctx->locked_files.emplace_back(full_path, s);
    }
    ps3_vfile_close(f);
}

void auto_import_nearby_raps(const std::filesystem::path& exe, const game_paths& p, pkg_edat_check& edat_check)
{
    if (edat_check.locked_files.empty()) return;
    std::error_code ec;
    std::vector<std::filesystem::path> dirs;
    auto push_dir = [&](const std::filesystem::path& d) {
        if (d.empty() || !std::filesystem::is_directory(d, ec)) return;
        for (const auto& existing : dirs) {
            if (existing == d) return;
            std::error_code eq_ec;
            if (std::filesystem::equivalent(existing, d, eq_ec) && !eq_ec) return;
        }
        dirs.push_back(d);
    };
    push_dir(exe / "exdata");
    for (const auto& d : p.dlcs) {
        const std::filesystem::path par = d.parent_path();
        push_dir(par);
        push_dir(par / "exdata");
        push_dir(par / "rap");
        push_dir(par / "raps");
        if (par.has_parent_path()) {
            push_dir(par.parent_path() / "exdata");
            push_dir(par.parent_path() / "rap");
            push_dir(par.parent_path() / "raps");
        }
    }
    if (!p.game.empty()) {
        push_dir(p.game.parent_path() / "exdata");
        if (std::filesystem::is_directory(p.game, ec)) push_dir(p.game / "exdata");
    }
    if (dirs.empty()) return;

    const std::filesystem::path exdata = exdata_dir(exe);
    std::vector<std::string> still_missing;
    for (const auto& [full_path, cid] : edat_check.locked_files) {
        ps3_vfile* f = ps3_vfile_open(full_path.c_str(), "rb");
        if (!f) {
            if (std::find(still_missing.begin(), still_missing.end(), cid) == still_missing.end()) still_missing.push_back(cid);
            continue;
        }
        bool unlocked = npdrm_check_vfile(f, edat_check.exdata.c_str(), nullptr) == 0;
        for (const auto& cand_dir : dirs) {
            if (unlocked) break;
            uint8_t rap[16];
            char found_cid[49] = {};
            if (npdrm_find_rap_in_dir(f, utf8(cand_dir, true).c_str(), rap, found_cid)) {
                std::filesystem::create_directories(exdata, ec);
                const std::string out_cid = found_cid[0] ? std::string(found_cid) : cid;
                const std::filesystem::path dst = exdata / path_of(out_cid + ".rap");
                if (std::filesystem::exists(dst, ec))
                    std::filesystem::permissions(dst, std::filesystem::perms::owner_write, std::filesystem::perm_options::add, ec);
                std::ofstream out(dst, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(rap), 16);
                unlocked = true;
            }
        }
        ps3_vfile_close(f);
        if (!unlocked && std::find(still_missing.begin(), still_missing.end(), cid) == still_missing.end())
            still_missing.push_back(cid);
    }
    edat_check.missing = std::move(still_missing);
}

mounted mount(const std::filesystem::path& exe, const game_paths& p, const std::filesystem::path& root)
{
    ps3_vfs_unmount_all();
    mounted m;
    game_check& c = m.check;
    const std::filesystem::path exdata = exdata_dir(exe);
    const std::filesystem::path game_root = root / "dev_hdd0" / "game";
    std::error_code ec;
    std::filesystem::create_directories(game_root, ec);
    const std::string game_dir = utf8(game_root, true);
    const std::string exdata_str = utf8(exdata, true);

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
            c.game = { item_state::ok, utf8(p.game.filename()) + " (" + title + ")" };
        }
    }

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
            c.update = match ? item_status{ item_state::ok, utf8(p.update.filename()) }
                             : item_status{ item_state::bad, "Version " + found + ", needs " + k_update_version };
        }
    }

    pkg_edat_check edat_check{ exdata_str, {}, {} };
    c.dlc = { item_state::ok, p.dlcs.empty() ? "None" : std::to_string(p.dlcs.size()) + " package" + (p.dlcs.size() == 1 ? "" : "s") };
    const std::string expected_dlc_tid = !game_tid.empty() ? game_tid : c.title_id;
    for (const auto& d : p.dlcs) {
        char cid[37] = {};
        if (ps3_vfs_mount_pkg(game_dir.c_str(), utf8(d).c_str(), cid)) {
            c.dlc = { item_state::bad, "Unreadable: " + utf8(d.filename()) };
            continue;
        }
        const std::string tid = pkg_title_id(cid);
        if (!is_supported_title(tid) || (!expected_dlc_tid.empty() && tid != expected_dlc_tid)) {
            c.dlc = { item_state::bad, "Wrong region (" + tid + (expected_dlc_tid.empty() ? "" : ", needs " + expected_dlc_tid) + "): " + utf8(d.filename()) };
            continue;
        }
        ps3_vfs_enum_last_mount(check_pkg_entry, &edat_check);
    }
    auto_import_nearby_raps(exe, p, edat_check);
    std::string locked;
    for (const std::string& mcid : edat_check.missing)
        locked += (locked.empty() ? "" : ", ") + short_id(mcid);
    if (p.dlcs.empty()) c.licences = { item_state::ok, "Not needed" };
    else if (locked.empty()) c.licences = { item_state::ok, "All unlocked" };
    else c.licences = { item_state::warn, "Missing: " + locked };

    c.firmware = firmware_installed(firmware_dir(exe, p)) ? item_status{ item_state::ok, "Installed" } : item_status{ item_state::missing, "Not installed" };
    return m;
}

}

bool game_check::ready() const
{
    for (const item_status* s : { &game, &update, &dlc, &firmware })
        if (s->state != item_state::ok) return false;
    return true;
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

game_add_result add_game_items(const std::filesystem::path& exe, game_paths& paths, const std::vector<std::filesystem::path>& items,
                               game_add_mode mode)
{
    game_add_result res;
    std::error_code ec;
    for (const auto& item : items) {
        if (!std::filesystem::is_directory(item, ec)) {
            sort_file(exe, paths, item, res, mode);
            continue;
        }
        const std::filesystem::path root = normalize_game_dir(item);
        if (is_game_folder(root)) {
            paths.game = root;
            continue;
        }
        const auto opts = std::filesystem::directory_options::skip_permission_denied;
        for (auto it = std::filesystem::recursive_directory_iterator(item, opts, ec); !ec && it != std::filesystem::recursive_directory_iterator();
             it.increment(ec)) {
            if (it->is_directory(ec)) {
                if (is_game_folder(it->path())) {
                    paths.game = it->path();
                    it.disable_recursion_pending();
                }
            } else {
                sort_file(exe, paths, it->path(), res, mode);
            }
        }
    }
    return res;
}

game_check check_game_paths(const std::filesystem::path& exe, const game_paths& paths)
{
    static int n = 0;
    return mount(exe, paths, exe / "user" / "check" / std::to_string(++n)).check;
}

namespace {

void apply_launch_options(const std::filesystem::path& exe, game_paths& paths, const launch_options& opts)
{
    std::error_code ec;
    if (!opts.game.empty())
        paths.game = normalize_game_dir(opts.game);
    if (!opts.update.empty()) {
        paths.update = opts.update;
    } else if (!paths.update.empty() && !std::filesystem::is_regular_file(paths.update, ec)) {
        const std::string tid = detect_game_title(exe, paths.game);
        const std::filesystem::path default_pkg = default_update_pkg(exe, is_supported_title(tid) ? tid : k_title_id_eu);
        paths.update = std::filesystem::is_regular_file(default_pkg, ec) ? default_pkg : std::filesystem::path{};
    }
    if (!opts.game.empty() || !opts.dlcs.empty()) {
        paths.dlcs.erase(std::remove_if(paths.dlcs.begin(), paths.dlcs.end(), [&](const std::filesystem::path& d) {
            std::error_code d_ec;
            return !std::filesystem::is_regular_file(d, d_ec);
        }), paths.dlcs.end());
    }
    if (!opts.dlcs.empty())
        add_game_items(exe, paths, opts.dlcs, game_add_mode::dlc);
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
    game_paths paths = read_paths(exe);
    apply_launch_options(exe, paths, opts);

    std::printf("progress 0.05 Checking game files\n");
    std::fflush(stdout);

    if (paths.game.empty()) {
        std::fprintf(stderr, "No game path provided. Pass --game=<path> to a BLES01227 or BLUS30721 folder or decrypted ISO.\n");
        return 3;
    }

    const std::string detected_tid = detect_game_title(exe, paths.game);
    const std::string active_tid = is_supported_title(detected_tid) ? detected_tid : k_title_id_eu;
    const std::filesystem::path default_pkg = default_update_pkg(exe, active_tid);
    bool update_matches_region = false;
    if (!paths.update.empty() && std::filesystem::is_regular_file(paths.update, ec)) {
        char ucid[37] = {};
        int has_eboot = 0;
        if (!ps3_vfs_pkg_info(utf8(paths.update).c_str(), ucid, &has_eboot))
            update_matches_region = pkg_title_id(ucid) == active_tid;
    }
    if (!update_matches_region && opts.update.empty()) {
        if (std::filesystem::is_regular_file(default_pkg, ec)) {
            paths.update = default_pkg;
        } else {
            std::string error;
            const bool ok = run_with_progress(0.10f, 0.45f, "Downloading official 1.02 update from Sony",
                [&](std::atomic<float>& p, std::string& err) { return update_download(default_pkg, p, err, nullptr, active_tid); }, error);
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

    std::printf("progress 0.95 Verifying game and update\n");
    std::fflush(stdout);
    mounted m = mount(exe, paths, exe / "user");
    if (!m.check.ready()) {
        if (m.check.game.state != item_state::ok) std::fprintf(stderr, "Game check failed: %s\n", m.check.game.text.c_str());
        if (m.check.update.state != item_state::ok) std::fprintf(stderr, "Update 1.02 check failed: %s\n", m.check.update.text.c_str());
        if (m.check.dlc.state != item_state::ok) std::fprintf(stderr, "DLC check failed: %s\n", m.check.dlc.text.c_str());
        if (m.check.firmware.state != item_state::ok) std::fprintf(stderr, "Firmware check failed: %s\n", m.check.firmware.text.c_str());
        return 6;
    }

    write_paths(exe, paths);
    std::filesystem::remove_all(exe / "user" / "check", ec);
    std::printf("progress 1.00 Ready\n");
    std::fflush(stdout);
    return 0;
}

bool load_game_files(const std::filesystem::path& exe, const std::filesystem::path& icon, game_files& out, const launch_options& opts)
{
    game_paths paths = read_paths(exe);
    const bool cli_override = !opts.game.empty() || !opts.update.empty() || !opts.dlcs.empty();
    apply_launch_options(exe, paths, opts);
    std::error_code ec;
    out.user = exe / "user";
    out.hdd0 = out.user / "dev_hdd0";
    out.hdd1 = out.user / "dev_hdd1";
    std::filesystem::create_directories(out.hdd1, ec);

    mounted m = mount(exe, paths, out.user);
    if (!m.check.ready()) {
        const bool ok = run_setup(exe, icon, paths, m.check);
        std::filesystem::remove_all(exe / "user" / "check", ec);
        if (!ok) return false;
        write_paths(exe, paths);
        m = mount(exe, paths, out.user);
        if (!m.check.ready()) return false;
    } else if (cli_override) {
        write_paths(exe, paths);
    }
    out.flash = firmware_dir(exe, paths);
    out.bdvd = m.bdvd;
    out.elf = std::move(m.elf);
    return true;
}
