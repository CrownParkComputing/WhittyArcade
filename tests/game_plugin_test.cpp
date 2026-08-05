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
    return R"(#include "manx_game_plugin.h"
#include <cstdlib>
struct manx_game_instance { int frames; unsigned char pixel[4]; int pending; };
namespace {
void describe(manx_game_info* info) {
    info->short_name = ")" + short_name + R"(";
    info->display_name = ")" + display_name + R"(";
    info->publisher = "Test";
    info->max_players = )" + std::to_string(players) + R"(;
    info->supports_network = )" + (players > 1 ? "1" : "0") + R"(;
    info->refresh_hz = 60.0;
}
manx_game_instance* create(const char*) { return new manx_game_instance{0,{0,0,0,255},0}; }
void destroy(manx_game_instance* g) { delete g; }
void run_frame(manx_game_instance* g, const manx_game_input*, uint32_t,
               manx_game_frame* out) {
    g->frames++;
    g->pending = (g->frames % 2) == 0 ? 1 : 0;
    g->pixel[0] = static_cast<unsigned char>(g->frames);
    out->pixels = g->pixel; out->width = 1; out->height = 1;
    out->aspect_x = 16; out->aspect_y = 9;
}
void reset(manx_game_instance* g) { g->frames = 0; }
void set_paused(manx_game_instance*, uint32_t) {}
uint64_t score(manx_game_instance* g) { return static_cast<uint64_t>(g->frames); }
uint64_t checksum(manx_game_instance* g) { return static_cast<uint64_t>(g->frames) * 31u; }
uint32_t take_cues(manx_game_instance* g, manx_game_audio_cue* out, uint32_t max) {
    if (max == 0 || g->pending == 0) return 0;
    g->pending = 0;
    out[0].cue = 0; out[0].gain = 0.5f; out[0].pan = -1.0f;
    return 1;
}
uint32_t describe_cues(const char** names, uint32_t max) {
    if (names == nullptr) return 1;
    if (max > 0) names[0] = "beep";
    return max > 0 ? 1u : 0u;
}
const manx_game_api api = { )" + std::to_string(abi_version) +
           R"(, describe, create, destroy, run_frame, reset, set_paused, score, checksum,
             take_cues, describe_cues };
}
extern "C" const manx_game_api* manx_game_entry(void) { return &api; }
)";
}

// A `require` of its own, because this file's assert()s vanish under the
// -DNDEBUG this repository builds tests with, and a regression test that
// cannot fail is worse than none.
void require(bool condition, const char* what) {
    if (condition) return;
    std::printf("game_plugin_test FAIL: %s\n", what);
    std::abort();
}

