// Canonical metadata and ROM-to-board routing for every supported platform.
#pragma once

#include "arcade_types.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

struct arcade_board_descriptor {
    arcade_board_type type;
    const char* id;
    const char* display_name;
    const char* menu_name;
    const char* rom_directory;
};

struct arcade_game_identity {
    arcade_board_type board;
    const char* short_name;
};

struct rom_set_manifest {
    const char* short_name;
    const char* display_name;
    arcade_board_type board;
    const char* split_parent;
    const char* extra_archives;
    bool working;
};

constexpr std::size_t arcade_board_count = 6;
using arcade_board_list =
    std::array<arcade_board_descriptor, arcade_board_count>;
constexpr std::size_t arcade_game_count = 16;
using arcade_game_list =
    std::array<rom_set_manifest, arcade_game_count>;

// This is the only board-name/order/directory table in the application.
const arcade_board_list& arcade_boards();
const arcade_board_descriptor& arcade_board(arcade_board_type type);
std::size_t arcade_board_index(arcade_board_type type);

// Canonical game metadata, shared by ROM import, menus and persistent-data
// tooling without loading or probing an archive.
const arcade_game_list& supported_rom_sets();
const rom_set_manifest* find_supported_rom_set(std::string_view short_name);

// Probe a ROM archive/directory through the loader owned by each board. The
// returned MAME short name is the stable key used by manifests and saved data.
std::optional<arcade_game_identity> identify_arcade_game(
    const std::string& path);
