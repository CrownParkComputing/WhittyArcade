#pragma once

#include <cstdint>

namespace gng {

// Convert MANX's centered action-board axes into GnG's active-low
// right/left/down/up/button1/button2 port. The host scaler spans 0x47..0xb7,
// so the action-board thresholds are deliberately symmetric around 0x7f.
constexpr uint8_t encode_player_port(uint8_t x, uint8_t y,
                                     bool button1, bool button2) noexcept {
    uint8_t port = 0xff;
    if (x > 0x90) port &= static_cast<uint8_t>(~0x01); // right
    if (x < 0x60) port &= static_cast<uint8_t>(~0x02); // left
    if (y > 0x90) port &= static_cast<uint8_t>(~0x04); // down
    if (y < 0x60) port &= static_cast<uint8_t>(~0x08); // up
    if (button1) port &= static_cast<uint8_t>(~0x10);
    if (button2) port &= static_cast<uint8_t>(~0x20);
    return port;
}

} // namespace gng
