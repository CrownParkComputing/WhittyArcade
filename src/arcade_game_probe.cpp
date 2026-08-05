// Archive-content probing is kept separate from the static game catalog so
// menus and persistence tools do not depend on every board's ROM loader.

#include "arcade_catalog.h"
#include "game_plugin_host.h"

#include "namco/galaxian/galaxian_rom.h"
#include "capcom/gng/gng_rom.h"
#include "sega/model1/model1_rom.h"
#include "sega/model2/model2_rom.h"
#include "namco/namco_rom.h"
#include "sega/system16b/system16b_rom.h"
#include "taito/taitoz/taitoz_rom.h"
#include "namco/system22/system22_rom.h"
#include "system246_rom.h"
#include "midway/midway_rom.h"

#include <filesystem>
namespace fs = std::filesystem;

std::optional<arcade_game_identity> identify_arcade_game(
        const std::string& path) {
    const ridge_racer_rom_set system22 = rom_loader::identify_set(path);
    if (system22 != ridge_racer_rom_set::unknown)
        return arcade_game_identity{arcade_board_type::system22,
                                    rom_loader::set_short_name(system22)};

    const system246_rom_set system246 =
        system246_rom_loader::identify_set(path);
    if (system246 != system246_rom_set::unknown)
        return arcade_game_identity{
            arcade_board_type::system246,
            system246_rom_loader::set_short_name(system246)};

    // An installed game plugin. Checked before the ROM loaders because a
    // plugin's bundle is a plain directory that none of them would claim, and
    // because discovery has already decided which library owns that folder -
    // asking the loaders first would only waste the work.
    if (const discovered_game* plugin = find_plugin_game(path))
        return arcade_game_identity{arcade_board_type::game_plugin,
                                    plugin->short_name};

    // Any other ".acgame" manifest is still a System 246/256 game -- the board
    // boots any manifest directly, so a collection title with no built-in enum
    // entry (e.g. Tekken 5) must route here too, keyed by its basename. Without
    // this, identify_arcade_game() returns nullopt for such a game and the
    // launch aborts ("Unsupported or incomplete ROM archive") before PCSX2 ever
    // starts. Mirrors the rom_library discovery pass that surfaces these games.
    const std::string acgame_key =
        system246_rom_loader::acgame_short_name(path);
    if (!acgame_key.empty())
        return arcade_game_identity{arcade_board_type::system246, acgame_key};

    const model1_rom_set model1 = model1_rom_loader::identify_set(path);
    if (model1 != model1_rom_set::unknown)
        return arcade_game_identity{arcade_board_type::model1,
                                    model1_rom_loader::set_short_name(model1)};

    const model2_rom_set model2 = model2_rom_loader::identify_set(path);
    if (model2 != model2_rom_set::unknown)
        return arcade_game_identity{arcade_board_type::model2,
                                    model2_rom_loader::set_short_name(model2)};

    const galaxian_rom_set classic = galaxian_rom_loader::identify_set(path);
    if (classic != galaxian_rom_set::unknown) {
        const arcade_board_type board = classic == galaxian_rom_set::phoenix ?
            arcade_board_type::phoenix : arcade_board_type::galaxian;
        return arcade_game_identity{
            board, galaxian_rom_loader::set_short_name(classic)};
    }

    const system16b::system16b_rom_set shinobi_set =
        system16b::system16b_rom_loader::identify_set(path);
    if (shinobi_set != system16b::system16b_rom_set::unknown)
        return arcade_game_identity{
            arcade_board_type::system16b,
            system16b::system16b_rom_loader::set_short_name(shinobi_set)};

    const gng::rom_set gng_set = gng::rom_loader::identify_set(path);
    if (gng_set != gng::rom_set::unknown)
        return arcade_game_identity{
            arcade_board_type::capcom_gng,
            gng::rom_loader::set_short_name(gng_set)};

    const taitoz::taitoz_rom_set taitoz_set =
        taitoz::taitoz_rom_loader::identify_set(path);
    if (taitoz_set != taitoz::taitoz_rom_set::unknown)
        return arcade_game_identity{
            arcade_board_type::taito_z,
            taitoz::taitoz_rom_loader::set_short_name(taitoz_set)};

    const namco::rom_set namco_set = namco::rom_loader::identify_set(path);
    if (namco_set != namco::rom_set::unknown)
        return arcade_game_identity{
            namco_set == namco::rom_set::galaga
                ? arcade_board_type::namco_galaga
                : arcade_board_type::namco_system1,
            namco::rom_loader::set_short_name(namco_set)};

    const midway_rom_set midway_set = midway_rom_loader::identify_set(path);
    if (midway_set != midway_rom_set::unknown)
        return arcade_game_identity{
            arcade_board_type::midway,
            midway_rom_loader::set_short_name(midway_set)};

    return std::nullopt;
}
