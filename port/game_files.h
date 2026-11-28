#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct game_files {
    std::filesystem::path bdvd, hdd0, hdd1, user, flash;
    std::vector<uint8_t> elf;
};

struct game_paths {
    std::filesystem::path game, update, firmware;
    std::vector<std::filesystem::path> dlcs;
    std::vector<std::filesystem::path> launch_dlcs;
};

enum class item_state { ok, warn, bad, missing };

struct item_status {
    item_state state = item_state::missing;
    std::string text;
};

enum class dlc_state { ok, missing_rap, wrong_rap, wrong_game, unreadable, duplicate };

struct dlc_status {
    std::filesystem::path file;
    std::string name, content_id, region, rap_id, detail;
    std::filesystem::path rap;
    dlc_state state = dlc_state::ok;
    bool needs_rap = false;
    bool given = false;
};

struct report_line {
    std::string text;
    bool problem = false;
};

struct game_check {
    item_status game, update, dlc, licences, firmware;
    std::string title_id;
    std::vector<dlc_status> dlcs;
    std::vector<std::filesystem::path> unused_raps;
    bool ready() const;
};

struct launch_options {
    std::filesystem::path game;
    std::filesystem::path update;
    std::vector<std::filesystem::path> items;
    bool prepare = false;
    bool launcher = false;
};

struct game_add_target {
    bool launch = false;
    bool game_fixed = false;
    bool update_fixed = false;
};

struct game_add_result {
    std::filesystem::path pup;
    std::vector<report_line> notes;
};

game_add_result add_game_items(const std::filesystem::path& exe, game_paths& paths, const std::vector<std::filesystem::path>& items,
                               game_add_target target = {});
game_check check_game_paths(const std::filesystem::path& exe, const game_paths& paths);
std::vector<report_line> dlc_report(const game_check& check, const std::vector<report_line>& notes);
bool dlc_blocks(const dlc_status& dlc);
std::filesystem::path firmware_dir(const std::filesystem::path& exe, const game_paths& paths);
std::filesystem::path default_update_pkg(const std::filesystem::path& exe, const std::string& title_id);
bool write_file_atomic(const std::filesystem::path& path, std::string_view data);
inline std::string utf8_from_path(const std::filesystem::path& p, bool generic = false)
{
    const std::u8string s = generic ? p.generic_u8string() : p.u8string();
    return std::string(s.begin(), s.end());
}

inline std::filesystem::path path_from_utf8(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

int prepare_game_files(const std::filesystem::path& exe, const launch_options& opts);
bool load_game_files(const std::filesystem::path& exe, const std::filesystem::path& icon, game_files& out, const launch_options& opts = {});
