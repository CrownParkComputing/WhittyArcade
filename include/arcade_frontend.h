// arcade_frontend.h - shared ROM-board and cabinet configuration dialogs.
#pragma once

#include "system22_rom.h"
#include "arcade_types.h"
#include "operator_menu.h"

#include <cstdint>
#include <string>
#include <vector>

enum class rom_selection_action : uint8_t {
    no_change,
    selected,
    exit_requested,
};

struct rom_selection_result {
    rom_selection_action action{rom_selection_action::no_change};
    std::string path;
};

rom_selection_result show_rom_selector(const std::string& current_path);
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
