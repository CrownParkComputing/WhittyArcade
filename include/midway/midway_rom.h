// Killer Instinct (Midway Wolf Unit) ROM loader.
//
// Identifies kinst and kinst2 ROM sets by probing for the boot ROM
// and optional hard disk image. Accepts ZIP archives and extracted
// directories.
//
// The Wolf Unit board loads its program from a small boot ROM and
// then boots the game off an IDE hard disk - the CHD file.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class midway_rom_set : uint8_t {
    unknown,
    killer_instinct,    // kinst  (v1.5d)
    killer_instinct_2,  // kinst2 (v1.4)
};

struct midway_rom_load_result {
    midway_rom_set set{midway_rom_set::unknown};
    std::vector<uint8_t> boot_rom;
    std::vector<uint8_t> game_roms;  // u10-u36 contiguous game ROMs (8 x 512KB = 4MB)
    std::string disc_path;
    std::string error;

    explicit operator bool() const {
        return error.empty() && set != midway_rom_set::unknown &&
               !boot_rom.empty();
    }
};

class midway_rom_loader {
public:
    static midway_rom_set identify_set(const std::string& path);
    static const char*      set_short_name(midway_rom_set set);
    static const char*      set_display_name(midway_rom_set set);

    // Locate the .chd disc image beside the selected ROM or in the
    // configured CHD directory.
    static std::string find_disc_path(
        const std::string& selected_path,
        const std::string& configured_chd_directory = {});

    static midway_rom_load_result load(
        const std::string& path,
        const std::string& configured_chd_directory = {});
};
