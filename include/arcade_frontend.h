// arcade_frontend.h - shared ROM-board and cabinet configuration dialogs.
#pragma once

#include "system22_rom.h"
#include "arcade_types.h"

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
void show_dip_switches(uint16_t& switches, ridge_racer_rom_set set);
void show_shinobi_dip_switches(uint16_t& switches);
void show_model1_cabinet_settings(bool& attract_sound_enabled);
