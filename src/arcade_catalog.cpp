#include "arcade_catalog.h"

#include <algorithm>
#include <stdexcept>

namespace {

constexpr arcade_board_list boards{{
    {arcade_board_type::system22, "system22", "Namco System 22",
     "NAMCO SYSTEM 22", "system22"},
    {arcade_board_type::model1, "model1", "Sega Model 1",
     "SEGA MODEL 1", "model1"},
    {arcade_board_type::model2, "model2", "Sega Model 2",
     "SEGA MODEL 2", "model2"},
    {arcade_board_type::phoenix, "phoenix", "Phoenix hardware",
     "PHOENIX HARDWARE", "phoenix"},
    {arcade_board_type::mooncrst, "galaxian", "Galaxian hardware",
     "GALAXIAN HARDWARE", "galaxian"},
    {arcade_board_type::shinobi, "system16b", "Sega System 16B",
     "SEGA SYSTEM 16B", "system16b"},
}};

constexpr arcade_game_list manifests{{
    {"ridgerac", "Ridge Racer (World, RR2 Ver.B)",
     arcade_board_type::system22, "", "namcoc71.zip + namcoc74.zip", true},
    {"ridgera2", "Ridge Racer 2 (World, RRS2)",
     arcade_board_type::system22, "", "namcoc71.zip + namcoc74.zip", true},
    {"raverace", "Rave Racer (World, RV2 Ver.B)",
     arcade_board_type::system22, "", "namcoc71.zip + namcoc74.zip", true},
    {"acedrive", "Ace Driver: Racing Evolution (World, AD2)",
     arcade_board_type::system22, "", "namcoc71.zip + namcoc74.zip", true},
    {"victlap", "Ace Driver: Victory Lap (World, ADV2 Ver.B)",
     arcade_board_type::system22, "", "namcoc71.zip + namcoc74.zip", true},
    {"cybrcomm", "Cyber Commando (Japan, CY1)",
     arcade_board_type::system22, "", "namcoc71.zip + namcoc74.zip", true},
    {"timecris", "Time Crisis (World, TS2 Ver.B)",
     arcade_board_type::system22, "", "namcoc71.zip", true},
    {"dirtdash", "Dirt Dash (World, DT2 Ver.C)",
     arcade_board_type::system22, "", "namcoc71.zip", true},
    {"ridgeracf", "Ridge Racer Full Scale (World, RRF2)",
     arcade_board_type::system22, "", "namcoc71.zip + namcoc74.zip", false},
    {"vformula", "Virtua Formula", arcade_board_type::model1,
     "vr.zip", "", true},
    {"vf", "Virtua Fighter", arcade_board_type::model1, "", "", true},
    {"swa", "Star Wars Arcade (US)", arcade_board_type::model1,
     "", "", true},
    {"wingwar", "Wing War (World)", arcade_board_type::model1,
     "", "", true},
    {"srallyc", "Sega Rally Championship (Revision C)",
     arcade_board_type::model2, "", "", true},
    {"phoenix", "Phoenix (Amstar, set 1)", arcade_board_type::phoenix,
     "", "", true},
    {"galaxian", "Galaxian (Namco set 1)", arcade_board_type::mooncrst,
     "", "", true},
    {"mooncrst", "Moon Cresta (Nichibutsu)", arcade_board_type::mooncrst,
     "", "", true},
    {"uniwars", "UniWar S (Irem)", arcade_board_type::mooncrst,
     "", "", true},
    {"shinobi4", "Shinobi (System 16B, set 4)",
     arcade_board_type::shinobi, "shinobi.zip",
     "shinobi6.zip for unencrypted sound ROM in split collections", true},
}};

} // namespace

const arcade_board_list& arcade_boards() {
    return boards;
}

std::size_t arcade_board_index(arcade_board_type type) {
    for (std::size_t index = 0; index < boards.size(); ++index)
        if (boards[index].type == type) return index;
    return boards.size();
}

const arcade_board_descriptor& arcade_board(arcade_board_type type) {
    const std::size_t index = arcade_board_index(type);
    if (index == boards.size())
        throw std::invalid_argument("Unknown arcade board type");
    return boards[index];
}

const arcade_game_list& supported_rom_sets() {
    return manifests;
}

const rom_set_manifest* find_supported_rom_set(std::string_view short_name) {
    const auto found = std::find_if(
        manifests.begin(), manifests.end(),
        [short_name](const rom_set_manifest& item) {
            return short_name == item.short_name;
        });
    return found == manifests.end() ? nullptr : &*found;
}
