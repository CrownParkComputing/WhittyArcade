#include "arcade_catalog.h"

#include "game_plugin_host.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace {

constexpr arcade_board_list boards{{
    // Not a board at all: the shelf that games shipping as plugins appear on.
    // Its "rom directory" is where installed games live, which is the folder
    // discovery scans.
    {arcade_board_type::game_plugin, "games", "Native Games",
     "NATIVE GAMES", "games"},
    {arcade_board_type::system22, "system22", "Namco System 22",
     "NAMCO SYSTEM 22", "system22"},
    {arcade_board_type::system246, "system246", "Namco System 246/256",
     "NAMCO SYSTEM 246/256", "system246"},
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
    {arcade_board_type::taito_z, "taito_z",
     "Taito Z System", "TAITO Z SYSTEM", "taito_z"},
    {arcade_board_type::midway, "midway",
     "Midway Wolf Unit", "MIDWAY WOLF UNIT", "midway"},
}};

constexpr std::array<rom_set_manifest, arcade_builtin_game_count> builtin_manifests{{
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
    {"warofbug", "War of the Bugs", arcade_board_type::galaxian,
     "", "", true, arcade_multiplayer_mode::alternating,
     "Armenia / Food and Fun"},
    {"aliensyn", "Alien Syndrome (System 16B)",
     arcade_board_type::system16b, "", "", true,
     arcade_multiplayer_mode::simultaneous,
     "Sega"},
    {"aurail", "Aurail (System 16B)",
     arcade_board_type::system16b, "", "", true,
     arcade_multiplayer_mode::alternating,
     "Sega / Westone"},
    {"riotcity", "Riot City (System 16B)",
     arcade_board_type::system16b, "", "", true,
     arcade_multiplayer_mode::simultaneous,
     "Sega / Westone"},
    {"goldnaxe2", "Golden Axe (System 16B)",
     arcade_board_type::system16b, "goldnaxe.zip", "", true,
     arcade_multiplayer_mode::simultaneous,
     "Sega"},
    {"altbeast", "Altered Beast (System 16B)",
     arcade_board_type::system16b, "", "", true,
     arcade_multiplayer_mode::simultaneous,
     "Sega"},
    {"ddux1", "Dynamite Dux (System 16B)",
     arcade_board_type::system16b, "ddux.zip", "", true,
     arcade_multiplayer_mode::simultaneous,
     "Sega"},
    {"tturfu", "Tough Turf (System 16B)",
     arcade_board_type::system16b, "tturf.zip", "", true,
     arcade_multiplayer_mode::simultaneous,
     "Sega / Sunsoft"},
    {"bayroute1", "Bay Route (System 16B, set 1 US)",
     arcade_board_type::system16b, "bayroute.zip",
     "bayroute1.zip for the unprotected set-1 chips", true,
     arcade_multiplayer_mode::simultaneous,
     "Sunsoft / Sega"},
    {"eswatd", "Cyber Police E-SWAT (System 16B)",
     arcade_board_type::system16b, "eswat.zip", "", true,
     arcade_multiplayer_mode::alternating,
     "Sega"},
    {"wb33d", "Wonder Boy III: Monster Lair (System 16B)",
     arcade_board_type::system16b, "wb3.zip", "", true,
     arcade_multiplayer_mode::simultaneous,
     "Sega / Westone"},
    {"shinobi4", "Shinobi (System 16B, set 4)",
     arcade_board_type::system16b, "shinobi.zip",
     "shinobi6.zip for unencrypted sound ROM in split collections", true,
     arcade_multiplayer_mode::alternating,
     "Sega"},
    {"contcirc", "Continental Circus (Taito Z System)",
     arcade_board_type::taito_z, "", "", true,
     arcade_multiplayer_mode::none,
     "Taito"},
    {"gng", "Ghosts'n Goblins (World? set 1)",
     arcade_board_type::capcom_gng, "", "", true,
     arcade_multiplayer_mode::alternating,
     "Capcom"},
    {"galaga", "Galaga (Namco rev. B)",
     arcade_board_type::namco_galaga, "", "", true,
     arcade_multiplayer_mode::alternating,
     "Namco"},
    {"galaga88", "Galaga '88 (World)",
     arcade_board_type::namco_system1, "", "", true,
     arcade_multiplayer_mode::alternating,
     "Namco"},
    {"pacmania", "Pac-Mania (World)",
     arcade_board_type::namco_system1, "", "", true,
     arcade_multiplayer_mode::alternating,
     "Namco"},
    {"kinst", "Killer Instinct (v1.5d)",
     arcade_board_type::midway, "", "kinst.chd", true,
     arcade_multiplayer_mode::simultaneous,
     "Midway / Rare"},
    {"kinst2", "Killer Instinct 2 (v1.4)",
     arcade_board_type::midway, "", "kinst2.chd", true,
     arcade_multiplayer_mode::simultaneous,
     "Midway / Rare"},
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

// Built-ins plus whatever discovery added, rebuilt whenever plugins are
// registered. The plugin records are kept alongside because each manifest row
// borrows their strings.
std::vector<discovered_game>& plugin_records() {
    static std::vector<discovered_game> records;
    return records;
}

arcade_game_list& catalogue() {
    static arcade_game_list all(builtin_manifests.begin(),
                                builtin_manifests.end());
    return all;
}

const arcade_game_list& supported_rom_sets() {
    return catalogue();
}

// Single-cabinet twin is the Model 2 Galaga twin / Shinobi twin /
// Manx TT Twin / Motor Raid Twin lineage: two cabinets, one ROM, each
// is half of a single game. They share a board type (model2), and the
// distinguishing signal is the short name. Twin Galaxy/Sega
// nomenclature for this is "TWIN".
bool short_name_is_model2_twin(const char* short_name) {
    if (!short_name) return false;
    static const char* const twins[] = {
        "galagat",   // Galaga twin
        "shinobia",  // Shinobi twin
        "manxttc",   // Manx TT Twin
        "motoraid",  // Motor Raid Twin
    };
    for (const char* t : twins) {
        if (std::strcmp(short_name, t) == 0) return true;
    }
    return false;
}

arcade_cabinet_form classify_cabinet_form(const rom_set_manifest& manifest) {
    switch (manifest.multiplayer) {
    case arcade_multiplayer_mode::none:
        return arcade_cabinet_form::single_cabinet_single_screen;
    case arcade_multiplayer_mode::alternating:
        // Galaxian-class uprights take turns. One screen, two seats.
        return arcade_cabinet_form::single_cabinet_dual_seat;
    case arcade_multiplayer_mode::simultaneous:
        // Virtua Cop / VF2 — two seats sharing one screen.
        return arcade_cabinet_form::single_cabinet_shared;
    case arcade_multiplayer_mode::native_link:
        // System 22 (C139 link), Model 2 "TWIN" sets, and System 246 walls
        // are all genuinely multi-screen / multi-cabinet. The Model 2 TWIN
        // line uses a single ROM set served to two cabinets; System 22 / 246
        // networks one cabinet per game slot.
        if (manifest.board == arcade_board_type::model2 &&
            short_name_is_model2_twin(manifest.short_name)) {
            return arcade_cabinet_form::twin_cabinet;
        }
        return arcade_cabinet_form::linked_network;
    }
    return arcade_cabinet_form::single_cabinet_single_screen;
}

const char* cabinet_form_label(arcade_cabinet_form form) {
    switch (form) {
    case arcade_cabinet_form::single_cabinet_single_screen:
        return "Single-screen cabinet";
    case arcade_cabinet_form::single_cabinet_dual_seat:
        return "Single-cabinet, two seats";
    case arcade_cabinet_form::single_cabinet_shared:
        return "Single-cabinet, shared screen";
    case arcade_cabinet_form::twin_cabinet:
        return "Twin cabinet";
    case arcade_cabinet_form::linked_network:
        return "Linked cabinet network";
    }
    return "Single-screen cabinet";
}

// One rebuild for both kinds of discovered game. They share the catalogue, so
// registering either from its own function would drop whatever the other had
// added - and the symptom is a game that vanishes depending on start-up order.
void rebuild_catalogue() {
    arcade_game_list& all = catalogue();
    all.assign(builtin_manifests.begin(), builtin_manifests.end());
    for (const discovered_game& game : plugin_records()) {
        rom_set_manifest row{};
        row.short_name = game.short_name.c_str();
        row.display_name = game.display_name.c_str();
        row.board = arcade_board_type::game_plugin;
        row.split_parent = "";
        // What the launcher tells the player they need. A plugin brings its own
        // data in its folder, so there is nothing for them to supply.
        row.extra_archives = "Installed game plugin; no ROM set required";
        row.working = true;
        row.multiplayer = game.max_players > 1
                              ? arcade_multiplayer_mode::simultaneous
                              : arcade_multiplayer_mode::none;
        row.publisher = game.publisher.c_str();
        all.push_back(row);
    }
}

void register_plugin_games(std::vector<discovered_game> games) {
    plugin_records() = std::move(games);
    rebuild_catalogue();
}

const discovered_game* find_plugin_game(std::string_view bundle_path) {
    for (const discovered_game& game : plugin_records())
        if (game.bundle_path == bundle_path || game.short_name == bundle_path)
            return &game;
    return nullptr;
}

const rom_set_manifest* find_supported_rom_set(std::string_view short_name) {
    const arcade_game_list& manifests = catalogue();
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
    // than MANX's shared two-player input/video transport.
    return manifest.working &&
           manifest.multiplayer == arcade_multiplayer_mode::native_link;
}
