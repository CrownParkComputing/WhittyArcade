// Runtime contract between the application shell and an emulated board.
#pragma once

#include "arcade_settings.h"
#include "arcade_types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class arcade_video_worker;

// Cabinet switches survive a ROM change, while every machine, CPU, audio
// device and RAM allocation is recreated by the session factory.
struct arcade_cabinet_state {
    uint16_t system22_dip_switches{0xffff};
    // SW1: 1C/1C. SW2: upright, demo sound on, 3 lives, normal difficulty,
    // slow bullets and English.
    uint16_t shinobi_dip_switches{0x7cff};
};

class emulator_session {
public:
    virtual ~emulator_session() = default;

    virtual arcade_board_type board_type() const noexcept = 0;
    virtual bool initialize(const std::string& rom_path,
                            const std::string& bios_path,
                            const emulator_settings& settings) = 0;
    virtual void run_frame() = 0;

    virtual arcade_host_action process_events() = 0;
    virtual void set_rom_choices(const std::vector<rom_choice>& choices) = 0;
    virtual bool take_rom_selection(std::string& path) = 0;
    virtual bool take_operator_settings_request() = 0;
    virtual bool take_controls_request() = 0;
    virtual void open_operator_settings() = 0;
    virtual void reload_input_mappings() = 0;
    virtual bool take_settings_change(emulator_settings& settings) = 0;
    virtual bool paused() const = 0;
    virtual void set_paused(bool paused) = 0;
    virtual void refresh_output() = 0;
    virtual double frame_seconds() const = 0;
};

// The sole board-to-runtime registration point. A new hardware platform adds
// one catalog entry and one factory branch; main() remains board-neutral.
std::unique_ptr<emulator_session> create_emulator_session(
    arcade_board_type board,
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet_state);
