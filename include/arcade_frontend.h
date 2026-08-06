// arcade_frontend.h - shared ROM-board and cabinet configuration dialogs.
#pragma once

#include "namco/system22/system22_rom.h"
#include "arcade_types.h"
#include "operator_menu.h"

#include <cstdint>
#include <string>
#include <vector>

class multiplayer_lobby;
class online_link;

enum class rom_selection_action : uint8_t {
    no_change,
    selected,
    exit_requested,
};

// A cabinet is either on its own or in a network session. Local pairs of
// processes linked over loopback are gone: every multiplayer route is the
// lobby now.
enum class cabinet_launch_mode : uint8_t {
    single,
    linked_network,
};

struct rom_selection_result {
    rom_selection_action action{rom_selection_action::no_change};
    std::string path;
    cabinet_launch_mode launch_mode{cabinet_launch_mode::single};
    int cabinet_node{};
    // Fullscreen follows from launch_mode - see the display policy in
    // main.cpp - so the selector only reports the twin placement choice.
    bool twin_separate_monitors{false};
    // Linked pair sharing one display, half the desktop per cabinet.
    bool twin_one_screen{false};
    // A session started as two-player. The board's second-player start is
    // then what the start button presses, so choosing a two-player game
    // begins a two-player game rather than a solo one on a shared screen.
    bool two_player{false};
    // Arcade wall: one ROM per column, all running at once across the
    // primary display. Empty unless the wall was chosen; entry 0 is this
    // process's own game.
    std::vector<std::string> wall_games;
    // Linked cabinets in the ring, chosen at launch. 2 for a classic twin;
    // games whose link supports more (Daytona's hardware takes up to 8)
    // offer the larger counts once verified.
    int cabinet_count{2};
};

// How many cabinets a game's link supports in this build. The hardware
// ceilings are higher (Daytona 8, Sega Rally 4); a game is raised here only
// once its ring actually races at that size.
int linked_cabinet_maximum(const std::string& short_name);

// The library browser as a modal over a paused, running cabinet: same grid,
// views and covers as the launcher. Returns the chosen ROM path, or empty
// when the player backs out.
std::string show_in_game_game_browser(const std::vector<rom_choice>& choices);

rom_selection_result show_rom_selector(const std::string& current_path,
                                       multiplayer_lobby* lobby = nullptr,
                                       online_link* online = nullptr);
std::vector<rom_choice> discover_rom_choices(const std::string& current_path);
operator_menu_definition make_system22_operator_menu(
    uint16_t switches, ridge_racer_rom_set set);
void apply_system22_operator_action(uint16_t& switches,
                                    const operator_menu_action& action);
operator_menu_definition make_system16b_operator_menu(uint16_t switches);
void apply_system16b_operator_action(uint16_t& switches,
                                   const operator_menu_action& action);
operator_menu_definition make_gng_operator_menu(uint16_t switches);
void apply_gng_operator_action(uint16_t& switches,
                               const operator_menu_action& action);
operator_menu_definition make_model1_operator_menu(bool attract_sound_enabled);
operator_menu_definition make_service_operator_menu(
    const std::string& title, const std::string& description,
    const std::string& action_label = "Open original service menu");
operator_menu_definition make_unavailable_operator_menu();
