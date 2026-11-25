#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct game_files {
    std::filesystem::path bdvd, hdd0, hdd1, user, flash;
    std::vector<uint8_t> elf;
};

struct game_paths {
    std::filesystem::path game, update, firmware;
    std::vector<std::filesystem::path> dlcs;
};

enum class item_state { ok, warn, bad, missing };

struct item_status {
    item_state state = item_state::missing;
    std::string text;
};

struct game_check {
    item_status game, update, dlc, licences, firmware;
    std::string title_id;
    bool ready() const;
};

struct launch_options {
    std::filesystem::path game;
    std::filesystem::path update;
    std::vector<std::filesystem::path> dlcs;
    bool prepare = false;
};

struct game_add_result {
    std::filesystem::path pup;
    std::string dlc_notice;
    std::string rap_notice;
};

enum class game_add_mode { any, dlc, rap };

game_add_result add_game_items(const std::filesystem::path& exe, game_paths& paths, const std::vector<std::filesystem::path>& items,
                               game_add_mode mode = game_add_mode::any);
game_check check_game_paths(const std::filesystem::path& exe, const game_paths& paths);
std::filesystem::path firmware_dir(const std::filesystem::path& exe, const game_paths& paths);
std::filesystem::path default_update_pkg(const std::filesystem::path& exe, const std::string& title_id);
int prepare_game_files(const std::filesystem::path& exe, const launch_options& opts);
bool load_game_files(const std::filesystem::path& exe, const std::filesystem::path& icon, game_files& out, const launch_options& opts = {});
