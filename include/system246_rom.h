// Namco System 246 ROM/media discovery for Ridge Racer V: Arcade Battle.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class system246_rom_set : uint8_t {
    unknown,
    ridge_racer_v_arcade_battle,
};

enum class system246_disc_container : uint8_t {
    unknown,
    hard_disk_chd,
    cdrom_chd,
};

struct system246_disc_info {
    system246_disc_container container{system246_disc_container::unknown};
    uint64_t logical_bytes{};
    uint32_t hunk_bytes{};
    uint32_t unit_bytes{};
    std::string sha1;
    std::string metadata_tag;
    std::string error;

    explicit operator bool() const { return error.empty(); }
};

struct system246_rom_load_result {
    system246_rom_set set{system246_rom_set::unknown};
    std::string dongle_archive;
    std::string disc_path;
    std::vector<uint8_t> dongle;
    system246_disc_info disc;
    std::string error;

    explicit operator bool() const {
        return error.empty() && set != system246_rom_set::unknown &&
               !dongle.empty() && static_cast<bool>(disc);
    }
};

class system246_rom_loader {
public:
    // Identification uses the MAME entry's exact basename, size and CRC.
    // Archive filenames and internal directories are deliberately ignored.
    static system246_rom_set identify_set(const std::string& path);
    static const char* set_short_name(system246_rom_set set);
    static const char* set_display_name(system246_rom_set set);

    // Finds rrv1-a.chd either beside the selected ZIP or in the conventional
    // MAME software directory (rrvac/rrv1-a.chd).
    static std::string find_disc_path(const std::string& selected_path);
    static system246_disc_info inspect_disc(const std::string& path);
    static system246_rom_load_result load(const std::string& path);
};
