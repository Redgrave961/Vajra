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
    bool ready() const;
};

std::filesystem::path add_game_items(const std::filesystem::path& exe, game_paths& paths, const std::vector<std::filesystem::path>& items);
game_check check_game_paths(const std::filesystem::path& exe, const game_paths& paths);
std::filesystem::path firmware_dir(const std::filesystem::path& exe, const game_paths& paths);
bool load_game_files(const std::filesystem::path& exe, game_files& out);
