#include "system246_controls.h"

#include <algorithm>
#include <cmath>

namespace {

uint8_t scale_axis(uint16_t value, uint16_t minimum, uint16_t maximum,
                   uint8_t output_maximum) {
    const uint16_t clamped = std::clamp(value, minimum, maximum);
    const double normalized = static_cast<double>(clamped - minimum) /
                              static_cast<double>(maximum - minimum);
    return static_cast<uint8_t>(std::lround(
        normalized * static_cast<double>(output_maximum)));
}

} // namespace

system246_drive_controls translate_system246_controls(
        const input_state& input) noexcept {
    system246_drive_controls result;
    // Whitty's cabinet ADC range deliberately leaves safety margins around
    // its 12-bit endpoints. Rescale that useful range to Play!'s full byte.
    result.wheel = scale_axis(input.steering, 0x280, 0xd80, 0xff);
    const uint8_t gas = scale_axis(input.gas, 0, 0x610, 0x7f);
    const uint8_t brake = scale_axis(input.brake, 0, 0x610, 0x7f);
    // Play!'s System 246 drive HLE expects gas on the positive half of left Y
    // (127 at rest, 0 down) and brake on the positive half of right X.
    result.gas_axis = static_cast<uint8_t>(0x7f - gas);
    result.brake_axis = static_cast<uint8_t>(0x80 + brake);
    // Coins are counters on System 246, while service is a normal JVS switch.
    // Keeping them separate is important for Ridge Racer V's FCA-1 board.
    result.coin = input.coin1;
    result.service = input.service;
    result.start = input.start;
    result.test = input.test;
    result.shift_down = input.shift_down;
    result.shift_up = input.shift_up;
    result.view1 = input.view;
    result.view2 = input.view2;
    result.view3 = input.view3;
    result.view4 = input.view4;
    result.menu_up = input.left_stick_y < 0x7f;
    result.menu_down = input.left_stick_y > 0x7f;
    return result;
}
