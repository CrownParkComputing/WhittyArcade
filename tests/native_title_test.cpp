// Discovering converted Xbox 360 titles and pairing them with the owned game.
//
// A converted title is half a thing: the recompiler produces native code but
// never extracts the artwork, audio or level data, so the binary mounts the
// retail package at runtime. Offer one without its game and it opens a window,
// fails to mount anything and dies - with no message naming the cause. Every
// check here is about refusing that outcome at discovery, where it can still be
// explained.

#include "arcade_catalog.h"
#include "rom_library.h"
#include "game_plugin_host.h"
#include "native_title_library.h"

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

void write_executable(const fs::path& path) {
    write_file(path, "#!/bin/sh\nexit 0\n");
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::add);
}

// A signed STFS package is recognised by its magic, not its extension.
void write_package(const fs::path& path) {
    write_file(path, std::string("LIVE") + std::string(1020, '\0'));
}

std::string catalog_entry(const std::string& slug, const std::string& name,
                          const std::string& binary, const std::string& source,
                          const std::string& status = "completed") {
    return "{\"slug\":\"" + slug + "\",\"name\":\"" + name +
           "\",\"title_id\":\"58410000\",\"binary_path\":\"" + binary +
           "\",\"source_path\":\"" + source + "\",\"status\":\"" + status +
           "\"}";
}

void test_a_converted_title_is_paired_with_its_package(const fs::path& root) {
    const fs::path binary = root / "bin" / "geometrywars2";
    const fs::path package = root / "owned" / "GW2" / "packagefile";
    write_executable(binary);
    write_package(package);
    write_file(root / "titles.json",
               "[" + catalog_entry("geometrywars2", "Geometry Wars 2",
                                   binary.string(), package.string()) + "]");

    native_title_library library;
    library.scan((root / "titles.json").string());
    const native_title* title = library.find("geometrywars2");
    assert(title != nullptr);
    assert(title->display_name == "Geometry Wars 2");
    assert(title->binary_path == binary.string());
    // A package is handed over whole; there is no separate xex or data root,
    // and inventing one would produce a command line the runtime rejects.
    assert(title->packaged());
    assert(title->package_path == package.string());
    assert(title->xex_path.empty() && title->game_root.empty());
}

void test_an_extracted_title_gets_its_xex_and_data_root(const fs::path& root) {
    const fs::path binary = root / "bin" / "robotron";
    const fs::path game = root / "owned" / "Robotron";
    write_executable(binary);
    write_file(game / "default.xex", "XEX2 pretend");
    write_file(game / "media" / "data.bin", "data");
    write_file(root / "extracted.json",
               "[" + catalog_entry("robotron", "Robotron", binary.string(),
                                   game.string()) + "]");

    native_title_library library;
    library.scan((root / "extracted.json").string());
    const native_title* title = library.find("robotron");
    assert(title != nullptr);
    // Extracted titles need BOTH: the image to load and the directory to read
    // data from. Handing over only one is the difference between a game that
    // runs and one that starts and stops.
    assert(!title->packaged());
    assert(title->xex_path == (game / "default.xex").string());
    assert(title->game_root == game.string());
}

void test_a_title_whose_owned_game_moved_is_refused_by_name(
    const fs::path& root) {
    // The failure this whole file exists for. The binary is fine; the game it
    // was converted from is no longer on the machine. Offering it would give a
    // window that opens and closes with nothing said.
    const fs::path binary = root / "bin" / "daytona";
    write_executable(binary);
    write_file(root / "moved.json",
               "[" + catalog_entry("daytona", "Daytona USA", binary.string(),
                                   (root / "not" / "here").string()) + "]");

    native_title_library library;
    library.scan((root / "moved.json").string());
    assert(library.find("daytona") == nullptr);
    bool explained = false;
    for (const rejected_title& rejected : library.rejected())
        if (rejected.short_name == "daytona") {
            explained = true;
            // The message has to name the path, or the answer is "it does not
            // work" and the player has nothing to go and look at.
            assert(rejected.reason.find("not") != std::string::npos);
            assert(rejected.reason.find((root / "not" / "here").string()) !=
                   std::string::npos);
        }
    assert(explained);
}

