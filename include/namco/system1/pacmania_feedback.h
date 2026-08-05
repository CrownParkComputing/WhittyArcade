// pacmania_feedback.h - semantic Pac-Mania events from emulated game state.
#pragma once

#include <array>
#include <cstdint>

namespace namco {

enum class pacmania_feedback_event : std::uint8_t {
    none,
    pellet,
    power_pellet,
    ghost_eaten,
    death,
};

struct pacmania_feedback_state {
    std::uint8_t lives{};
    std::uint8_t pellet_count{};
    std::uint8_t frightened_timer{};
    std::array<std::uint8_t, 4> score_signature{};
};

struct pacmania_feedback_signal {
    pacmania_feedback_event event{pacmania_feedback_event::none};
    // Ghost captures use 1..4 so successive ghosts can feel progressively
    // larger. Other events leave this at one.
    std::uint8_t level{1};
};

class pacmania_feedback_tracker {
public:
    pacmania_feedback_signal update(
        const pacmania_feedback_state& state) noexcept;
    void reset() noexcept;

private:
    pacmania_feedback_state m_previous{};
    std::uint8_t m_ghost_chain{};
    bool m_initialized{};
};

} // namespace namco
