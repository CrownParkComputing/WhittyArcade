#include "play_input.h"

#include "system246_controls.h"

#include "ControllerInfo.h"
#include "PadHandler.h"
#include "PadInterface.h"
#include "iop/namco_sys246/Iop_NamcoSys246.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

void system246_input_channel::set_state(const input_state& state) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state = state;
}

input_state system246_input_channel::state() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

void system246_input_channel::hold_test_for_frames(uint32_t frames) {
    uint32_t current = m_forced_test_frames.load(std::memory_order_relaxed);
    while (current < frames &&
           !m_forced_test_frames.compare_exchange_weak(
               current, frames, std::memory_order_release,
               std::memory_order_relaxed)) {}
}

bool system246_input_channel::take_forced_test_frame() {
    uint32_t current = m_forced_test_frames.load(std::memory_order_acquire);
    while (current != 0) {
        if (m_forced_test_frames.compare_exchange_weak(
                current, current - 1, std::memory_order_acq_rel,
                std::memory_order_acquire))
            return true;
    }
    return false;
}

namespace {

class system246_pad_handler final : public CPadHandler {
public:
    explicit system246_pad_handler(
            std::shared_ptr<system246_input_channel> channel)
        : m_channel(std::move(channel)) {}

    void Update(uint8* ram) override {
        input_state input = m_channel ? m_channel->state() : input_state{};
        if (m_channel && m_channel->take_forced_test_frame()) input.test = true;
        const system246_drive_controls controls =
            translate_system246_controls(input);
        const bool trace = [] {
            const char* value = std::getenv("WHITTYARCADE_TRACE");
            return value && *value && std::strcmp(value, "0") != 0;
        }();
        if (trace &&
            ((controls.coin && !m_last_coin) ||
             (controls.start && !m_last_start))) {
            std::printf("System 246 Play pad coin=%d start=%d listeners=%zu\n",
                        controls.coin, controls.start,
                        m_interfaces.size());
            std::fflush(stdout);
        }
        const bool insert_coin = controls.coin && !m_last_coin;
        m_last_coin = controls.coin;
        m_last_start = controls.start;

        for (CPadInterface* listener : m_interfaces) {
            if (!listener) continue;
            listener->SetAxisState(
                0, PS2::CControllerInfo::ANALOG_LEFT_X,
                controls.wheel, ram);
            listener->SetAxisState(
                0, PS2::CControllerInfo::ANALOG_LEFT_Y,
                controls.gas_axis, ram);
            listener->SetAxisState(
                0, PS2::CControllerInfo::ANALOG_RIGHT_X,
                controls.brake_axis, ram);

            set_button(listener, PS2::CControllerInfo::SELECT,
                       controls.service, ram);
            set_button(listener, PS2::CControllerInfo::START,
                       controls.start, ram);
            // Play!'s System 246 HLE recognises TEST when both JVS system
            // bits (L3 and R3) are held together.
            set_button(listener, PS2::CControllerInfo::L3,
                       controls.test, ram);
            set_button(listener, PS2::CControllerInfo::R3,
                       controls.test, ram);
            set_button(listener, PS2::CControllerInfo::L1,
                       controls.shift_down, ram);
            set_button(listener, PS2::CControllerInfo::R1,
                       controls.shift_up, ram);
            set_button(listener, PS2::CControllerInfo::SQUARE,
                       controls.view1, ram);
            set_button(listener, PS2::CControllerInfo::TRIANGLE,
                       controls.view2, ram);
            set_button(listener, PS2::CControllerInfo::CIRCLE,
                       controls.view3, ram);
            set_button(listener, PS2::CControllerInfo::CROSS,
                       controls.view4, ram);
            set_button(listener, PS2::CControllerInfo::DPAD_UP,
                       controls.menu_up, ram);
            set_button(listener, PS2::CControllerInfo::DPAD_DOWN,
                       controls.menu_down, ram);

            if (auto* system =
                    dynamic_cast<Iop::Namco::CSys246*>(listener)) {
                if (insert_coin) system->InsertCoin(0);
                system->UpdateRrvFcaFrame(ram);
            }
        }
    }

private:
    static void set_button(CPadInterface* listener,
                           PS2::CControllerInfo::BUTTON button,
                           bool pressed, uint8* ram) {
        listener->SetButtonState(0, button, pressed, ram);
    }

    std::shared_ptr<system246_input_channel> m_channel;
    bool m_last_coin{};
    bool m_last_start{};
};

} // namespace

CPadHandler* make_system246_pad_handler(
        std::shared_ptr<system246_input_channel> channel) {
    return new system246_pad_handler(std::move(channel));
}
