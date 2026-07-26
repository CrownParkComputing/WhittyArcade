#include "arcade_catalog.h"

#include <algorithm>
#include <stdexcept>

namespace {

constexpr arcade_board_list boards{{
    {arcade_board_type::system22, "system22", "Namco System 22",
     "NAMCO SYSTEM 22", "system22"},
    {arcade_board_type::system246, "system246", "Namco System 246/256",
     "NAMCO SYSTEM 246/256", "system246"},
    {arcade_board_type::xbox360, "xbox360", "Microsoft Xbox 360",
     "XBOX 360", "xbox360"},
    {arcade_board_type::model1, "model1", "Sega Model 1",
     "SEGA MODEL 1", "model1"},
    {arcade_board_type::model2, "model2", "Sega Model 2",
     "SEGA MODEL 2", "model2"},
    {arcade_board_type::phoenix, "phoenix", "Phoenix hardware",
     "PHOENIX HARDWARE", "phoenix"},
    {arcade_board_type::galaxian, "galaxian", "Galaxian hardware",
     "GALAXIAN HARDWARE", "galaxian"},
    {arcade_board_type::system16b, "system16b", "Sega System 16B",
     "SEGA SYSTEM 16B", "system16b"},
    {arcade_board_type::capcom_gng, "capcom_gng",
     "Capcom Ghosts'n Goblins hardware",
     "CAPCOM GHOSTS'N GOBLINS", "capcom_gng"},
    {arcade_board_type::namco_galaga, "namco_galaga",
     "Namco Galaga hardware", "NAMCO GALAGA", "namco_galaga"},
    {arcade_board_type::namco_system1, "namco_system1",
     "Namco System 1", "NAMCO SYSTEM 1", "namco_system1"},
}};

constexpr arcade_game_list manifests{{
    {"ridgerac", "Ridge Racer (World, RR2 Ver.B)",
     arcade_board_type::system22, "", "namcoc71.zip + namcoc74.zip", true,
     arcade_multiplayer_mode::none, "Namco"},
    {"ridgera2", "Ridge Racer 2 (World, RRS2)",
     arcade_board_type::system22, "", "namcoc71.zip + namcoc74.zip", true,
     arcade_multiplayer_mode::native_link,
     "Namco"},
    {"raverace", "Rave Racer (World, RV2 Ver.B)",
     arcade_board_type::system22, "", "namcoc71.zip + namcoc74.zip", true,
     arcade_multiplayer_mode::native_link,
     "Namco"},
    {"acedrive", "Ace Driver: Racing Evolution (World, AD2)",
     arcade_board_type::system22, "", "namcoc71.zip + namcoc74.zip", true,
     arcade_multiplayer_mode::none, "Namco"},
    {"victlap", "Ace Driver: Victory Lap (World, ADV2 Ver.B)",
     arcade_board_type::system22, "", "namcoc71.zip + namcoc74.zip", true,
     arcade_multiplayer_mode::none, "Namco"},
    {"cybrcomm", "Cyber Commando (Japan, CY1)",
     arcade_board_type::system22, "", "namcoc71.zip + namcoc74.zip", true,
     arcade_multiplayer_mode::none, "Namco"},
    {"timecris", "Time Crisis (World, TS2 Ver.B)",
     arcade_board_type::system22, "", "namcoc71.zip", true,
     arcade_multiplayer_mode::none, "Namco"},
    {"dirtdash", "Dirt Dash (World, DT2 Ver.C)",
     arcade_board_type::system22, "", "namcoc71.zip", true,
     arcade_multiplayer_mode::none, "Namco"},
    {"aquajet", "Aqua Jet (World, AJ2 Ver.B)",
     arcade_board_type::system22, "", "namcoc71.zip", true,
     arcade_multiplayer_mode::none, "Namco"},
    {"ridgeracf", "Ridge Racer Full Scale (World, RRF2)",
     arcade_board_type::system22, "", "namcoc71.zip + namcoc74.zip", false,
     arcade_multiplayer_mode::none, "Namco"},
    {"rrvac", "Ridge Racer V: Arcade Battle (RRV3 Ver.A)",
     arcade_board_type::system246, "", "rrv1-a.chd", true,
     arcade_multiplayer_mode::none, "Namco"},
    {"motogp", "MotoGP", arcade_board_type::system246, "",
     "motogp_hdd.chd", true,
     arcade_multiplayer_mode::none, "Namco"},
    {"robotron", "Robotron: 2084 (offline Xbox 360)",
     arcade_board_type::xbox360, "",
     "Extracted default.xex with classic/ and media/ data", true,
     arcade_multiplayer_mode::none, "Williams"},
    {"geometrywars", "Geometry Wars: Retro Evolved (Xbox 360)",
     arcade_board_type::xbox360, "",
     "Extracted default.xex with GeometryWars1.dat and GW1.xwb", true,
     arcade_multiplayer_mode::none, "Microsoft"},
    // Played straight out of the package it was downloaded as: everything the
    // title reads is sealed inside that one file, so there is nothing to
    // extract and nothing to put beside it.
    {"geometrywars2", "Geometry Wars: Retro Evolved 2 (Xbox 360)",
     arcade_board_type::xbox360, "",
     "Signed STFS package (LIVE, CON or PIRS) holding the whole title", true,
     arcade_multiplayer_mode::none, "Microsoft"},
    // Dumped both ways, and either plays. The package is preferred, so that is
    // what the requirement names first.
    {"spacegiraffe", "Space Giraffe (Xbox 360)",
     arcade_board_type::xbox360, "",
     "Signed STFS package, or an extracted default.xex with its Media/ data",
     true, arcade_multiplayer_mode::none, "Llamasoft"},
    {"vformula", "Virtua Formula", arcade_board_type::model1,
     "vr.zip", "", true,
     arcade_multiplayer_mode::none, "Sega"},
    {"vf", "Virtua Fighter", arcade_board_type::model1, "", "", true,
     arcade_multiplayer_mode::none, "Sega"},
    {"swa", "Star Wars Arcade (US)", arcade_board_type::model1,
     "", "", true,
     arcade_multiplayer_mode::none, "Sega"},
    {"wingwar", "Wing War (World)", arcade_board_type::model1,
     "", "", true,
     arcade_multiplayer_mode::none, "Sega"},
    {"srallyc", "Sega Rally Championship (Revision C)",
     arcade_board_type::model2, "", "", true,
     arcade_multiplayer_mode::native_link,
     "Sega"},
    {"vcop2", "Virtua Cop 2 (Model 2A)",
     arcade_board_type::model2, "", "", true,
     arcade_multiplayer_mode::simultaneous,
     "Sega"},
    {"vcop", "Virtua Cop (Model 2)",
     arcade_board_type::model2, "", "", true,
     arcade_multiplayer_mode::simultaneous,
     "Sega"},
    {"daytona", "Daytona USA (Revision A)",
     arcade_board_type::model2, "", "model1io.zip", true,
     arcade_multiplayer_mode::native_link,
     "Sega"},
    {"vf2", "Virtua Fighter 2 (Version 2.1)",
     arcade_board_type::model2, "", "", true,
     arcade_multiplayer_mode::simultaneous,
     "Sega"},
    {"manxttc", "Manx TT Superbike - Twin (Revision C)",
     arcade_board_type::model2, "", "", true,
     arcade_multiplayer_mode::native_link,
     "Sega"},
    {"motoraid", "Motor Raid - Twin",
     arcade_board_type::model2, "", "", true,
     arcade_multiplayer_mode::native_link,
     "Sega"},
    {"phoenix", "Phoenix (Amstar, set 1)", arcade_board_type::phoenix,
     "", "", true, arcade_multiplayer_mode::alternating,
     "Amstar"},
    {"galaxian", "Galaxian (Namco set 1)", arcade_board_type::galaxian,
     "", "", true, arcade_multiplayer_mode::alternating,
     "Namco"},
    {"mooncrst", "Moon Cresta (Nichibutsu)", arcade_board_type::galaxian,
     "", "", true, arcade_multiplayer_mode::alternating,
     "Nichibutsu"},
    {"uniwars", "UniWar S (Irem)", arcade_board_type::galaxian,
     "", "", true, arcade_multiplayer_mode::alternating,
     "Irem"},
    {"shinobi4", "Shinobi (System 16B, set 4)",
     arcade_board_type::system16b, "shinobi.zip",
     "shinobi6.zip for unencrypted sound ROM in split collections", true,
     arcade_multiplayer_mode::alternating,
     "Sega"},
    {"gng", "Ghosts'n Goblins (World? set 1)",
     arcade_board_type::capcom_gng, "", "", true,
     arcade_multiplayer_mode::alternating,
     "Capcom"},
    {"galaga", "Galaga (Namco rev. B)",
     arcade_board_type::namco_galaga, "", "", true,
     arcade_multiplayer_mode::alternating,
     "Namco"},
    {"pacmania", "Pac-Mania (World)",
     arcade_board_type::namco_system1, "", "", true,
     arcade_multiplayer_mode::alternating,
     "Namco"},
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

bool supports_network_two_player(const rom_set_manifest& manifest) {
    return manifest.working &&
           (manifest.multiplayer == arcade_multiplayer_mode::alternating ||
            manifest.multiplayer == arcade_multiplayer_mode::simultaneous);
}

bool supports_native_system_link(const rom_set_manifest& manifest) {
    // System Link means the original cabinet communication hardware, rather
    // than WhittyArcade's shared two-player input/video transport.
    return manifest.working &&
           manifest.multiplayer == arcade_multiplayer_mode::native_link;
}
