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
    // std::string (not const char*) so boards whose short name is derived at
    // runtime -- e.g. a System 246/256 game keyed by its ".acgame" basename --
    // can carry a dynamic name rather than only a static catalog literal.
    std::string short_name;
};

enum class arcade_multiplayer_mode : uint8_t {
    none,
    alternating,
    simultaneous,
    native_link,
};

struct rom_set_manifest {
    const char* short_name;
    const char* display_name;
    arcade_board_type board;
    const char* split_parent;
    const char* extra_archives;
    bool working;
    // Advertise multiplayer launch modes only when the current emulator
    // actually wires the board's P2 inputs, not merely because the original
    // title was marketed for multiple players.
    arcade_multiplayer_mode multiplayer{arcade_multiplayer_mode::none};
    // Who put the machine in the arcade, which is not always the board's
    // maker: Moon Cresta and UniWar S both run on Galaxian hardware but came
    // from Nichibutsu and Irem. Browsing by publisher is only honest if this
    // is per game rather than derived from the board.
    const char* publisher{""};
};

constexpr std::size_t arcade_board_count = 12;
using arcade_board_list =
    std::array<arcade_board_descriptor, arcade_board_count>;
constexpr std::size_t arcade_game_count = 43;
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
bool supports_network_two_player(const rom_set_manifest& manifest);
bool supports_native_system_link(const rom_set_manifest& manifest);

// Probe a ROM archive/directory through the loader owned by each board. The
// returned MAME short name is the stable key used by manifests and saved data.
std::optional<arcade_game_identity> identify_arcade_game(
    const std::string& path);
