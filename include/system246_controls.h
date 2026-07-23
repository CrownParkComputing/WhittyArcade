// Board-neutral to Namco System 246 driving-control translation.
#pragma once

#include "arcade_types.h"

#include <cstdint>

struct system246_drive_controls {
    uint8_t wheel{0x80};
    uint8_t gas_axis{0x7f};
    uint8_t brake_axis{0x80};
    bool coin{};
    bool service{};
    bool start{};
    bool test{};
    bool shift_down{};
    bool shift_up{};
    bool view1{};
    bool view2{};
    bool view3{};
    bool view4{};
    bool menu_up{};
    bool menu_down{};
};

system246_drive_controls translate_system246_controls(
    const input_state& input) noexcept;
