#include "namco/system1/system1_machine.h"

extern "C" {
#include "namcos1_machine.h"
#include "namcos1_render.h"
}

#include <algorithm>
#include <array>
#include <vector>

namespace namco {

struct system1_machine::impl {
    std::array<uint32_t, width * height> frame{};
    std::array<uint32_t, width * height> source{};
    bool loaded{};
    // The feedback tracker reads fixed Pac-Mania work-RAM addresses, which
    // mean nothing in another game's memory map, so it only runs for the
    // game it was written for.
    bool pacmania_feedback{};
    pacmania_feedback_tracker feedback_tracker{};
    pacmania_feedback_signal feedback_signal{};
};

system1_machine::system1_machine() : m_impl(std::make_unique<impl>()) {}
system1_machine::~system1_machine() = default;

bool system1_machine::initialize(const pacmania_roms& roms) {
    if (!roms.valid) return false;
    g88_load_pacmania(
        roms.program6.data(), roms.program7.data(),
        roms.sound0.data(), roms.sound1.data(), roms.mcu.data(),
        roms.voice.data(), roms.chars[0].data(), roms.chars[1].data(),
        roms.chars[2].data(), roms.chars[3].data(), roms.mask.data(),
        roms.sprite0.data(), roms.sprite1.data());
    m_impl->loaded = true;
    m_impl->pacmania_feedback = true;
    reset();
    return true;
}

bool system1_machine::initialize(const galaga88_roms& roms) {
    if (!roms.valid) return false;
    g88_load(
        roms.program0.data(), roms.program1.data(), roms.program5.data(),
        roms.program6.data(), roms.program7.data(),
        roms.sound0.data(), roms.sound1.data(), roms.mcu.data(),
        roms.voices[0].data(), roms.voices[1].data(), roms.voices[2].data(),
        roms.voices[3].data(), roms.voices[4].data(), roms.voices[5].data(),
        roms.chars[0].data(), roms.chars[1].data(), roms.chars[2].data(),
        roms.chars[3].data(), roms.mask.data(),
        roms.sprites[0].data(), roms.sprites[1].data(),
        roms.sprites[2].data(), roms.sprites[3].data(),
        roms.sprites[4].data(), roms.sprites[5].data());
    m_impl->loaded = true;
    m_impl->pacmania_feedback = false;
    reset();
    return true;
}

void system1_machine::reset() {
    if (m_impl->loaded) g88_reset();
    m_impl->feedback_tracker.reset();
    m_impl->feedback_signal = {};
}

void system1_machine::set_input(const input_state& input, uint8_t dips) {
    uint8_t p1 = 0xff, p2 = 0xff, coin = 0xff;
    const auto clear = [](uint8_t& port, unsigned bit, bool pressed) {
        if (pressed) port &= static_cast<uint8_t>(~(1u << bit));
    };
    clear(p1, 0, input.left_stick_x > 0x90);
    clear(p1, 1, input.left_stick_x < 0x60);
    clear(p1, 2, input.left_stick_y > 0x90);
    clear(p1, 3, input.left_stick_y < 0x60);
    clear(p1, 4, input.buttons[0]);
    clear(p1, 5, input.buttons[1]);
    clear(p1, 6, input.buttons[2]);
    clear(p1, 7, input.start);
    clear(p2, 0, input.p2_stick_x > 0x90);
    clear(p2, 1, input.p2_stick_x < 0x60);
    clear(p2, 2, input.p2_stick_y > 0x90);
    clear(p2, 3, input.p2_stick_y < 0x60);
    clear(p2, 4, input.p2_buttons[0]);
    clear(p2, 5, input.p2_buttons[1]);
    clear(p2, 6, input.p2_buttons[2]);
    clear(p2, 7, input.p2_start);
    clear(coin, 3, input.coin2);
    clear(coin, 4, input.coin1);
    clear(coin, 5, input.service);
    clear(coin, 6, input.test);
    g88_set_p1(p1);
    g88_set_p2(p2);
    g88_set_coin(coin);
    g88_set_dipsw(dips);
}

void system1_machine::run_frame() {
    if (!m_impl->loaded) return;
    g88_run_frame();
    if (m_impl->pacmania_feedback) {
        const pacmania_feedback_state feedback_state{
            g88_peek_main(0x0621),
            g88_peek_main(0x0074),
            g88_peek_main(0x0059),
            {g88_peek_main(0x0614), g88_peek_main(0x0615),
             g88_peek_main(0x0616), g88_peek_main(0x0617)}};
        const pacmania_feedback_signal signal =
            m_impl->feedback_tracker.update(feedback_state);
        if (signal.event != pacmania_feedback_event::none)
            m_impl->feedback_signal = signal;
    }
    g88_render(m_impl->source.data());
    for (std::size_t i = 0; i < m_impl->frame.size(); ++i) {
        const uint32_t rgb = m_impl->source[i];
        m_impl->frame[i] = ((rgb >> 16) & 0xff) |
            (rgb & 0x00ff00) | ((rgb & 0xff) << 16) | 0xff000000u;
    }
}

const uint32_t* system1_machine::framebuffer() const {
    return m_impl->frame.data();
}

uint16_t system1_machine::program_counter() const {
    return static_cast<uint16_t>(g88_pc(0));
}

int system1_machine::fault() const {
    return g88_fault() ? g88_fault() : g88_mcu_trap();
}

pacmania_feedback_signal system1_machine::take_feedback_signal() noexcept {
    const pacmania_feedback_signal signal = m_impl->feedback_signal;
    m_impl->feedback_signal = {};
    return signal;
}

} // namespace namco
