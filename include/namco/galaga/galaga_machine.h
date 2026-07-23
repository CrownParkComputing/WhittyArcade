#pragma once

#include "arcade_types.h"
#include "namco/namco_rom.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace namco {

class galaga_machine {
public:
    static constexpr int width = 224;
    static constexpr int height = 288;
    static constexpr double refresh_hz = 60.606;

    galaga_machine();
    ~galaga_machine();
    galaga_machine(const galaga_machine&) = delete;
    galaga_machine& operator=(const galaga_machine&) = delete;

    bool initialize(const galaga_roms& roms);
    void reset();
    void configure_network_two_player(bool enabled);
    void set_input(const input_state& input);
    void set_sound_write_handler(
        std::function<void(unsigned, uint8_t)> handler);
    void run_frame();

    const uint32_t* framebuffer() const;
    uint16_t program_counter() const;
    int active_player() const;
    int credit_count() const;
    // 0 while waiting for a start, otherwise the start line accepted by the
    // emulated Namco 51xx credit I/O (1 PLAY or 2 PLAYERS).
    int selected_players() const;

private:
    struct impl;
    std::unique_ptr<impl> m_impl;
};

} // namespace namco
