#pragma once

#include "gng_rom.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace gng {

class machine {
public:
    static constexpr int width = 256;
    static constexpr int height = 224;
    static constexpr uint32_t cycles_per_frame = 100608;
    static constexpr double refresh_hz = 6000000.0 / cycles_per_frame;

    machine();
    ~machine();
    machine(const machine&) = delete;
    machine& operator=(const machine&) = delete;

    bool initialize(const roms& rom_data);
    void reset();
    void set_inputs(uint8_t system, uint8_t player1, uint8_t player2,
                    uint8_t dsw1, uint8_t dsw2);
    void set_sound_callbacks(std::function<void(uint8_t)> command,
                             std::function<void(bool)> reset_line);
    void run_frame();

    const std::vector<uint32_t>& framebuffer() const noexcept;
    uint16_t program_counter() const noexcept;
    uint8_t stopped() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> m;
};

} // namespace gng