void test_a_missing_binary_is_refused_by_name(const fs::path& root) {
    const fs::path package = root / "owned" / "GW1" / "packagefile";
    write_package(package);
    write_file(root / "nobinary.json",
               "[" + catalog_entry("geometrywars", "Geometry Wars",
                                   (root / "bin" / "never_built").string(),
                                   package.string()) + "]");

    native_title_library library;
    library.scan((root / "nobinary.json").string());
    assert(library.find("geometrywars") == nullptr);
    bool explained = false;
    for (const rejected_title& rejected : library.rejected())
        if (rejected.short_name == "geometrywars") explained = true;
    assert(explained && "a title with no built binary must be named, not "
                        "silently dropped");
}

void test_a_broken_catalogue_does_not_stop_the_arcade(const fs::path& root) {
    // MANX must still start. A catalogue that cannot be parsed is one
    // feature missing, not a machine that will not boot.
    write_file(root / "broken.json", "{ this is not json at all ");
    native_title_library library;
    library.scan((root / "broken.json").string());
    assert(library.titles().empty());
    assert(!library.rejected().empty() && "a broken catalogue must be reported");

    // A catalogue that simply is not there means no conversions are installed,
    // which is normal and not worth complaining about.
    native_title_library absent;
    absent.scan((root / "does_not_exist.json").string());
    assert(absent.titles().empty());
    assert(absent.rejected().empty());
}

void test_discovery_order_is_stable(const fs::path& root) {
    const fs::path a = root / "bin" / "alpha_title";
    const fs::path b = root / "bin" / "beta_title";
    write_executable(a);
    write_executable(b);
    const fs::path package = root / "owned" / "shared" / "packagefile";
    write_package(package);
    // Deliberately out of order in the file.
    write_file(root / "order.json",
               "[" + catalog_entry("zulu", "Zulu", b.string(),
                                   package.string()) +
                   "," + catalog_entry("alpha", "Alpha", a.string(),
                                       package.string()) + "]");

    native_title_library library;
    library.scan((root / "order.json").string());
    assert(library.titles().size() == 2);
    assert(library.titles()[0].short_name == "alpha" &&
           "the launcher's order must not depend on the file's order");
    assert(library.titles()[1].short_name == "zulu");
}

void test_titles_reach_the_catalogue_without_evicting_plugins(
    const fs::path& root) {
    const fs::path binary = root / "bin" / "catalogued";
    const fs::path package = root / "owned" / "cat" / "packagefile";
    write_executable(binary);
    write_package(package);
    write_file(root / "cat.json",
               "[" + catalog_entry("catalogued", "Catalogued Title",
                                   binary.string(), package.string()) + "]");

    native_title_library library;
    library.scan((root / "cat.json").string());
    register_native_titles(library.titles());

    const rom_set_manifest* listed = find_supported_rom_set("catalogued");
    assert(listed != nullptr);
    assert(listed->board == arcade_board_type::xbox360);
    assert(listed->working);
    // And the launch path can get back to the binary and the owned game.
    const native_title* back = find_native_title("catalogued");
    assert(back != nullptr && back->binary_path == binary.string());

    // Registering plugins afterwards must not evict the titles, and vice
    // versa: both rebuild one shared table, so whichever ran second used to
    // silently drop the other's games.
    register_plugin_games({});
    assert(find_supported_rom_set("catalogued") != nullptr &&
           "registering plugins dropped the converted titles");
    register_native_titles(library.titles());
    assert(find_supported_rom_set("catalogued") != nullptr);
}

