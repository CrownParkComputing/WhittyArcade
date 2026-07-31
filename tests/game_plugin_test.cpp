// Discovery and the plugin ABI contract.
//
// A game plugin is loaded from disk and called through a C function table, so
// the failures worth testing are the ones that would otherwise be silent: a
// library that is not a game, one built against a different ABI, one with a
// hole in its table, two claiming the same name, and a discovery order that
// depends on how the file system felt that day. Each of those either produces a
// game that quietly never appears, or a crash a long way from its cause.
//
// The plugins here are built by the test itself so it needs nothing installed.

#include "arcade_catalog.h"
#include "game_plugin_host.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Compiles a one-file plugin into <root>/<name>/<name>.so. Returns false when
// no compiler is available, so the suite degrades to skipping rather than
// failing on a machine that cannot build one.
bool build_plugin(const fs::path& root, const std::string& name,
                  const std::string& source) {
    const fs::path dir = root / name;
    fs::create_directories(dir);
    const fs::path cpp = dir / (name + ".cpp");
    {
        std::ofstream out(cpp);
        out << source;
    }
    const std::string include = INCLUDE_DIR;
    const std::string command = "c++ -std=c++20 -fPIC -shared -I" + include +
                                " " + cpp.string() + " -o " +
                                (dir / (name + ".so")).string() + " 2>/dev/null";
    return std::system(command.c_str()) == 0;
}

std::string game_source(const std::string& short_name,
                        const std::string& display_name, unsigned players,
                        unsigned abi_version) {
    return R"(#include "whitty_game_plugin.h"
#include <cstdlib>
struct whitty_game_instance { int frames; unsigned char pixel[4]; };
namespace {
void describe(whitty_game_info* info) {
    info->short_name = ")" + short_name + R"(";
    info->display_name = ")" + display_name + R"(";
    info->publisher = "Test";
    info->max_players = )" + std::to_string(players) + R"(;
    info->supports_network = )" + (players > 1 ? "1" : "0") + R"(;
    info->refresh_hz = 60.0;
}
whitty_game_instance* create(const char*) { return new whitty_game_instance{0,{0,0,0,255}}; }
void destroy(whitty_game_instance* g) { delete g; }
void run_frame(whitty_game_instance* g, const whitty_game_input*, uint32_t,
               whitty_game_frame* out) {
    g->frames++;
    g->pixel[0] = static_cast<unsigned char>(g->frames);
    out->pixels = g->pixel; out->width = 1; out->height = 1;
    out->aspect_x = 16; out->aspect_y = 9;
}
void reset(whitty_game_instance* g) { g->frames = 0; }
void set_paused(whitty_game_instance*, uint32_t) {}
uint64_t score(whitty_game_instance* g) { return static_cast<uint64_t>(g->frames); }
uint64_t checksum(whitty_game_instance* g) { return static_cast<uint64_t>(g->frames) * 31u; }
const whitty_game_api api = { )" + std::to_string(abi_version) +
           R"(, describe, create, destroy, run_frame, reset, set_paused, score, checksum };
}
extern "C" const whitty_game_api* whitty_game_entry(void) { return &api; }
)";
}

void test_a_good_plugin_is_found_and_described(const fs::path& root) {
    game_plugin_library library;
    library.scan(root.string());
    const discovered_game* game = library.find("alpha");
    assert(game != nullptr);
    assert(game->display_name == "Alpha Game");
    assert(game->max_players == 2);
    assert(game->supports_network);
    // The folder is the bundle, and its name is the key everything else in the
    // arcade uses - scores, artwork, bezels.
    assert(fs::path(game->bundle_path).filename() == "alpha");
}

void test_a_library_that_is_not_a_game_is_reported(const fs::path& root) {
    game_plugin_library library;
    library.scan(root.string());
    bool named = false;
    for (const rejected_plugin& rejected : library.rejected())
        if (rejected.library_path.find("notagame") != std::string::npos) {
            named = true;
            assert(rejected.reason.find("whitty_game_entry") !=
                   std::string::npos);
        }
    assert(named && "a .so that is not a game must be reported, not ignored");
}

void test_a_wrong_abi_is_refused_with_both_versions(const fs::path& root) {
    game_plugin_library library;
    library.scan(root.string());
    assert(library.find("fromthefuture") == nullptr);
    bool named = false;
    for (const rejected_plugin& rejected : library.rejected())
        if (rejected.library_path.find("fromthefuture") != std::string::npos) {
            named = true;
            // The message has to say what it was built for AND what this
            // arcade speaks, or the answer is "it does not work".
            assert(rejected.reason.find("99") != std::string::npos);
            assert(rejected.reason.find(
                       std::to_string(WHITTY_GAME_ABI_VERSION)) !=
                   std::string::npos);
        }
    assert(named);
}

