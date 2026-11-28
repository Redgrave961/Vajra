#pragma once

#include "game_files.h"

bool run_setup(const std::filesystem::path& exe, const std::filesystem::path& icon, game_paths& paths, game_check check, std::vector<report_line> notes);
