#pragma once

#include "arcade_types.h"
#include "namco_rom.h"

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
    void set_input(const input_state& input);
    void set_sound_write_handler(
        std::function<void(unsigned, uint8_t)> handler);
    void run_frame();

    const uint32_t* framebuffer() const;
    uint16_t program_counter() const;

private:
    struct impl;
    std::unique_ptr<impl> m_impl;
};

} // namespace namco
