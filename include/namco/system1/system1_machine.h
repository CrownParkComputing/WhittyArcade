#pragma once

#include "arcade_types.h"
#include "namco/namco_rom.h"
#include "namco/system1/pacmania_feedback.h"

#include <cstdint>
#include <memory>

namespace namco {

class system1_machine {
public:
    static constexpr int width = 224;
    static constexpr int height = 288;
    static constexpr double refresh_hz = 60.606;

    system1_machine();
    ~system1_machine();
    system1_machine(const system1_machine&) = delete;
    system1_machine& operator=(const system1_machine&) = delete;

    bool initialize(const pacmania_roms& roms);
    bool initialize(const galaga88_roms& roms);
    void reset();
    void set_input(const input_state& input, uint8_t dips);
    void run_frame();
    const uint32_t* framebuffer() const;
    uint16_t program_counter() const;
    int fault() const;
    pacmania_feedback_signal take_feedback_signal() noexcept;

private:
    struct impl;
    std::unique_ptr<impl> m_impl;
};

} // namespace namco
