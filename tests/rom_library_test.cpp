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
    // And from the machine's real extracted Xbox 360 titles, for the same
    // reason: the native-port scan would otherwise add whatever is installed.
    set_test_environment("MANX_XBOX360_GAME_ROOT", "/nonexistent-xbox360-game-root");
    // And from the machine's downloaded Xbox 360 packages, which is where the
    // package scan looks by default.
    set_test_environment("MANX_XBOX360_PACKAGE_ROOT",
             "/nonexistent-xbox360-package-root");
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

    // An Xbox 360 title that was never extracted is one signed STFS package
    // stored the way the console stores it: <TITLE ID>/<content type>/<content
    // hash>, below a directory named after the store listing, so the path has
    // spaces and the filename is a hash. It is neither an archive nor a
    // directory, so only the package pass can see it - and a package that is all
    // there is a ready title, because everything it reads is inside it.
    {
        const auto discover_package = [&](const fs::path& packages,
                                          const fs::path& package) {
            set_test_environment("MANX_XBOX360_PACKAGE_ROOT", packages.string().c_str());
            const auto found = discover_library_roms("");
            set_test_environment("MANX_XBOX360_PACKAGE_ROOT",
                     "/nonexistent-xbox360-package-root");
            const std::string wanted =
                fs::absolute(package).lexically_normal().string();
            for (const rom_choice& choice : found)
                if (choice.path == wanted) return choice;
            assert(false && "the package was not discovered");
            return rom_choice{};
        };

        // Space Giraffe rather than Geometry Wars: both Geometry Wars titles
        // are now native game plugins, discovered on disk rather than carried
        // as Xbox 360 ROM sets, so a package of one is deliberately no longer
        // recognised here. Space Giraffe still ships as a package and exercises
        // exactly the same path.
        const fs::path whole_root = root / "downloads";
        const fs::path whole = whole_root / "Space Giraffe" /
                               "5841080C" / "000D0000" /
                               "834312072F4985F9D33D0B9549FFAA32C505FDAD58";
        // A real package declares a 0xad0e-byte header, which STFS rounds up
        // to 0xb000.
        make_package(whole, 0x5841080c, 0xad0e, 0xb000);
        const rom_choice sequel = discover_package(whole_root, whole);
        assert(sequel.board == arcade_board_type::xbox360);
        assert(sequel.label.find("[ready]") != std::string::npos);
        assert(sequel.path.find(' ') != std::string::npos &&
               "the console's layout puts spaces in the path");
        const rom_audit_result audited = audit_rom_path(sequel.path);
        assert(audited.success);
        assert(audited.set_name == "spacegiraffe");

        // A part-downloaded package is still listed, so the game is visible
        // rather than silently absent, but it is not ready and the audit says
        // which file fell short.
        const fs::path partial_root = root / "interrupted";
        const fs::path partial = partial_root / "Space Giraffe" /
                                 "5841080C" / "000D0000" /
                                 "834312072F4985F9D33D0B9549FFAA32C505FDAD58";
        make_package(partial, 0x5841080c, 0xad0e, 0x1000);
        const rom_choice incomplete = discover_package(partial_root, partial);
        assert(incomplete.label.find("[ready]") == std::string::npos);
        const rom_audit_result failed = audit_rom_path(incomplete.path);
        assert(!failed.success);
        assert(failed.message.find(partial.filename().string()) !=
               std::string::npos);
    }

    // Space Giraffe was dumped both ways, and both copies play. A machine can
    // therefore hold two of the same game, and exactly one may be offered -
    // chosen by the title's registered preference, not by which of the two passes
    // ran first or which entry the filesystem listed first.
    {
        const std::uint32_t giraffe_id = 0x5841080cu;
        const fs::path games = root / "suite-games";
        const fs::path extraction = games / "spacegiraffe" / "extracted";
        make_xex(extraction / "default.xex", giraffe_id);
        touch_file(extraction / "Media" / "modules.ox");
        touch_file(extraction / "Media" / "sounds" / "GRsounds.xwb");
        const fs::path packages = root / "giraffe-downloads";
        const fs::path package = packages / "Space Giraffe" / "5841080C" /
                                 "000D0000" /
                                 "5B340CF9CCDDEC28B0810E99A01D0877D960B42358";
        make_package(package, giraffe_id, 0xad0e, 0xb000);

        const auto giraffes = [](const fs::path& game_root,
                                 const fs::path& package_root) {
            set_test_environment("MANX_XBOX360_GAME_ROOT", game_root.string().c_str());
            set_test_environment("MANX_XBOX360_PACKAGE_ROOT",
                     package_root.string().c_str());
            const auto found = discover_library_roms("");
            set_test_environment("MANX_XBOX360_GAME_ROOT",
                     "/nonexistent-xbox360-game-root");
            set_test_environment("MANX_XBOX360_PACKAGE_ROOT",
                     "/nonexistent-xbox360-package-root");
            std::vector<rom_choice> listed;
            for (const rom_choice& choice : found)
                if (choice.label.find("Space Giraffe") != std::string::npos)
                    listed.push_back(choice);
            return listed;
        };

        const std::vector<rom_choice> both = giraffes(games, packages);
        assert(both.size() == 1 && "the same game was offered twice");
        assert(both.front().board == arcade_board_type::xbox360);
        assert(both.front().path ==
               fs::absolute(package).lexically_normal().string() &&
               "the extracted copy was offered over the preferred package");
        assert(both.front().label.find("[ready]") != std::string::npos);
        const rom_audit_result audited = audit_rom_path(both.front().path);
        assert(audited.success);
        assert(audited.set_name == "spacegiraffe");

        // A preference is not a requirement: with only the extraction on the
        // machine, the game is offered from the copy that is there.
        const std::vector<rom_choice> extracted_only =
            giraffes(games, "/nonexistent-xbox360-package-root");
        assert(extracted_only.size() == 1);
        assert(extracted_only.front().path ==
               fs::absolute(extraction).lexically_normal().string());
        assert(extracted_only.front().label.find("[ready]") !=
               std::string::npos);
        assert(audit_rom_path(extracted_only.front().path).success);

        // And an extraction that got only the executable - a valid XEX with the
        // right title ID and none of Media/ - is the copy that faults on the
        // first asset it reads. It is listed, so it can be seen and fixed, but it
        // is never ready and never audits clean.
        const fs::path broken_games = root / "broken-games";
        const fs::path broken = broken_games / "spacegiraffe" / "extracted";
        make_xex(broken / "default.xex", giraffe_id);
        touch_file(broken / "image.bin");
        const std::vector<rom_choice> partial_extraction =
            giraffes(broken_games, "/nonexistent-xbox360-package-root");
        assert(partial_extraction.size() == 1);
        assert(partial_extraction.front().path ==
               fs::absolute(broken).lexically_normal().string());
        assert(partial_extraction.front().label.find("[ready]") ==
               std::string::npos);
        const rom_audit_result broken_audit =
            audit_rom_path(partial_extraction.front().path);
        assert(!broken_audit.success);
        assert(broken_audit.message.find("Media/modules.ox") !=
               std::string::npos);
    }

    fs::remove_all(root);
    return 0;
}
