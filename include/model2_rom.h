// Sega Model 2 ROM loading and board-native memory images.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class model2_rom_set : uint8_t {
    unknown,
    sega_rally_revision_c,
};

struct model2_roms {
    // Intel i960KB program and the large CPU-board data bus.
    std::vector<uint8_t> main_cpu;
    std::vector<uint8_t> main_data;

    // MB86234 geometry input, model data, textures and fixed-point tables.
    std::vector<uint8_t> copro_data;
    std::vector<uint8_t> polygon_data;
    std::vector<uint8_t> texture_data;
    std::vector<uint8_t> copro_tgp_tables;
    std::vector<uint8_t> other_data;
    std::vector<uint8_t> video_tables;

    // 68000 + SCSP sound board and auxiliary Z80 boards.
    std::vector<uint8_t> sound_cpu;
    std::vector<uint8_t> samples;
    std::vector<uint8_t> communication_cpu;
    std::vector<uint8_t> drive_cpu;
    std::vector<uint8_t> billboard_cpu;

    // The billboard is an external cabinet display and is not required to
    // boot the main Model 2 game board.
    bool complete() const;
    bool has_billboard() const { return billboard_cpu.size() == 0x10000; }
};

struct model2_rom_load_result {
    model2_rom_set set{model2_rom_set::unknown};
    model2_roms roms;
    std::string error;

    explicit operator bool() const { return error.empty() && roms.complete(); }
};

class model2_rom_loader {
public:
    // ZIP entries may be nested and are matched case-insensitively by
    // basename. segabill.zip is located beside the selected game archive.
    static model2_rom_set identify_set(const std::string& path);
    static const char* set_short_name(model2_rom_set set);
    static const char* set_display_name(model2_rom_set set);
    static model2_rom_load_result load(const std::string& path);
};
