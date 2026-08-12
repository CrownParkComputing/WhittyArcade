#pragma once

#include "manx_game_achievements.h"

#include <filesystem>
#include <string>
#include <vector>

// Per-OS-user persistence for native game-plugin achievements. The host owns
// the path and writes atomically; plugins only see typed state through the ABI.
std::filesystem::path plugin_achievement_path(std::string_view short_name);

bool load_plugin_achievements(
    std::string_view short_name,
    std::vector<manx_game_achievement_state>& states,
    std::string& error);

bool save_plugin_achievements(
    std::string_view short_name,
    const std::vector<manx_game_achievement_state>& states,
    std::string& error);