void test_a_duplicate_short_name_keeps_one_and_names_the_other(
    const fs::path& root) {
    game_plugin_library library;
    library.scan(root.string());
    // Exactly one survives, and the rejection says which library shadowed it -
    // otherwise a stale copy silently wins and the game looks out of date for
    // no visible reason.
    int alphas = 0;
    for (const discovered_game& game : library.games())
        if (game.short_name == "alpha") ++alphas;
    assert(alphas == 1);
    bool explained = false;
    for (const rejected_plugin& rejected : library.rejected())
        if (rejected.reason.find("already provided by") != std::string::npos)
            explained = true;
    assert(explained);
}

void test_discovery_order_does_not_depend_on_the_file_system(
    const fs::path& root) {
    // Two scans of the same tree must agree, and which duplicate won must be
    // the same both times. Directory iteration order is not guaranteed, so
    // without sorting this passes on one machine and fails on another.
    game_plugin_library first;
    game_plugin_library second;
    first.scan(root.string());
    second.scan(root.string());
    assert(first.games().size() == second.games().size());
    for (std::size_t i = 0; i < first.games().size(); ++i) {
        assert(first.games()[i].short_name == second.games()[i].short_name);
        assert(first.games()[i].library_path == second.games()[i].library_path);
    }
}

void test_a_loaded_plugin_runs_and_reports(const fs::path& root) {
    game_plugin_library library;
    library.scan(root.string());
    const discovered_game* game = library.find("alpha");
    assert(game != nullptr);
    std::string error;
    std::unique_ptr<loaded_plugin> plugin = library.load(*game, error);
    assert(plugin && plugin->valid());

    whitty_game_instance* instance = plugin->api()->create(
        game->bundle_path.c_str());
    assert(instance != nullptr);
    whitty_game_input input{};
    whitty_game_frame frame{};
    plugin->api()->run_frame(instance, &input, 1, &frame);
    assert(frame.pixels != nullptr && frame.width == 1 && frame.height == 1);
    assert(plugin->api()->score(instance) == 1);
    const uint64_t after_one = plugin->api()->state_checksum(instance);
    plugin->api()->run_frame(instance, &input, 1, &frame);
    // The checksum has to move with the simulation; a constant one would report
    // two diverged machines as being in sync, which is worse than no check.
    assert(plugin->api()->state_checksum(instance) != after_one);
    plugin->api()->reset(instance);
    assert(plugin->api()->score(instance) == 0);
    plugin->api()->destroy(instance);
}

void test_discovered_games_reach_the_catalogue(const fs::path& root) {
    game_plugin_library library;
    library.scan(root.string());
    register_plugin_games(library.games());
    const rom_set_manifest* listed = find_supported_rom_set("alpha");
    assert(listed != nullptr);
    assert(listed->board == arcade_board_type::game_plugin);
    assert(listed->working);
    // Two players means the launcher may offer it as a shared game.
    assert(listed->multiplayer == arcade_multiplayer_mode::simultaneous);
    // And the session can find its way back to the bundle from the catalogue.
    const discovered_game* back = find_plugin_game(listed->short_name);
    assert(back != nullptr && back->short_name == "alpha");
}

} // namespace

int main() {
    const fs::path root =
        fs::temp_directory_path() / "whitty_game_plugin_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    if (!build_plugin(root, "alpha",
                      game_source("alpha", "Alpha Game", 2,
                                  WHITTY_GAME_ABI_VERSION))) {
        std::printf("no host compiler available; skipping\n");
        return 0;
    }
    // Same short name, different folder: the duplicate case.
    build_plugin(root, "zulu",
                 game_source("alpha", "Alpha Game (stale copy)", 2,
                             WHITTY_GAME_ABI_VERSION));
    build_plugin(root, "fromthefuture",
                 game_source("fromthefuture", "Too New", 1, 99));
    build_plugin(root, "notagame", "extern \"C\" int unrelated() { return 0; }\n");

    test_a_good_plugin_is_found_and_described(root);
    test_a_library_that_is_not_a_game_is_reported(root);
    test_a_wrong_abi_is_refused_with_both_versions(root);
    test_a_duplicate_short_name_keeps_one_and_names_the_other(root);
    test_discovery_order_does_not_depend_on_the_file_system(root);
    test_a_loaded_plugin_runs_and_reports(root);
    test_discovered_games_reach_the_catalogue(root);

    fs::remove_all(root, ec);
    std::printf("game_plugin_test: all checks passed\n");
    return 0;
}
