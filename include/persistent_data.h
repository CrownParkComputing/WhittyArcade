// Persistent operator-data inventory and safe file-management helpers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct persistent_file_info {
    std::string key;
    std::string label;
    std::string path;
    std::size_t expected_size{};
    bool exists{};
    std::size_t actual_size{};
    std::string modified;
};

struct persistent_game_info {
    std::string short_name;
    std::string display_name;
    std::vector<persistent_file_info> files;
};

struct persistent_action_result {
    bool success{};
    std::string message;
};

std::string nvram_root_path();
std::vector<persistent_game_info> persistent_games();
const persistent_game_info* find_persistent_game(
    const std::vector<persistent_game_info>& games, const std::string& short_name);

// Read-only summary of verified fields plus the board-specific service-menu
// procedure. Unknown bytes are deliberately not presented as guessed options.
std::string persistent_settings_report(const std::string& short_name);

persistent_action_result backup_persistent_game(const std::string& short_name);
persistent_action_result reset_persistent_game(const std::string& short_name);
persistent_action_result export_persistent_game(const std::string& short_name,
                                                const std::string& directory);
persistent_action_result import_persistent_game(const std::string& short_name,
                                                const std::string& source);

// A hash of every persistent file a game owns, or 0 when it has none - the
// 2D boards keep nothing between sessions. Netplay compares this across the
// two machines before the first frame: identical ROMs but different operator
// data means the two boards start from different memory and diverge.
std::uint64_t persistent_state_hash(const std::string& short_name);

// Exact-sized System 22 EEPROM helpers shared by the emulator and manager.
bool load_system22_eeprom(const std::string& short_name,
                          void* destination, std::size_t size);
bool save_system22_eeprom(const std::string& short_name,
                          const void* source, std::size_t size);
