#include "arcade_feedback.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace arcade_feedback {
namespace {

std::atomic<unsigned> pending_impact{0};
std::atomic<unsigned> pending_gameplay_event{0};

bool is_one_of(std::string_view name,
               std::initializer_list<std::string_view> names) noexcept {
    for (std::string_view candidate : names)
        if (name == candidate) return true;
    return false;
}

} // namespace

void publish_impact(float strength) noexcept {
    const unsigned encoded = static_cast<unsigned>(
        std::clamp(strength, 0.0f, 1.0f) * 65535.0f);
    unsigned current = pending_impact.load(std::memory_order_relaxed);
    while (current < encoded &&
           !pending_impact.compare_exchange_weak(
               current, encoded, std::memory_order_release,
               std::memory_order_relaxed)) {}
}

float take_impact() noexcept {
    return static_cast<float>(
               pending_impact.exchange(0, std::memory_order_acquire)) /
           65535.0f;
}

void publish_gameplay_event(gameplay_event_kind kind,
                            std::uint8_t level) noexcept {
    if (kind == gameplay_event_kind::none) return;
    const unsigned encoded =
        (static_cast<unsigned>(kind) << 8) | std::max<unsigned>(1, level);
    unsigned current = pending_gameplay_event.load(std::memory_order_relaxed);
    while (current < encoded &&
           !pending_gameplay_event.compare_exchange_weak(
               current, encoded, std::memory_order_release,
               std::memory_order_relaxed)) {}
}

gameplay_event take_gameplay_event() noexcept {
    const unsigned encoded = pending_gameplay_event.exchange(
        0, std::memory_order_acquire);
    return {static_cast<gameplay_event_kind>((encoded >> 8) & 0xff),
            static_cast<std::uint8_t>(encoded & 0xff)};
}

command gameplay_event_command(gameplay_event event) noexcept {
    switch (event.kind) {
    case gameplay_event_kind::pellet:
        return {false, 0, 0, 0x1000, 0x2c00, 32};
    case gameplay_event_kind::power_pellet:
        return {false, 0, 0, 0x5000, 0x7800, 100};
    case gameplay_event_kind::ghost_eaten: {
        const unsigned chain = std::clamp<unsigned>(event.level, 1, 4) - 1;
        return {false, 0, 0,
                static_cast<std::uint16_t>(0x6800 + chain * 0x1800),
                static_cast<std::uint16_t>(0xa000 + chain * 0x1400),
                static_cast<std::uint16_t>(120 + chain * 20)};
    }
    case gameplay_event_kind::death:
        return {false, 0, 0, 0xffff, 0xb000, 260};
    case gameplay_event_kind::none:
        return {};
    }
    return {};
}

bool profile::accepts_generic_impact() const noexcept {
    // Pac-Mania has precise RAM-derived events. Mixing in broad audio
    // transient guesses would swamp its deliberately light pellet taps.
    return m_kind != kind::jumping_maze;
}

void profile::configure(std::string_view name) noexcept {
    reset();
    if (is_one_of(name, {"galaxian", "mooncrst", "uniwars", "galaga",
                         "phoenix"})) {
        m_kind = kind::fixed_shooter;
    } else if (name == "pacmania") {
        m_kind = kind::jumping_maze;
    } else if (name == "shinobi4") {
        m_kind = kind::ninja_action;
    } else if (name == "gng") {
        m_kind = kind::two_button_action;
    } else if (is_one_of(name, {"aliensyn", "aurail", "riotcity",
                                "goldnaxe2", "altbeast", "ddux1",
                                "tturfu"})) {
        m_kind = kind::three_button_action;
    } else if (is_one_of(name, {"vcop", "vcop2", "timecrisis3",
                                "timecrisis4", "vampirenight", "cobra"})) {
        m_kind = kind::lightgun;
    } else if (is_one_of(name, {"srallyc", "daytona", "manxttc",
                                "motoraid", "rrvac", "motogp",
                                "raverace", "acedrive", "victlap",
                                "dirtdash", "aquajet"}) ||
               name.find("ridge") != std::string_view::npos ||
               name.find("moto") != std::string_view::npos) {
        m_kind = kind::driving;
    } else if (name == "vf2" ||
               name.find("tekken") != std::string_view::npos ||
               name.find("soulcal") != std::string_view::npos) {
        m_kind = kind::fighting;
    } else if (name == "cybrcomm") {
        m_kind = kind::twin_weapon;
    }
}

void profile::reset() noexcept {
    m_kind = kind::none;
    m_previous_buttons.fill(false);
    m_continuous_active = false;
    m_last_rev_gas = 0;
    m_driving_pulse_cooldown = 0;
    m_jump_landing_frames = 0;
    m_previous_shift = false;
}

