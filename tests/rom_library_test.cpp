#include "arcade_settings.h"
#include "rom_library.h"
#include "test_platform.h"

#include <minizip/zip.h>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
void make_zip(const fs::path& path, const std::vector<std::string>& entries) {
    fs::create_directories(path.parent_path());
    zipFile archive = zipOpen64(path.string().c_str(), APPEND_STATUS_CREATE);
    assert(archive);
    for (const std::string& entry : entries) {
        zip_fileinfo info{};
        assert(zipOpenNewFileInZip(archive, entry.c_str(), &info, nullptr, 0,
                                   nullptr, 0, nullptr, Z_DEFLATED,
                                   Z_BEST_SPEED) == ZIP_OK);
        const char byte = 0;
        assert(zipWriteInFileInZip(archive, &byte, 1) == ZIP_OK);
        assert(zipCloseFileInZip(archive) == ZIP_OK);
    }
    assert(zipClose(archive, nullptr) == ZIP_OK);
}
} // namespace

int main() {
    // Keep discovery isolated from the machine's real PCSX2 arcade tree so the
    // System 246 .acgame scan does not add extra games to this fixture.
    ::setenv("WHITTY_SYSTEM246_ACGAME_ROOT", "/nonexistent-system246-acgame-root",
             1);
    // And from the machine's real extracted Xbox 360 titles, for the same
    // reason: the native-port scan would otherwise add whatever is installed.
    ::setenv("WHITTY_XBOX360_GAME_ROOT", "/nonexistent-xbox360-game-root", 1);
    const fs::path root = fs::temp_directory_path() /
        ("whittyarcade-rom-library-test-" +
         std::to_string(test_process_id()));
    const fs::path data = root / "data";
    fs::create_directories(data);
    if (!test_set_environment("XDG_DATA_HOME", data)) return 1;
    if (!test_set_environment("XDG_CONFIG_HOME", root / "config")) return 1;

    emulator_settings settings;
    settings.rom_directory = (root / "my-roms").string();
    settings.chd_directory = (root / "my-discs").string();
    settings.library_setup_complete = true;
    if (!save_settings(settings)) return 1;

    // WhittyArcade reads set ZIPs straight from its ROM folder. Drop a merged
    // ridgerac.zip (per-set subdir, identified by basename) plus the C71/C74
    // device archives directly into the folder - there is no import step.
    const fs::path rom_folder(rom_library_path());
    make_zip(rom_folder / "ridgerac.zip", {"ridgerac/rr2_prgllb.4d"});
    make_zip(rom_folder / "namcoc71.zip", {"c71.bin"});
    make_zip(rom_folder / "namcoc74.zip", {"c74.bin"});
    make_zip(rom_folder / "unrelated.zip", {"nothing.bin"});

    const auto discovered = discover_library_roms("");
    assert(discovered.size() == 1);
    assert(discovered.front().board == arcade_board_type::system22);
    // The device companions sit beside the set, so it reads as ready.
    assert(discovered.front().label.find("[ready]") != std::string::npos);

    // The ROM and CHD folders live side by side under the data directory, and
    // discovery creates both so the user knows where to drop files.
    assert(fs::is_directory(rom_library_path()));
    assert(fs::is_directory(chd_library_path()));
    assert(fs::path(rom_library_path()) == root / "my-roms");
    assert(fs::path(chd_library_path()) == root / "my-discs");
    // Custom locations are used directly rather than being copied beneath a
    // WhittyArcade-managed import tree.

    const std::string required_sets = required_rom_sets_text();
    assert(supported_rom_sets().size() >= 15);
    for (const rom_set_manifest& manifest : supported_rom_sets()) {
        assert(required_sets.find(manifest.short_name) != std::string::npos);
        assert(arcade_board_index(manifest.board) < arcade_board_count);
    }
    // Synthetic one-byte ROMs fail the read-only audit.
    assert(!audit_rom_path(discovered.front().path).success);

    fs::remove_all(root);
    return 0;
}
