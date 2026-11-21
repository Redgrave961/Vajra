#include "game_files.h"

#include "firmware.h"
#include "setup_window.h"
#include "../ps3recomp/libs/filesystem/ps3_self.h"
#include "../ps3recomp/libs/filesystem/ps3_vfs.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {

constexpr const char* k_title_id = "BLES01227";
constexpr const char* k_update_version = "01.02";

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
    ps3_vstat st;
    ps3_vfile_fstat(f, &st);
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
    for (uint32_t i = 0; i < count && 0x14 + (size_t)i * 16 + 16 <= d.size(); i++) {
        const size_t e = 0x14 + (size_t)i * 16;
        const size_t name = keys + (size_t)(d[e] | d[e + 1] << 8), value = values + le32(e + 12), len = le32(e + 4);
        if (name >= d.size() || value + len > d.size()) break;
        if (!std::strcmp(reinterpret_cast<const char*>(d.data() + name), key))
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

bool is_our_iso(const std::filesystem::path& exe, const std::filesystem::path& iso)
{
    const std::filesystem::path probe = exe / "user" / "check" / "probe";
    ps3_vfs_unmount_all();
    const bool ok = !ps3_vfs_mount_iso(utf8(probe, true).c_str(), utf8(iso).c_str()) &&
                    sfo_string(probe / "PS3_GAME" / "PARAM.SFO", "TITLE_ID") == k_title_id;
    ps3_vfs_unmount_all();
    return ok;
}

void sort_file(const std::filesystem::path& exe, game_paths& paths, const std::filesystem::path& file, std::filesystem::path& pup)
{
    std::string ext = utf8(file.extension());
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (ext == ".iso") {
        if (is_our_iso(exe, file)) paths.game = file;
    } else if (ext == ".pkg") {
        char cid[37];
        int has_eboot = 0;
        if (ps3_vfs_pkg_info(utf8(file).c_str(), cid, &has_eboot) || std::string(cid + 7, 9) != k_title_id) return;
        if (has_eboot) paths.update = file;
        else if (std::find(paths.dlcs.begin(), paths.dlcs.end(), file) == paths.dlcs.end()) paths.dlcs.push_back(file);
    } else if (ext == ".rap") {
        std::error_code ec;
        std::filesystem::create_directories(exdata_dir(exe), ec);
        std::filesystem::copy_file(file, exdata_dir(exe) / file.filename(), std::filesystem::copy_options::overwrite_existing, ec);
    } else if (ext == ".pup") {
        pup = file;
    }
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

    if (p.game.empty()) c.game = { item_state::missing, "Not set" };
    else {
        if (std::filesystem::is_directory(p.game)) m.bdvd = p.game;
        else if (!ps3_vfs_mount_iso(utf8(root / "dev_bdvd", true).c_str(), utf8(p.game).c_str())) m.bdvd = root / "dev_bdvd";
        const std::string title = m.bdvd.empty() ? std::string() : sfo_string(m.bdvd / "PS3_GAME" / "PARAM.SFO", "TITLE_ID");
        if (title.empty()) c.game = { item_state::bad, "No PS3 game found" };
        else if (title != k_title_id) c.game = { item_state::bad, "Wrong game (" + title + ")" };
        else if (!starts_with(m.bdvd / "PS3_GAME" / "USRDIR" / "EBOOT.BIN", "SCE", 4))
            c.game = { item_state::bad, "Encrypted ISO" };
        else c.game = { item_state::ok, utf8(p.game.filename()) };
    }

    if (p.update.empty()) c.update = { item_state::missing, "Not set" };
    else {
        char cid[37];
        std::vector<uint8_t> self;
        const std::filesystem::path title_dir = game_root / k_title_id;
        uint8_t* elf = nullptr;
        size_t elf_size = 0;
        if (ps3_vfs_mount_pkg(game_dir.c_str(), utf8(p.update).c_str(), cid)) c.update = { item_state::bad, "Unreadable package" };
        else if (std::string(cid + 7, 9) != k_title_id) c.update = { item_state::bad, "Wrong game (" + std::string(cid + 7, 9) + ")" };
        else if (!read_all(title_dir / "USRDIR" / "EBOOT.BIN", self) || ps3_self_decrypt(self.data(), self.size(), &elf, &elf_size))
            c.update = { item_state::bad, "Needs update " + std::string(k_update_version) };
        else {
            const bool match = hex(sha256(elf, elf_size)) == PORT_EBOOT_SHA256;
            if (match) m.elf.assign(elf, elf + elf_size);
            std::free(elf);
            const std::string found = sfo_string(title_dir / "PARAM.SFO", "APP_VER");
            c.update = match ? item_status{ item_state::ok, utf8(p.update.filename()) }
                             : item_status{ item_state::bad, "Version " + found + ", needs " + k_update_version };
        }
    }

    std::string locked;
    c.dlc = { item_state::ok, p.dlcs.empty() ? "None" : std::to_string(p.dlcs.size()) + " package" + (p.dlcs.size() == 1 ? "" : "s") };
    for (const auto& d : p.dlcs) {
        char cid[37];
        if (ps3_vfs_mount_pkg(game_dir.c_str(), utf8(d).c_str(), cid)) {
            c.dlc = { item_state::bad, "Unreadable: " + utf8(d.filename()) };
            continue;
        }
        if (std::string(cid + 7, 9) != k_title_id) {
            c.dlc = { item_state::bad, "Wrong game: " + utf8(d.filename()) };
            continue;
        }
        const std::string rap = std::string(cid) + ".rap";
        if (std::filesystem::exists(exdata / rap)) continue;
        locked += (locked.empty() ? "" : ", ") + short_id(cid);
    }
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

std::filesystem::path add_game_items(const std::filesystem::path& exe, game_paths& paths, const std::vector<std::filesystem::path>& items)
{
    std::filesystem::path pup;
    std::error_code ec;
    for (const auto& item : items) {
        if (!std::filesystem::is_directory(item, ec)) {
            sort_file(exe, paths, item, pup);
            continue;
        }
        const std::filesystem::path root = item.filename() == "PS3_GAME" ? item.parent_path() : item;
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
                sort_file(exe, paths, it->path(), pup);
            }
        }
    }
    return pup;
}

game_check check_game_paths(const std::filesystem::path& exe, const game_paths& paths)
{
    static int n = 0;
    return mount(exe, paths, exe / "user" / "check" / std::to_string(++n)).check;
}

bool load_game_files(const std::filesystem::path& exe, const std::filesystem::path& icon, game_files& out)
{
    game_paths paths = read_paths(exe);
    const game_check first = check_game_paths(exe, paths);
    if (!first.ready()) {
        if (!run_setup(exe, icon, paths, first)) return false;
        write_paths(exe, paths);
    }
    std::error_code ec;
    std::filesystem::remove_all(exe / "user" / "check", ec);

    out.user = exe / "user";
    out.hdd0 = out.user / "dev_hdd0";
    out.hdd1 = out.user / "dev_hdd1";
    out.flash = firmware_dir(exe, paths);
    std::filesystem::create_directories(out.hdd1, ec);
    mounted m = mount(exe, paths, out.user);
    if (!m.check.ready()) return false;
    out.bdvd = m.bdvd;
    out.elf = std::move(m.elf);
    return true;
}
