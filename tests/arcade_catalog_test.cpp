#include "arcade_catalog.h"
#include "test_platform.h"
#include "namco/galaxian/galaxian_rom.h"
#include "capcom/gng/gng_rom.h"
#include "sega/model1/model1_rom.h"
#include "sega/model2/model2_rom.h"
#include "namco/namco_rom.h"
#include "sega/system16b/system16b_rom.h"
#include "namco/system22/system22_rom.h"
#include "system246_rom.h"
#include "xbox360_rom.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

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
    // Every board that emulates hardware ships with games. The plugin shelf
    // does not: the games that appear on it are discovered on disk at start-up,
    // so an empty built-in count there is the correct state, not a gap.
    for (std::size_t board = 0; board < arcade_board_count; ++board) {
        if (arcade_boards()[board].type == arcade_board_type::game_plugin)
            continue;
        assert(games_per_board[board] != 0);
    }

    // Native arcade System Link is deliberately opt-in. Only titles whose
    // cabinet communications path is currently enabled (Ridge Racer 2, Rave
    // Racer, Sega Rally and Daytona USA) advertise it; ordinary
    // alternating/simultaneous games use the separate network 2P path and
    // must never appear in the System Link list.
    std::size_t system_link_games = 0;
    for (const rom_set_manifest& game : supported_rom_sets()) {
        if (!supports_native_system_link(game)) continue;
        ++system_link_games;
        const std::string name(game.short_name);
        assert(name == "ridgera2" || name == "raverace" ||
               name == "srallyc" || name == "daytona" ||
               name == "manxttc" || name == "motoraid");
        assert(!supports_network_two_player(game));
    }
    assert(system_link_games == 6);
    assert(supports_network_two_player(
        *find_supported_rom_set("galaga")));
    assert(!supports_native_system_link(
        *find_supported_rom_set("galaga")));

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
             ridge_racer_rom_set::time_crisis,
             ridge_racer_rom_set::dirt_dash,
             ridge_racer_rom_set::aqua_jet})
        assert_registered(rom_loader::set_short_name(set),
                          arcade_board_type::system22);
    assert_registered(system246_rom_loader::set_short_name(
                          system246_rom_set::ridge_racer_v_arcade_battle),
                      arcade_board_type::system246);
    for (xbox360_rom_set set : {
             xbox360_rom_set::robotron_2084,
             // Both Geometry Wars titles are native game plugins now,
             // discovered on disk rather than carried in the catalogue as Xbox
             // 360 ROM sets. The loader below still recognises their packages -
             // that is what the import path reads - but nothing offers them as
             // a board game, so they are deliberately absent here.
             xbox360_rom_set::space_giraffe})
        assert_registered(xbox360_rom_loader::set_short_name(set),
                          arcade_board_type::xbox360);
    for (model1_rom_set set : {
             model1_rom_set::virtua_formula,
             model1_rom_set::virtua_fighter,
             model1_rom_set::star_wars_arcade,
             model1_rom_set::wing_war})
        assert_registered(model1_rom_loader::set_short_name(set),
                          arcade_board_type::model1);
    for (model2_rom_set set : {
             model2_rom_set::sega_rally_revision_c,
             model2_rom_set::virtua_cop_2,
             model2_rom_set::virtua_cop})
        assert_registered(model2_rom_loader::set_short_name(set),
                          arcade_board_type::model2);
    assert_registered(galaxian_rom_loader::set_short_name(
                          galaxian_rom_set::phoenix),
                      arcade_board_type::phoenix);
    assert_registered(galaxian_rom_loader::set_short_name(
                          galaxian_rom_set::galaxian),
                      arcade_board_type::galaxian);
    assert_registered(galaxian_rom_loader::set_short_name(
                          galaxian_rom_set::mooncrst),
                      arcade_board_type::galaxian);
    assert_registered(galaxian_rom_loader::set_short_name(
                          galaxian_rom_set::uniwars),
                      arcade_board_type::galaxian);
    assert_registered(system16b::system16b_rom_loader::set_short_name(
                          system16b::system16b_rom_set::shinobi_us),
                      arcade_board_type::system16b);
    assert_registered(system16b::system16b_rom_loader::set_short_name(
                          system16b::system16b_rom_set::bay_route),
                      arcade_board_type::system16b);
    assert_registered(gng::rom_loader::set_short_name(
                          gng::rom_set::world_set_1),
                      arcade_board_type::capcom_gng);
    assert_registered(namco::rom_loader::set_short_name(
                          namco::rom_set::galaga),
                      arcade_board_type::namco_galaga);
    assert_registered(namco::rom_loader::set_short_name(
                          namco::rom_set::pacmania),
                      arcade_board_type::namco_system1);

    // Exercise the central probe using minimal directory fixtures. This
    // checks routing only; individual loaders have separate full-ROM tests.
    const fs::path root = fs::temp_directory_path() /
        ("manx-catalog-test-" + std::to_string(test_process_id()));
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
    probe("dirtdash", {"dt2verc.rom1"}, arcade_board_type::system22,
          "dirtdash");
    probe("aquajet", {"aj2verb.1"}, arcade_board_type::system22,
          "aquajet");
    // Collections that keep the hyphenated dump name must probe identically.
    probe("aquajet-alias", {"aj2ver-b.1"}, arcade_board_type::system22,
          "aquajet");
    probe("model1", {"epr-15638.14", "epr-15639.15"},
          arcade_board_type::model1, "vformula");
    probe("model2", {"epr-17888c.12", "epr-17889c.13"},
          arcade_board_type::model2, "srallyc");
    probe("vcop2", {"epr-18524.12", "epr-18525.13"},
          arcade_board_type::model2, "vcop2");
    probe("vcop", {"epr-17166b.12", "epr-17167b.13"},
          arcade_board_type::model2, "vcop");
    probe("phoenix", {"ic45", "h5-ic49.5a"},
          arcade_board_type::phoenix, "phoenix");
    probe("galaxian", {"galmidw.u", "1h.bin"},
          arcade_board_type::galaxian, "galaxian");
    probe("mooncrst", {"mc1", "mcs_b"},
          arcade_board_type::galaxian, "mooncrst");
    probe("uniwars", {"f07_1a.bin", "egg10"},
          arcade_board_type::galaxian, "uniwars");
    probe("shinobi", {"epr-11360.a7"}, arcade_board_type::system16b,
          "shinobi4");
    // Bay Route (set 1, US, unprotected) is identified by its plain
    // 68000 pair; the encrypted parent set's epr-125xx chips must NOT
    // match (the loader cannot decrypt FD1094 code).
    probe("bayroute1", {"br.a1", "br.a4"},
          arcade_board_type::system16b, "bayroute1");
    // The encrypted parent set (FD1094 317-0116, epr-125xx chips) must
    // NOT identify as anything: the loader cannot decrypt its 68000 code,
    // and its chips share no name with the unprotected set 1.
    {
        const fs::path directory = root / "bayroute-parent-encrypted";
        fs::create_directories(directory);
        touch(directory / "epr-12517.a7");
        touch(directory / "epr-12516.a5");
        assert(!identify_arcade_game(directory.string()));
    }
    // E-SWAT is the pre-decrypted `eswatd` clone, identified by the two
    // bootleg_-prefixed program chips that are the only thing separating
    // it from its FD1094 parent.
    probe("eswatd", {"bootleg_epr-12658.a1", "bootleg_epr-12659.a2"},
          arcade_board_type::system16b, "eswatd");
    // Galaga '88 shares the Namco System 1 board with Pac-Mania. Its ROM
    // set is identified by CRC rather than by filename, so it cannot be
    // probed with empty stand-in files the way the Sega sets can; assert
    // the catalog registration instead and leave content checks to
    // namco_rom_test, which runs against a real archive.
    assert_registered("galaga88", arcade_board_type::namco_system1);
    assert_registered("pacmania", arcade_board_type::namco_system1);

    // Wonder Boy III is the pre-decrypted `wb33d`. Its near-namesake
    // `wb31d` is a System 16A board, and keying on this set's own chips
    // is what keeps the two apart.
    probe("wb33d", {"bootleg_epr-12136.a5", "bootleg_epr-12137.a7"},
          arcade_board_type::system16b, "wb33d");
    {
        const fs::path directory = root / "wb31d-system16a";
        fs::create_directories(directory);
        touch(directory / "bootleg_epr12082.bin");
        touch(directory / "bootleg_epr12084.bin");
        assert(!identify_arcade_game(directory.string()));
    }
    // The FD1094 317-0130 parent ships the same two sockets without the
    // prefix. There is no decryptor here, so it must identify as nothing
    // rather than being loaded and run as garbage.
    {
        const fs::path directory = root / "eswat-parent-encrypted";
        fs::create_directories(directory);
        touch(directory / "epr-12658.a1");
        touch(directory / "epr-12659.a2");
        touch(directory / "317-0130.key");
        assert(!identify_arcade_game(directory.string()));
    }

    // A missing path must never silently fall through to System 22. This is
    // the contract the application uses when deciding which session to build.
    assert(!identify_arcade_game("/definitely/missing/manx.zip"));
    fs::remove_all(root);
    return 0;
}
