#include "arcade_catalog.h"
#include "galaxian_rom.h"
#include "model1_rom.h"
#include "model2_rom.h"
#include "shinobi_rom.h"
#include "system22_rom.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

void touch(const fs::path& path) {
    std::ofstream(path, std::ios::binary).put('\0');
}

void assert_registered(const char* short_name, arcade_board_type board) {
    const rom_set_manifest* manifest = find_supported_rom_set(short_name);
    assert(manifest && manifest->board == board);
}

} // namespace

int main() {
    const arcade_board_list& boards = arcade_boards();
    assert(boards.size() == arcade_board_count);

    std::set<arcade_board_type> types;
    std::set<std::string> ids;
    for (std::size_t index = 0; index < boards.size(); ++index) {
        const arcade_board_descriptor& board = boards[index];
        assert(types.insert(board.type).second);
        assert(ids.insert(board.id).second);
        assert(board.display_name && std::strlen(board.display_name) != 0);
        assert(board.menu_name && std::strlen(board.menu_name) != 0);
        assert(board.rom_directory && std::strlen(board.rom_directory) != 0);
        assert(arcade_board_index(board.type) == index);
        assert(&arcade_board(board.type) == &board);
    }

    std::set<std::string> short_names;
    std::array<std::size_t, arcade_board_count> games_per_board{};
    for (const rom_set_manifest& game : supported_rom_sets()) {
        assert(game.short_name && std::strlen(game.short_name) != 0);
        assert(game.display_name && std::strlen(game.display_name) != 0);
        assert(short_names.insert(game.short_name).second);
        assert(find_supported_rom_set(game.short_name) == &game);
        const std::size_t board_index = arcade_board_index(game.board);
        assert(board_index < arcade_board_count);
        ++games_per_board[board_index];
    }
    for (std::size_t count : games_per_board) assert(count != 0);

    // Loader-to-catalog short-name contracts. A loader may change its
    // internal enum, but its durable MAME key must always resolve here.
    for (ridge_racer_rom_set set : {
             ridge_racer_rom_set::ridge_racer,
             ridge_racer_rom_set::ridge_racer_full_scale,
             ridge_racer_rom_set::ridge_racer_2,
             ridge_racer_rom_set::rave_racer,
             ridge_racer_rom_set::ace_driver,
             ridge_racer_rom_set::victory_lap,
             ridge_racer_rom_set::cyber_commando,
             ridge_racer_rom_set::time_crisis})
        assert_registered(rom_loader::set_short_name(set),
                          arcade_board_type::system22);
    for (model1_rom_set set : {
             model1_rom_set::virtua_formula,
             model1_rom_set::virtua_fighter,
             model1_rom_set::star_wars_arcade,
             model1_rom_set::wing_war})
        assert_registered(model1_rom_loader::set_short_name(set),
                          arcade_board_type::model1);
    assert_registered(model2_rom_loader::set_short_name(
                          model2_rom_set::sega_rally_revision_c),
                      arcade_board_type::model2);
    assert_registered(galaxian_rom_loader::set_short_name(
                          galaxian_rom_set::phoenix),
                      arcade_board_type::phoenix);
    assert_registered(galaxian_rom_loader::set_short_name(
                          galaxian_rom_set::mooncrst),
                      arcade_board_type::mooncrst);
    assert_registered(shinobi::shinobi_rom_loader::set_short_name(
                          shinobi::shinobi_rom_set::shinobi_us),
                      arcade_board_type::shinobi);

    // Exercise the central probe using minimal directory fixtures. This
    // checks routing only; individual loaders have separate full-ROM tests.
    const fs::path root = fs::temp_directory_path() /
        ("whittyarcade-catalog-test-" + std::to_string(getpid()));
    fs::create_directories(root);
    const auto probe = [&](const char* name,
                           std::initializer_list<const char*> entries,
                           arcade_board_type expected_board,
                           const char* expected_name) {
        const fs::path directory = root / name;
        fs::create_directories(directory);
        for (const char* entry : entries) touch(directory / entry);
        const auto identity = identify_arcade_game(directory.string());
        assert(identity && identity->board == expected_board);
        assert(std::string(identity->short_name) == expected_name);
    };
    probe("system22", {"rr2_prgllb.4d"}, arcade_board_type::system22,
          "ridgerac");
    probe("timecris", {"ts2verb.1"}, arcade_board_type::system22,
          "timecris");
    probe("model1", {"epr-15638.14", "epr-15639.15"},
          arcade_board_type::model1, "vformula");
    probe("model2", {"epr-17888c.12", "epr-17889c.13"},
          arcade_board_type::model2, "srallyc");
    probe("phoenix", {"ic45", "h5-ic49.5a"},
          arcade_board_type::phoenix, "phoenix");
    probe("mooncrst", {"mc1", "mcs_b"},
          arcade_board_type::mooncrst, "mooncrst");
    probe("shinobi", {"epr-11360.a7"}, arcade_board_type::shinobi,
          "shinobi4");

    // A missing path must never silently fall through to System 22. This is
    // the contract the application uses when deciding which session to build.
    assert(!identify_arcade_game("/definitely/missing/whittyarcade.zip"));
    fs::remove_all(root);
    return 0;
}
