#include "arcade_settings.h"
#include "rom_library.h"
#include "test_platform.h"

#include <minizip/zip.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// setenv is POSIX and MinGW has no such thing; Windows spells it _putenv_s.
// The test only ever sets a variable and expects it to stick, which both do.
void set_test_environment(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
#endif
}

} // namespace

namespace {
// entry_bytes matters for the device firmware archives: the loader checks a
// device against its real size, so a one-byte stub reads as a corrupt C74 and
// the set never reports itself ready.
void make_zip(const fs::path& path, const std::vector<std::string>& entries,
              std::size_t entry_bytes = 1) {
    fs::create_directories(path.parent_path());
    zipFile archive = zipOpen64(path.string().c_str(), APPEND_STATUS_CREATE);
    assert(archive);
    for (const std::string& entry : entries) {
        zip_fileinfo info{};
        assert(zipOpenNewFileInZip(archive, entry.c_str(), &info, nullptr, 0,
                                   nullptr, 0, nullptr, Z_DEFLATED,
                                   Z_BEST_SPEED) == ZIP_OK);
        const std::vector<char> body(std::max<std::size_t>(entry_bytes, 1), 0);
        assert(zipWriteInFileInZip(archive, body.data(),
                                   static_cast<unsigned>(body.size())) ==
               ZIP_OK);
        assert(zipCloseFileInZip(archive) == ZIP_OK);
    }
    assert(zipClose(archive, nullptr) == ZIP_OK);
}

void touch_file(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).put('\0');
}

// A stand-in extracted title: an XEX2 image carrying nothing but the
// execution-info title ID, which is all identification reads.
void make_xex(const fs::path& path, std::uint32_t title_id) {
    fs::create_directories(path.parent_path());
    std::vector<unsigned char> bytes(0x100, 0);
    std::memcpy(bytes.data(), "XEX2", 4);
    const auto put_be32 = [&bytes](std::size_t offset, std::uint32_t value) {
        bytes[offset] = static_cast<unsigned char>(value >> 24);
        bytes[offset + 1] = static_cast<unsigned char>(value >> 16);
        bytes[offset + 2] = static_cast<unsigned char>(value >> 8);
        bytes[offset + 3] = static_cast<unsigned char>(value);
    };
    // One optional header, the execution info, at 0x40; the title ID sits 0x0c
    // into it.
    put_be32(0x14, 1);
    put_be32(0x18, 0x00040006);
    put_be32(0x1c, 0x40);
    put_be32(0x4c, title_id);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

// A stand-in Xbox 360 STFS package: the signature at offset zero, the header
// size it claims, and the title ID. Discovery has nothing else to go on - the
// real files are named after the SHA-1 of their content.
void make_package(const fs::path& path, std::uint32_t title_id,
                  std::uint32_t declared_header_size, std::size_t size) {
    fs::create_directories(path.parent_path());
    std::vector<unsigned char> bytes(size);
    std::memcpy(bytes.data(), "LIVE", 4);
    const auto put_be32 = [&bytes](std::size_t offset, std::uint32_t value) {
        bytes[offset] = static_cast<unsigned char>(value >> 24);
        bytes[offset + 1] = static_cast<unsigned char>(value >> 16);
        bytes[offset + 2] = static_cast<unsigned char>(value >> 8);
        bytes[offset + 3] = static_cast<unsigned char>(value);
    };
    put_be32(0x340, declared_header_size);
    put_be32(0x360, title_id);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}
} // namespace

int main() {
    // Keep discovery isolated from the machine's real PCSX2 arcade tree so the
    // System 246 .acgame scan does not add extra games to this fixture.
    set_test_environment("MANX_SYSTEM246_ACGAME_ROOT", "/nonexistent-system246-acgame-root");
    const fs::path root = fs::temp_directory_path() /
        ("manx-rom-library-test-" +
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

    // MANX reads set ZIPs straight from its ROM folder. Drop a merged
    // ridgerac.zip (per-set subdir, identified by basename) plus the C71/C74
    // device archives directly into the folder - there is no import step.
    const fs::path rom_folder(rom_library_path());
    make_zip(rom_folder / "ridgerac.zip", {"ridgerac/rr2_prgllb.4d"});
    make_zip(rom_folder / "namcoc71.zip", {"c71.bin"}, 0x2000);
    make_zip(rom_folder / "namcoc74.zip", {"c74.bin"}, 0x4000);
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
    // MANX-managed import tree.

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