void test_a_converted_title_replaces_a_same_named_row(const fs::path& root) {
    // The catalogue keys scores, artwork and bundles on the short name, so two
    // rows sharing one is an ambiguous key rather than a duplicate listing.
    // Space Giraffe really did appear twice: once from the built-in list and
    // once as a converted title.
    const fs::path binary = root / "bin" / "spacegiraffe";
    const fs::path package = root / "owned" / "sg" / "packagefile";
    write_executable(binary);
    write_package(package);
    write_file(root / "sg.json",
               "[" + catalog_entry("spacegiraffe", "Space Giraffe (converted)",
                                   binary.string(), package.string()) + "]");

    // The built-in catalogue already carries a spacegiraffe row.
    assert(find_supported_rom_set("spacegiraffe") != nullptr);

    native_title_library library;
    library.scan((root / "sg.json").string());
    register_native_titles(library.titles());

    int rows = 0;
    for (const rom_set_manifest& row : supported_rom_sets())
        if (row.short_name != nullptr &&
            std::string(row.short_name) == "spacegiraffe")
            ++rows;
    assert(rows == 1 && "a converted title must replace the row it duplicates");
    // And the surviving row is the converted one, which knows how to run it.
    assert(find_native_title("spacegiraffe") != nullptr);
}

void test_import_state_is_reported(const fs::path& root) {
    // A recompiled title plays from the owned package whether or not anything
    // has been imported, so an imported and a never-imported title look
    // identical everywhere else. This is the only thing that tells them apart.
    const char* home = std::getenv("HOME");
    const char* data = std::getenv("XDG_DATA_HOME");
    const fs::path base = data != nullptr && *data != '\0'
                              ? fs::path(data)
                              : fs::path(home != nullptr ? home : "") /
                                    ".local" / "share";
    const fs::path bundle =
        base / "MANX" / "games" / "manx_import_state_test";

    // Nothing installed: reported as nothing, not as an error.
    std::error_code ec;
    fs::remove_all(bundle, ec);
    assert(!imported_assets_for("manx_import_state_test").any());
    assert(!imported_assets_for("").any());

    write_file(bundle / "sfx" / "shot.wav", "RIFF");
    write_file(bundle / "sfx" / "bomb.wav", "RIFF");
    write_file(bundle / "art" / "boxart.png", "PNG");
    // Only .wav counts as a sound; the staging directory an import leaves
    // behind must not be mistaken for one.
    write_file(bundle / "sfx" / "notes.txt", "ignore me");
    const imported_assets found =
        imported_assets_for("manx_import_state_test");
    assert(found.sounds == 2 && "only .wav files are sounds");
    assert(found.artwork == 1);
    assert(found.any());
    fs::remove_all(bundle, ec);
    (void)root;
}

void test_the_games_list_says_a_title_is_recomp(const fs::path& root) {
    // A recompiled title needs no ROM at all. Without saying so, an entry that
    // is complete reads exactly like one whose ROM is missing - and the two
    // want opposite actions from whoever is reading the list.
    const fs::path binary = root / "bin" / "listed";
    const fs::path package = root / "owned" / "listed" / "packagefile";
    write_executable(binary);
    write_package(package);
    write_file(root / "listed.json",
               "[" + catalog_entry("listed", "Listed Title", binary.string(),
                                   package.string()) + "]");
    native_title_library library;
    library.scan((root / "listed.json").string());
    register_native_titles(library.titles());

    const std::string text = required_rom_sets_text();
    const std::size_t at = text.find("listed - Listed Title");
    assert(at != std::string::npos && "the title is missing from the list");
    const std::string entry = text.substr(at, 400);
    assert(entry.find("[recomp]") != std::string::npos);
    // The binary and the game it plays from, because "where did this come
    // from" is the first question a listing like this has to answer.
    assert(entry.find(binary.string()) != std::string::npos);
    assert(entry.find(package.string()) != std::string::npos);
    // And the import state, stated even when nothing has been imported.
    assert(entry.find("Imported:") != std::string::npos);
}

} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "manx_native_title_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    test_a_converted_title_is_paired_with_its_package(root);
    test_an_extracted_title_gets_its_xex_and_data_root(root);
    test_a_title_whose_owned_game_moved_is_refused_by_name(root);
    test_a_missing_binary_is_refused_by_name(root);
    test_a_broken_catalogue_does_not_stop_the_arcade(root);
    test_discovery_order_is_stable(root);
    test_titles_reach_the_catalogue_without_evicting_plugins(root);
    test_a_converted_title_replaces_a_same_named_row(root);
    test_import_state_is_reported(root);
    test_the_games_list_says_a_title_is_recomp(root);

    fs::remove_all(root, ec);
    std::printf("native_title_test: all checks passed\n");
    return 0;
}
