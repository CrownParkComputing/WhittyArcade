#include "persistent_data.h"
#include "test_platform.h"
#include "arcade_catalog.h"

#include <array>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("manx-persistent-test-" +
         std::to_string(test_process_id()));
    const fs::path config = root / "config";
    fs::create_directories(config);
    if (!test_set_environment("XDG_CONFIG_HOME", config)) return 1;

    std::array<unsigned char, 0x2000> expected{};
    for (std::size_t index = 0; index < expected.size(); ++index)
        expected[index] = static_cast<unsigned char>(index * 17u);
    assert(save_system22_eeprom("ridgerac", expected.data(), expected.size()));
    std::array<unsigned char, 0x2000> actual{};
    assert(load_system22_eeprom("ridgerac", actual.data(), actual.size()));
    assert(actual == expected);

    auto games = persistent_games();
    for (const rom_set_manifest& manifest : supported_rom_sets()) {
        const bool has_board_storage = manifest.working &&
            (manifest.board == arcade_board_type::system22 ||
             manifest.board == arcade_board_type::system246 ||
             manifest.board == arcade_board_type::model1 ||
             manifest.board == arcade_board_type::model2);
        assert((find_persistent_game(games, manifest.short_name) != nullptr) ==
               has_board_storage);
    }
    const persistent_game_info* ridge = find_persistent_game(games, "ridgerac");
    assert(ridge && ridge->files.size() == 1 && ridge->files[0].exists);
    assert(backup_persistent_game("ridgerac").success);

    const fs::path exported = root / "export";
    fs::create_directories(exported);
    assert(export_persistent_game("ridgerac", exported.string()).success);
    assert(fs::file_size(exported / "MANX-ridgerac-save" /
                         "eeprom") == 0x2000);

    assert(reset_persistent_game("ridgerac").success);
    assert(!fs::exists(ridge->files[0].path));
    assert(import_persistent_game(
        "ridgerac",
        (exported / "MANX-ridgerac-save" / "eeprom").string()).success);
    assert(load_system22_eeprom("ridgerac", actual.data(), actual.size()));
    assert(actual == expected);
    const fs::path wrong = root / "wrong.bin";
    std::ofstream(wrong, std::ios::binary).put('\0');
    assert(!import_persistent_game("ridgerac", wrong.string()).success);

    fs::remove_all(root);
    return 0;
}
