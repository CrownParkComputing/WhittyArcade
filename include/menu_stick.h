// menu_stick.h - turning an analogue stick into discrete menu movement.
#pragma once

#include <cstdlib>

// An analogue stick reports where it is, not that it moved. A menu needs
// discrete steps, so a stick has to be latched: it steps once when it crosses
// the threshold and cannot step again until it comes back towards centre.
// Without that a menu runs away the instant the stick is nudged - which is
// why these menus worked with a d-pad and not with a stick.
//
// The release point sits well inside the engage point so a stick resting
// slightly off centre - which most worn arcade sticks and pads do - cannot
// chatter between stepping and not stepping.
class menu_stick {
public:
    // SDL's axis range is +/-32767. Engage needs a deliberate push; release
    // needs most of the way back.
    static constexpr int engage = 18000;
    static constexpr int release = 8000;

    // The step this axis position asks for: -1, 0 or +1. Pass each axis's
    // value as it changes; axis selects which of the two the caller is
    // reporting, so one latch tracks a whole stick.
    enum axis { horizontal, vertical };

    int step(axis which, int value) {
        int& held = which == horizontal ? m_horizontal : m_vertical;
        bool& armed = which == horizontal ? m_horizontal_armed
                                          : m_vertical_armed;
        const int direction =
            value >= engage ? 1 : (value <= -engage ? -1 : 0);
        if (direction == 0) {
            if (std::abs(value) <= release) { held = 0; armed = true; }
            return 0;
        }
        // An axis that has never been near centre is not a stick somebody
        // is pushing - it is a pedal, a trigger or a wheel at rest, which
        // reads -32767 and stays there. Believing it means the menu moves
        // on its own for ever, and no amount of latching helps because the
        // axis never comes back. A real stick passes through centre on its
        // way to anywhere, so this costs a genuine push nothing.
        if (!armed) return 0;
        // Already stepped this way; the stick must return before it can step
        // again, or one push would scroll the whole list.
        if (held == direction) return 0;
        held = direction;
        return direction;
    }

private:
    int m_horizontal{};
    int m_vertical{};
    bool m_horizontal_armed{};
    bool m_vertical_armed{};
};
