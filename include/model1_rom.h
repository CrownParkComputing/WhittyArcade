// Sega Model 1 ROM loading and board-native memory images.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class model1_rom_set : uint8_t {
    unknown,
    virtua_formula,
    virtua_fighter,
    star_wars_arcade,
    wing_war,
};

struct model1_roms {
    // V60's 24-bit address image. Unpopulated sockets/address ranges read as
    // 0xff; program and ROM-O devices are placed at their hardware addresses.
    std::vector<uint8_t> main_cpu;

    // 68000 sound program and the two independent MultiPCM sample buses.
    std::vector<uint8_t> sound_cpu;
    std::vector<uint8_t> samples_1;
    std::vector<uint8_t> samples_2;

    // Optional Sega Z80 Digital Sound Board. Star Wars Arcade uses the Z80
    // program to select Layer-II music streams from a separate ROM bus.
    std::vector<uint8_t> dsb_cpu;
    std::vector<uint8_t> dsb_mpeg;

    // Geometry board data, already interleaved in the byte order seen by the
    // Model 1 devices.
    std::vector<uint8_t> polygon_data;
    std::vector<uint8_t> copro_data;
    std::vector<uint8_t> lookup_tables;

    // Auxiliary boards. The I/O image and NVRAM are useful but are not needed
    // for the first direct-input implementation.
    std::vector<uint8_t> communication_cpu;
    std::vector<uint8_t> io_cpu;
    std::vector<uint8_t> default_nvram;

    // The archive contains the historical reconstructed TGP image. It runs on
    // the standalone MB86233 core and drives the real geometry-data ROM,
    // shared RAM, arithmetic lookup tables and V60 FIFOs.
    std::vector<uint8_t> legacy_tgp;

    bool complete() const;
};

struct model1_rom_load_result {
    model1_rom_set set{model1_rom_set::unknown};
    model1_roms roms;
    std::string error;

    explicit operator bool() const { return error.empty() && roms.complete(); }
};

class model1_rom_loader {
public:
    // Both ZIP archives and extracted directories are accepted. ZIP entries
    // may be nested; matching is case-insensitive and by basename.
    static model1_rom_set identify_set(const std::string& path);
    static const char* set_short_name(model1_rom_set set);
    static const char* set_display_name(model1_rom_set set);
    static model1_rom_load_result load(const std::string& path);
};
