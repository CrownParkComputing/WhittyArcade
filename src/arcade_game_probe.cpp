// Archive-content probing is kept separate from the static game catalog so
// menus and persistence tools do not depend on every board's ROM loader.

#include "arcade_catalog.h"

#include "galaxian_rom.h"
#include "model1_rom.h"
#include "model2_rom.h"
#include "shinobi_rom.h"
#include "system22_rom.h"

std::optional<arcade_game_identity> identify_arcade_game(
        const std::string& path) {
    const ridge_racer_rom_set system22 = rom_loader::identify_set(path);
    if (system22 != ridge_racer_rom_set::unknown)
        return arcade_game_identity{arcade_board_type::system22,
                                    rom_loader::set_short_name(system22)};

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
            arcade_board_type::phoenix : arcade_board_type::mooncrst;
        return arcade_game_identity{
            board, galaxian_rom_loader::set_short_name(classic)};
    }

    const shinobi::shinobi_rom_set shinobi_set =
        shinobi::shinobi_rom_loader::identify_set(path);
    if (shinobi_set != shinobi::shinobi_rom_set::unknown)
        return arcade_game_identity{
            arcade_board_type::shinobi,
            shinobi::shinobi_rom_loader::set_short_name(shinobi_set)};

    return std::nullopt;
}
