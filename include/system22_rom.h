// system22_rom.h - ROM loading and management
#pragma once

#include "system22_types.h"
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

// ROM entry structure matching MAME format
struct rom_entry {
    std::string name;
    uint32_t offset;
    uint32_t length;
    uint32_t crc;
    std::array<uint8_t, 20> sha1;
};

// Ridge Racer ROM layout (from MAME namcos22.cpp)
struct ridge_racer_roms {
    // Main CPU program (2MB, 32-bit wide, 4 files)
    std::vector<uint8_t> maincpu_rom;

    // MCU sound data (512KB)
    std::vector<uint8_t> mcu_rom;

    // Texture data (16MB total)
    std::vector<uint8_t> texture_rom;  // 4 banks: rr1cg0-3.bin
    // Rave Racer populates its textile region from byte zero; Ridge Racer
    // populates the upper half of the same 16 MiB address space.
    std::size_t texture_region_offset{0x800000};
    std::size_t texture_bank_count{4};
    // Ridge Racer-era tilemaps derive bit 15 from attribute bit 0.  Rave
    // Racer stores its tile numbers directly and must not use that remap.
    bool texture_tile_high_bit_from_attr{true};

    // Texture tilemap (2.5MB)
    std::vector<uint8_t> tilemap_rom;

    // Point ROM (3D model data, 1.5MB)
    std::vector<uint8_t> point_rom;

    // C352 samples (8MB total)
    std::vector<uint8_t> c352_samples;

    // Gamma PROMs (palette lookup)
    std::vector<uint8_t> gamma_proms;
    std::vector<uint8_t> eeprom;

    // Internal firmware for the two Namco custom devices. C71 is a
    // TMS320C25 and C74 is an M37702; these images are separate from the
    // Ridge Racer set itself in MAME-compatible collections.
    std::vector<uint8_t> c71_firmware;
    std::vector<uint8_t> c74_firmware;

    bool has_complete_program() const { return maincpu_rom.size() == 0x200000; }
    bool has_c71_firmware() const { return c71_firmware.size() == 0x2000; }
    bool has_c74_firmware() const { return c74_firmware.size() == 0x4000; }
};

enum class ridge_racer_rom_set : uint8_t {
    unknown,
    ridge_racer,
    ridge_racer_full_scale,
    ridge_racer_2,
    rave_racer,
    ace_driver,
    victory_lap,
    cyber_commando,
};

// ROM loader class - supports ZIP files
class rom_loader {
public:
    // Load from directory
    static ridge_racer_roms load_ridge_racer(
        const std::string& path, const std::string& bios_path = {});

    // Identify a supported ROM board by the files it contains. This works for
    // both extracted directories and ZIP archives, including merged sets.
    static ridge_racer_rom_set identify_set(const std::string& path);
    static const char* set_short_name(ridge_racer_rom_set set);
    static const char* set_display_name(ridge_racer_rom_set set);
    static bool is_working_set(ridge_racer_rom_set set);

private:
    static void load_system_firmware(const std::string& path, ridge_racer_roms& roms);

};
