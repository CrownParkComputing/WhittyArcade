// Sega Model 2 ROM loading and board-native memory images.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class model2_rom_set : uint8_t {
    unknown,
    sega_rally_revision_c,
    virtua_cop_2,
    virtua_cop,
    daytona,
    virtua_fighter_2,
    manx_tt,
    motor_raid,
};

// Declarative per-game hardware configuration. The Model 2 board hardware
// modules (bus, session) read this profile instead of switching on the rom
// set, so game identity is resolved once (model2_rom_loader::profile_for) at
// the composition root rather than leaking into every device.
struct model2_game_profile {
    // Which sound board the cabinet carries. Model 2A-CRX uses the on-board
    // SCSP; the original Model 2 (Virtua Cop) drives the Sega Model 1 sound
    // board (segam1audio: 68000 + YM3438 + dual MultiPCM).
    enum class sound_board { scsp, segam1audio } sound{sound_board::scsp};

    // How the cabinet I/O and controls are wired at 0x01c00000. Wheel/pedal
    // cabinets and 315-5296 light-gun cabinets both read the 315-5296 chip;
    // the original Model 2 cabinets read an I/O board over dual-port RAM
    // instead - Virtua Cop the model1io2 (TMPZ84C015), Daytona the earlier
    // model1io (315-5338A) driving board shared with Model 1 racers.
    enum class io_kind {
        crx_wheel,
        crx_gun,
        // 315-5296 with two digital fighter pads (Virtua Fighter 2):
        // punch/kick/guard and an 8-way stick per player on IN1/IN2.
        crx_fighter,
        // 315-5296 motorbike cabinet (Manx TT): throttle/brake/bank on
        // the ADC, shift up/down buttons on IN1.
        crx_bike,
        model1io2_dpram,
        model1io_dpram,
    } io{io_kind::crx_wheel};

    // Show the on-screen light-gun sight in place of the desktop pointer.
    bool lightgun{false};

    // Per-game NVRAM subdirectory so saves never collide between games.
    const char* nvram_leaf{"srallyc"};

    // Original Model 2 (Daytona, Virtua Cop) maps 0x00220000-0x0023ffff as a
    // window onto the program ROM at offset 0x20000; Model 2A has RAM there.
    bool original_model2{false};
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

    // Original Model 2 games (Virtua Cop, Daytona, ...) drive the Sega Model 1
    // sound board (segam1audio: 68000 + YM3438 + two MultiPCM buses) instead of
    // the Model 2A SCSP board. sound_cpu then holds the 68000 program and these
    // two vectors carry the independent MultiPCM sample buses. They stay empty
    // for the SCSP sets, which populate `samples` instead.
    std::vector<uint8_t> multipcm_samples_1;
    std::vector<uint8_t> multipcm_samples_2;

    // Cabinet I/O microcontroller firmware. Virtua Cop reads its guns and
    // digital inputs through a Sega model1io2 board (TMPZ84C015 Z80 running
    // this program) shared with the i960 over dual-port RAM; the Model 2A sets
    // read the 315-5296 chip directly and leave this empty.
    std::vector<uint8_t> io_cpu;

    // Factory 93C46 image shipped with the set (Manx TT's Twin-mode
    // configuration). Used when no saved EEPROM exists.
    std::vector<uint8_t> default_eeprom;

    // The set that produced this image. Required regions differ per game;
    // complete() uses it to assert sizes without making each board
    // (Model 2A, Model 2B, Model 2C-CRX) carry its own struct.
    model2_rom_set set{model2_rom_set::unknown};

    // The billboard is an external cabinet display and is not required to
    // boot the main Model 2 game board. complete() enforces per-set
    // expectations for the in-cabinet regions.
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
    // The declarative hardware configuration for a set. This is the single
    // place game identity maps to Model 2 board wiring.
    static model2_game_profile profile_for(model2_rom_set set);
    static model2_rom_load_result load(const std::string& path);
    // Per-set expected size, used to decide when a loaded image has
    // produced all the regions the machine can boot from. The set
    // decides which optional regions (drive CPU, M2COMM comm board,
    // billboard) are required; vcop2 has none of those, so the check is
    // a per-set table rather than a hard-coded "all sizes match".
    static bool is_complete(model2_rom_set set, const model2_roms& roms);
};
