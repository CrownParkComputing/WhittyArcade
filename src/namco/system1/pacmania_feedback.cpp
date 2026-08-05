#include "namco/system1/pacmania_feedback.h"

#include <algorithm>

namespace namco {

pacmania_feedback_signal pacmania_feedback_tracker::update(
        const pacmania_feedback_state& state) noexcept {
    if (!m_initialized) {
        m_previous = state;
        m_initialized = true;
        return {};
    }

    const bool died = m_previous.lives >= 1 && m_previous.lives <= 9 &&
        state.lives < m_previous.lives && state.lives <= 9;
    const bool pellet = state.pellet_count ==
        static_cast<std::uint8_t>(m_previous.pellet_count + 1);
    const unsigned timer_rise = state.frightened_timer >=
            m_previous.frightened_timer
        ? state.frightened_timer - m_previous.frightened_timer
        : 0;
    const bool power_pellet =
        (m_previous.frightened_timer == 0 && state.frightened_timer != 0) ||
        timer_rise >= 0x20;
    const bool score_changed =
        state.score_signature != m_previous.score_signature;
    const bool frightened = m_previous.frightened_timer != 0 ||
        state.frightened_timer != 0;
    const bool ghost_eaten = frightened && score_changed && !pellet &&
        !power_pellet;

    if (power_pellet || state.frightened_timer == 0)
        m_ghost_chain = 0;
    if (ghost_eaten)
        m_ghost_chain = static_cast<std::uint8_t>(
            std::min<unsigned>(4, m_ghost_chain + 1));

    m_previous = state;
    if (died) return {pacmania_feedback_event::death, 1};
    if (power_pellet)
        return {pacmania_feedback_event::power_pellet, 1};
    if (ghost_eaten)
        return {pacmania_feedback_event::ghost_eaten, m_ghost_chain};
    if (pellet) return {pacmania_feedback_event::pellet, 1};
    return {};
}

void pacmania_feedback_tracker::reset() noexcept {
    m_previous = {};
    m_ghost_chain = 0;
    m_initialized = false;
}

} // namespace namco
