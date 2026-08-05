// Canonical metadata and ROM-to-board routing for every supported platform.
#pragma once

#include "arcade_types.h"

#include <array>
#include <vector>
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

// High-level cabinet form, derived from multiplayer mode plus the board.
// Used by the MANX launcher to decide whether a game belongs in the
// Single-screen tab or the Multi-screen tab, and what cabinet icon to
// draw on its card.
//
//   single_cabinet_single_screen:  one screen, one cabinet. Most uprights.
//   single_cabinet_dual_seat:      one cabinet, two seats alternating. Galaxian.
//   single_cabinet_shared:         one cabinet, two seats simultaneously. Virtua Cop.
//   twin_cabinet:                  two cabinets, one player each, linked. Galaga twin.
//   linked_network:                cabinets talking over a wire. Ridge Racer 2.
//
// `twin_cabinet` and `linked_network` both qualify for the MANX Multi-screen
// tab; `single_cabinet_dual_seat` and `single_cabinet_shared` are still a
// single screen, but a multi-seat cabinet - they live in the Single-screen
// tab with a 2P badge.
enum class arcade_cabinet_form : uint8_t {
    single_cabinet_single_screen,
    single_cabinet_dual_seat,
    single_cabinet_shared,
    twin_cabinet,
    linked_network,
};

struct native_title;

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

// Classify a manifest. Boards with `multiplayer == none` are always
// single_cabinet_single_screen; boards with simultaneous multiplayer are
// single_cabinet_shared; boards with alternating multiplayer are
// single_cabinet_dual_seat; boards with native_link multiplayer are split
// into twin_cabinet (Model 2 twin, Galaxian twin) vs linked_network (System 22
// link) by inspecting the board.
arcade_cabinet_form classify_cabinet_form(const rom_set_manifest& manifest);

// Short, cabinet-form-aware label for the launcher's info chip.
// Returns something like "Single-screen upright", "Twin cockpit", or
// "Linked cabinet wall".
const char* cabinet_form_label(arcade_cabinet_form form);

constexpr std::size_t arcade_board_count = 15;
using arcade_board_list =
    std::array<arcade_board_descriptor, arcade_board_count>;
// The built-in games are a fixed table; the list as a whole is not, because
// games also arrive as plugins discovered on disk at start-up. A std::array
// sized by a constant would mean a game could only ever be added by rebuilding
// the arcade, which is exactly what the plugin path exists to avoid.
constexpr std::size_t arcade_builtin_game_count = 49;
using arcade_game_list = std::vector<rom_set_manifest>;

// This is the only board-name/order/directory table in the application.
const arcade_board_list& arcade_boards();
const arcade_board_descriptor& arcade_board(arcade_board_type type);
std::size_t arcade_board_index(arcade_board_type type);

// Canonical game metadata, shared by ROM import, menus and persistent-data
// tooling without loading or probing an archive.
const arcade_game_list& supported_rom_sets();

// Adds games found on disk to the catalogue. Called once, after discovery and
// before the launcher reads the list. The records are kept alive here because
// rom_set_manifest holds borrowed `const char*`, so whatever owns those strings
// has to outlive the catalogue - passing by value and storing is deliberate.
struct discovered_game;
void register_plugin_games(std::vector<discovered_game> games);

// Adds the converted Xbox 360 titles to the catalogue. Called with the plugin
// list already registered - both rebuild the same table, so the last caller
// must see everything.
void register_native_titles(std::vector<native_title> titles);

// The converted title a short name or binary path refers to, or null.
const native_title* find_native_title(std::string_view key);

// The converted title whose OWNED GAME is at this path, or null. The launcher
// hands a session the path it selected, which for a converted title is the
// package or XEX it runs against - not its short name.
const native_title* find_native_title_for_game(std::string_view game_path);

// The game a bundle path names, or null. The launcher hands a session the path
// it selected, which for a plugin is its folder - that is what identifies it.
const discovered_game* find_plugin_game(std::string_view bundle_path);
const rom_set_manifest* find_supported_rom_set(std::string_view short_name);
bool supports_network_two_player(const rom_set_manifest& manifest);
bool supports_native_system_link(const rom_set_manifest& manifest);

// Probe a ROM archive/directory through the loader owned by each board. The
// returned MAME short name is the stable key used by manifests and saved data.
std::optional<arcade_game_identity> identify_arcade_game(
    const std::string& path);