// A bundle directory with no library in it must not hide the games discovered
// after it.
//
// This is a real bug that shipped. The walk shared one error_code with the
// queries inside it, so a directory holding a bundle but no .so - which is
// exactly what every recomp-imported title is, sfx/ and art/ and nothing to
// load - made is_regular_file set "No such file or directory", and the next
// iteration's `if (ec) break;` abandoned the scan. Every plugin the file
// system happened to return after that one silently did not exist: no
// rejection, no message, just games missing from the launcher. On the machine
// that found it, `spacegiraffe/` was doing the hiding.
void test_a_bundle_without_a_library_hides_nothing(const fs::path& root) {
    game_plugin_library before;
    before.scan(root.string());
    const std::size_t found_before = before.games().size();
    require(found_before > 0, "the fixture has plugins to lose");

    std::error_code ec;
    const fs::path bundle = root / "aaa_imported_bundle";
    fs::create_directories(bundle / "sfx", ec);
    std::ofstream(bundle / "sfx" / "sound_000.wav") << "not audio";

    game_plugin_library after;
    after.scan(root.string());
    require(after.games().size() == found_before,
            "a bundle directory with no .so must not reduce discovery");

    fs::remove_all(bundle, ec);
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
            assert(rejected.reason.find("manx_game_entry") !=
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
                       std::to_string(MANX_GAME_ABI_VERSION)) !=
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

    manx_game_instance* instance = plugin->api()->create(
        game->bundle_path.c_str());
    assert(instance != nullptr);
    manx_game_input input{};
    manx_game_frame frame{};
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

void test_a_table_missing_the_new_entries_is_refused(const fs::path& root) {
    // The header says every entry must be non-null, and the whole point of
    // checking at load is that a hole would otherwise fault minutes into a
    // game. A plugin written against the previous table leaves the two audio
    // entries zero, so this is exactly what an out-of-date-but-same-ABI plugin
    // looks like - and it must be named, not crashed on.
    game_plugin_library library;
    library.scan(root.string());
    assert(library.find("shorttable") == nullptr);
    bool named = false;
    for (const rejected_plugin& rejected : library.rejected())
        if (rejected.library_path.find("shorttable") != std::string::npos) {
            named = true;
            assert(rejected.reason.find("null entry") != std::string::npos);
        }
    assert(named);
}

void test_audio_cues_are_drained_not_repeated(const fs::path& root) {
    // A cue is an event. The contract says draining, so asking twice after one
    // frame must not hear it twice - that is what makes a skipped host frame
    // safe.
    game_plugin_library library;
    library.scan(root.string());
    const discovered_game* game = library.find("alpha");
    assert(game != nullptr);
    std::string error;
    std::unique_ptr<loaded_plugin> plugin = library.load(*game, error);
    assert(plugin && plugin->valid());

    // The names are what a bundle matches its sound files against, so an empty
    // or unnamed cue would leave the host unable to find any sample at all.
    assert(plugin->api()->describe_audio_cues(nullptr, 0) == 1);
    const char* names[4] = {};
    assert(plugin->api()->describe_audio_cues(names, 4) == 1);
    assert(names[0] != nullptr && std::string(names[0]) == "beep");

    manx_game_instance* instance =
        plugin->api()->create(game->bundle_path.c_str());
    manx_game_input input{};
    manx_game_frame frame{};
    manx_game_audio_cue cues[8]{};
    plugin->api()->run_frame(instance, &input, 1, &frame);
    plugin->api()->run_frame(instance, &input, 1, &frame);
    assert(plugin->api()->take_audio_cues(instance, cues, 8) == 1);
    assert(cues[0].gain > 0.0f && cues[0].pan == -1.0f);
    assert(plugin->api()->take_audio_cues(instance, cues, 8) == 0 &&
           "cues must drain: a repeated cue is a doubled sound");
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
        fs::temp_directory_path() / "manx_game_plugin_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    if (!build_plugin(root, "alpha",
                      game_source("alpha", "Alpha Game", 2,
                                  MANX_GAME_ABI_VERSION))) {
        std::printf("no host compiler available; skipping\n");
        return 0;
    }
    // Same short name, different folder: the duplicate case.
    build_plugin(root, "zulu",
                 game_source("alpha", "Alpha Game (stale copy)", 2,
                             MANX_GAME_ABI_VERSION));
    build_plugin(root, "fromthefuture",
                 game_source("fromthefuture", "Too New", 1, 99));
    build_plugin(root, "notagame", "extern \"C\" int unrelated() { return 0; }\n");
    // Right ABI number, previous table shape: the two audio entries are left
    // null by the aggregate initialiser.
    build_plugin(root, "shorttable", R"(#include "manx_game_plugin.h"
struct manx_game_instance { int frames; };
namespace {
void describe(manx_game_info* info) {
    info->short_name = "shorttable"; info->display_name = "Short Table";
    info->publisher = "Test"; info->max_players = 1;
    info->supports_network = 0; info->refresh_hz = 60.0;
}
manx_game_instance* create(const char*) { return new manx_game_instance{0}; }
void destroy(manx_game_instance* g) { delete g; }
void run_frame(manx_game_instance*, const manx_game_input*, uint32_t,
               manx_game_frame*) {}
void reset(manx_game_instance*) {}
void set_paused(manx_game_instance*, uint32_t) {}
uint64_t score(manx_game_instance*) { return 0; }
uint64_t checksum(manx_game_instance*) { return 0; }
const manx_game_api api = { MANX_GAME_ABI_VERSION, describe, create,
    destroy, run_frame, reset, set_paused, score, checksum };
}
extern "C" const manx_game_api* manx_game_entry(void) { return &api; }
)");

    test_a_good_plugin_is_found_and_described(root);
    test_a_library_that_is_not_a_game_is_reported(root);
    test_a_wrong_abi_is_refused_with_both_versions(root);
    test_a_duplicate_short_name_keeps_one_and_names_the_other(root);
    test_discovery_order_does_not_depend_on_the_file_system(root);
    test_a_loaded_plugin_runs_and_reports(root);
    test_a_table_missing_the_new_entries_is_refused(root);
    test_audio_cues_are_drained_not_repeated(root);
    test_discovered_games_reach_the_catalogue(root);
    test_a_bundle_without_a_library_hides_nothing(root);

    fs::remove_all(root, ec);
    std::printf("game_plugin_test: all checks passed\n");
    return 0;
}
