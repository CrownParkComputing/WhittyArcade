#include "rom_library.h"

#include <minizip/zip.h>

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {
void make_zip(const fs::path& path, const std::vector<std::string>& entries) {
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
    const fs::path root = fs::temp_directory_path() /
        ("whittyarcade-rom-library-test-" + std::to_string(getpid()));
    const fs::path source = root / "mame";
    const fs::path data = root / "data";
    fs::create_directories(source);
    setenv("XDG_DATA_HOME", data.c_str(), 1);
    setenv("WHITTYARCADE_NO_LEGACY_ROM_SCAN", "1", 1);

    // A merged archive may use per-set subdirectories; identification is by
    // basename and import must retain the archive unchanged.
    make_zip(source / "ridgerac.zip", {"ridgerac/rr2_prgllb.4d"});
    make_zip(source / "namcoc71.zip", {"c71.bin"});
    make_zip(source / "namcoc74.zip", {"c74.bin"});
    make_zip(source / "unrelated.zip", {"nothing.bin"});

    const rom_import_result imported = import_rom_path(source.string());
    assert(!imported.error.empty()); // Synthetic one-byte ROMs fail the audit.
    assert(imported.archives_scanned == 4);
    assert(imported.games_found == 1);
    assert(imported.archives_imported == 3);
    assert(fs::is_regular_file(data / "WhittyArcade" / "roms" /
                               "system22" / "ridgerac.zip"));
    assert(fs::is_regular_file(data / "WhittyArcade" / "roms" /
                               "system22" / "namcoc71.zip"));

    const auto discovered = discover_library_roms("");
    assert(discovered.size() == 1);
    assert(discovered.front().board == arcade_board_type::system22);
    assert(discovered.front().label.find("[ready]") != std::string::npos);
    const std::string required_sets = required_rom_sets_text();
    assert(supported_rom_sets().size() >= 15);
    for (const rom_set_manifest& manifest : supported_rom_sets()) {
        assert(required_sets.find(manifest.short_name) != std::string::npos);
        assert(arcade_board_index(manifest.board) < arcade_board_count);
    }
    assert(!audit_rom_path(discovered.front().path).success);

    fs::remove_all(root);
    return 0;
}