command profile::update(const input_state& state, bool suppressed) noexcept {
    command result;
    const auto rising = [&](std::size_t button) {
        return state.buttons[button] && !m_previous_buttons[button];
    };

    if (m_kind == kind::driving) {
        // Driving pads are event-only. A rising throttle/gear change gives a
        // short rev kick, and a powered or braking high-angle turn gives
        // spaced tyre-scrub pulses. Holding any control can never hold a
        // motor on; board/audio collision events arrive on the impact bus.
        result.set_continuous = true;
        m_continuous_active = true;
        if (m_driving_pulse_cooldown != 0) --m_driving_pulse_cooldown;
        if (!suppressed) {
            const bool shifted = state.shift_up || state.shift_down;
            if (state.gas + 0x60u < m_last_rev_gas)
                m_last_rev_gas = state.gas;
            const bool next_rev_band =
                state.gas > m_last_rev_gas + 0x70u;
            const unsigned steering = static_cast<unsigned>(std::abs(
                static_cast<int>(state.steering) - 0x800));
            const bool sliding = steering > 0x220 &&
                (state.gas > 0x180 || state.brake > 0x90);
            if ((next_rev_band || (shifted && !m_previous_shift)) &&
                m_driving_pulse_cooldown == 0) {
                const unsigned strength = std::min<unsigned>(
                    0x7800, 0x3800 +
                    std::min<unsigned>(state.gas, 0x610) *
                        0x4000u / 0x610u);
                result.pulse_low = static_cast<std::uint16_t>(strength);
                result.pulse_high = static_cast<std::uint16_t>(
                    std::min<unsigned>(0x4000, strength / 2));
                result.pulse_ms = 90;
                m_driving_pulse_cooldown = 5;
                if (next_rev_band)
                    m_last_rev_gas = static_cast<std::uint16_t>(
                        std::min<unsigned>(state.gas,
                            m_last_rev_gas + 0x100u));
            } else if (sliding && m_driving_pulse_cooldown == 0) {
                result.pulse_low = 0x6800;
                result.pulse_high = 0xd000;
                result.pulse_ms = 100;
                m_driving_pulse_cooldown = 6;
            }
            m_previous_shift = shifted;
        } else {
            m_last_rev_gas = 0;
        }
    } else if (suppressed && m_continuous_active) {
        result.set_continuous = true;
        m_continuous_active = false;
    }

    if (!suppressed && result.pulse_ms == 0) {
        switch (m_kind) {
        case kind::fixed_shooter:
            if (rising(0)) result = {false, 0, 0, 0x1800, 0x7800, 45};
            break;
        case kind::jumping_maze:
            if (rising(0)) {
                // Crisp high-motor take-off, followed by a heavier landing.
                // Pac-Mania's jump arc is fixed-duration; scheduling it here
                // keeps synthetic game feel in the shared profile service.
                result = {false, 0, 0, 0x3000, 0xb000, 90};
                m_jump_landing_frames = 38;
            } else if (m_jump_landing_frames != 0 &&
                       --m_jump_landing_frames == 0) {
                result = {false, 0, 0, 0x8800, 0x5000, 110};
            }
            break;
        case kind::ninja_action:
            if (rising(2)) result = {false, 0, 0, 0xb000, 0xffff, 180};
            else if (rising(1)) result = {false, 0, 0, 0x4800, 0x3800, 65};
            else if (rising(0)) result = {false, 0, 0, 0x2000, 0x7000, 45};
            break;
        case kind::two_button_action:
            if (rising(1)) result = {false, 0, 0, 0x4800, 0x3000, 65};
            else if (rising(0)) result = {false, 0, 0, 0x2000, 0x7000, 45};
            break;
        case kind::three_button_action:
            if (rising(1)) result = {false, 0, 0, 0x4800, 0x3800, 65};
            else if (rising(2)) result = {false, 0, 0, 0x8000, 0xa000, 90};
            else if (rising(0)) result = {false, 0, 0, 0x2800, 0x7000, 45};
            break;
        case kind::lightgun:
            if (rising(0)) result = {false, 0, 0, 0x5000, 0xffff, 95};
            break;
        case kind::fighting:
            for (std::size_t button = 0; button < 6; ++button) {
                if (!rising(button)) continue;
                const bool heavy = button >= 2;
                result = {false, 0, 0,
                          static_cast<std::uint16_t>(heavy ? 0x7800 : 0x3000),
                          static_cast<std::uint16_t>(heavy ? 0x9000 : 0x6800),
                          static_cast<std::uint16_t>(heavy ? 70 : 45)};
                break;
            }
            break;
        case kind::twin_weapon:
            if ((state.shift_down && !m_previous_buttons[0]) ||
                (state.shift_up && !m_previous_buttons[1]))
                result = {false, 0, 0, 0x4800, 0xe800, 85};
            break;
        case kind::driving:
        case kind::none:
            break;
        }
    }

    if (suppressed && m_kind == kind::jumping_maze)
        m_jump_landing_frames = 0;

    for (std::size_t i = 0; i < m_previous_buttons.size(); ++i)
        m_previous_buttons[i] = state.buttons[i];
    if (m_kind == kind::twin_weapon) {
        m_previous_buttons[0] = state.shift_down;
        m_previous_buttons[1] = state.shift_up;
    }
    return result;
}

} // namespace arcade_feedback
