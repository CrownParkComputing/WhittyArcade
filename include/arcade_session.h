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
    uint16_t system16b_dip_switches{0x7cff};
    // SW1 low, SW2 high: 1C/1C, demo sound on, upright, 3 lives, normal.
    uint16_t gng_dip_switches{0xfbdf};
    // Namco System 1: service, freeze, watchdog/diagnostic options all off.
    uint8_t system1_dip_switches{0xff};
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

    // True when the game opens and owns its own window, as a recompiled native
    // title does - it is a separate process with its own renderer, and
    // MANX only started it.
    //
    // The launcher's own window must get out of the way when that happens. Left
    // up, there are two windows on screen: the game draws in one while the
    // launcher holds the focus in the other, so the pad appears dead. The game
    // is running perfectly and reports the controller it opened - it simply is
    // not the window being typed at.
    virtual bool owns_its_own_window() const noexcept { return false; }
    virtual void set_rom_choices(const std::vector<rom_choice>& choices) = 0;
    virtual bool take_rom_selection(std::string& path) = 0;
    virtual bool take_operator_settings_request() = 0;
    virtual bool take_controls_request() = 0;
    // The player asked to switch games from inside a running cabinet; the
    // host answers with the full library browser.
    virtual bool take_game_picker_request() { return false; }
    virtual void open_operator_settings() = 0;
    virtual void reload_input_mappings() = 0;
    virtual bool take_settings_change(emulator_settings& settings) = 0;
    virtual bool paused() const = 0;
    virtual void set_paused(bool paused) = 0;
    virtual void refresh_output() = 0;
    // Polls the netplay link without running a frame, so a cabinet waiting
    // on its partner still notices when that partner goes away.
    virtual void pump_netplay_link() {}
    virtual double frame_seconds() const = 0;
    // 1/2 for an alternating game's active side, 0 when both players are
    // active, and -1 when this board does not expose a verified turn marker.
    virtual int active_player() const { return -1; }
    // True when run_frame waits for the emulated display producer. Such a
    // session must not also be throttled by main()'s independent host timer.
    virtual bool producer_paced() const { return false; }
};

// The sole board-to-runtime registration point. A new hardware platform adds
// one catalog entry and one factory branch; main() remains board-neutral.
std::unique_ptr<emulator_session> create_emulator_session(
    arcade_board_type board,
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet_state);
