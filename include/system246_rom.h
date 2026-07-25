// Namco System 246 ROM/media discovery for Ridge Racer V: Arcade Battle.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class system246_rom_set : uint8_t {
    unknown,
    ridge_racer_v_arcade_battle,
    motogp,
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
    static std::string find_disc_path(
        const std::string& selected_path,
        const std::string& configured_chd_directory = {});
    static system246_disc_info inspect_disc(const std::string& path);
    static system246_rom_load_result load(
        const std::string& path,
        const std::string& configured_chd_directory = {});

    // System 246/256 games boot the PCSX2 arcade core through a small
    // "<name>.acgame" manifest that names the ELF loader, dongle image and
    // disc/HDD media in the same directory. A game is "ready" when that
    // manifest and every file it references exist beside it; this lets both
    // RRV and MotoGP be marked ready without hard-coding a single game's
    // file names. When it returns false and missing is non-null, missing
    // names the first absent item.
    static bool acgame_ready(const std::string& path,
                             std::string* missing = nullptr);

    // The game's short name: the lowercased basename of its ".acgame" manifest
    // (e.g. "tekken5"), resolving a directory selection to the manifest inside
    // it. Empty when the selection has no manifest. This is what lets ANY
    // collection title be identified as a System 246/256 game without a
    // built-in enum entry -- identify_set only knows the curated sets, but the
    // board boots any manifest, so routing keys off this instead.
    static std::string acgame_short_name(const std::string& path);
};
