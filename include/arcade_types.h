// arcade_types.h - data shared by all emulated arcade boards
#pragma once

#include <cstdint>
#include <string>

enum class arcade_board_type : uint8_t {
    system22,
    system246,
    xbox360,
    xbox,
    model1,
    model2,
    phoenix,
    galaxian,
    system16b,
    capcom_gng,
    namco_galaga,
    namco_system1,
    // Not a machine: a game that ships as a plugin and is discovered beside its
    // data rather than compiled in.
    game_plugin,
};

// Host-window policy is explicit so closing the application cannot be
// confused with leaving a running cabinet and returning to the launcher.
enum class arcade_host_action : uint8_t {
    continue_running,
    return_to_menu,
    exit_application,
};

constexpr arcade_host_action classify_arcade_host_event(
    bool close_requested, bool escape_pressed, bool key_repeated) noexcept {
    if (close_requested) return arcade_host_action::exit_application;
    if (escape_pressed && !key_repeated)
        return arcade_host_action::return_to_menu;
    return arcade_host_action::continue_running;
}

struct rom_choice {
    std::string path;
    std::string label;
    arcade_board_type board{arcade_board_type::system22};
    // Who published the machine. Carried alongside the board because the
    // launcher browses by both, and the two do not always agree - Moon Cresta
    // is Nichibutsu on Namco's Galaxian hardware.
    std::string publisher;
};

struct input_state {
    // ---- Driving boards (System 22/246, Model 1, Model 2) ----
    uint16_t steering{0x800};
    uint16_t gas{0};
    uint16_t brake{0};
    uint8_t  left_stick_x{0x7f};
    uint8_t  left_stick_y{0x7f};
    uint8_t  right_stick_x{0x7f};
    uint8_t  right_stick_y{0x7f};
    bool     coin1{false};
    bool     coin2{false};
    bool     service{false};
    bool     test{false};
    bool     start{false};
    bool     shift_down{false};
    bool     shift_up{false};
    bool     view{false};
    bool     view2{false};
    bool     view3{false};
    bool     view4{false};

    // ---- Action boards (Shinobi, future System 16-B games) ----
    // Action boards use left_stick_x/y as a 4-direction joystick
    // (<0x60 = left/up, >0x90 = right/down, matching the action-board
    // threshold convention). buttons[] supplies the per-game action
    // buttons (Shinobi: fire / jump / throw); boards with fewer than
    // eight buttons leave the rest false.
    uint8_t  buttons[8]{};

    // P2 joystick + buttons for two-player cabinets (Moon Cresta
    // second stick). Action boards that are single-player leave this
    // zero/false.
    uint8_t  p2_stick_x{0x7f};
    uint8_t  p2_stick_y{0x7f};
    uint8_t  p2_buttons[8]{};
    bool     p2_start{false};
};

struct rgba_color {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};
    uint8_t a{0};

    constexpr explicit rgba_color(uint32_t rgba = 0)
        : r(static_cast<uint8_t>(rgba >> 24)),
          g(static_cast<uint8_t>(rgba >> 16)),
          b(static_cast<uint8_t>(rgba >> 8)),
          a(static_cast<uint8_t>(rgba)) {}
};
