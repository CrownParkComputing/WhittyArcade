// arcade_feedback.h - board-neutral force-feedback synthesis and event bus.
#pragma once

#include "arcade_types.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace arcade_feedback {

// Audio backends and native game plugins publish consequence events here.
// The controller adapter consumes the strongest pending event once per frame.
void publish_impact(float strength) noexcept;
float take_impact() noexcept;

enum class gameplay_event_kind : std::uint8_t {
    none,
    pellet,
    power_pellet,
    ghost_eaten,
    death,
};

struct gameplay_event {
    gameplay_event_kind kind{gameplay_event_kind::none};
    std::uint8_t level{1};
};

// Emulators publish semantic events rather than controller motor values. The
// shared layer owns the tactile tuning, just as it does for input profiles.
void publish_gameplay_event(gameplay_event_kind kind,
                            std::uint8_t level = 1) noexcept;
gameplay_event take_gameplay_event() noexcept;

struct command {
    bool set_continuous{false};
    std::uint16_t continuous_low{0};
    std::uint16_t continuous_high{0};
    std::uint16_t pulse_low{0};
    std::uint16_t pulse_high{0};
    std::uint16_t pulse_ms{0};
};

command gameplay_event_command(gameplay_event event) noexcept;

// Converts ordinary cabinet controls into tactile cues for games without a
// native feedback output. Profiles are kept here so input adapters and board
// emulators never contain game-name lists or effect tuning.
class profile {
public:
    void configure(std::string_view game_short_name) noexcept;
    command update(const input_state& state, bool suppressed) noexcept;
    bool accepts_generic_impact() const noexcept;
    void reset() noexcept;

private:
    enum class kind : std::uint8_t {
        none,
        fixed_shooter,
        jumping_maze,
        ninja_action,
        two_button_action,
        three_button_action,
        lightgun,
        driving,
        fighting,
        twin_weapon,
    };

    kind m_kind{kind::none};
    std::array<bool, 8> m_previous_buttons{};
    bool m_continuous_active{false};
    std::uint16_t m_last_rev_gas{0};
    std::uint8_t m_driving_pulse_cooldown{0};
    std::uint8_t m_jump_landing_frames{0};
    bool m_previous_shift{false};
};

} // namespace arcade_feedback
