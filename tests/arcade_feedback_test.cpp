#include "arcade_feedback.h"

#include <cassert>
#include <cstdio>

int main() {
    input_state state{};
    arcade_feedback::profile feedback;

    feedback.configure("galaxian");
    assert(feedback.update(state, false).pulse_ms == 0);
    state.buttons[0] = true;
    const arcade_feedback::command shot = feedback.update(state, false);
    assert(shot.pulse_high > shot.pulse_low && shot.pulse_ms == 45);
    assert(feedback.update(state, false).pulse_ms == 0);

    feedback.configure("pacmania");
    state = {};
    state.buttons[0] = true;
    const arcade_feedback::command takeoff = feedback.update(state, false);
    assert(takeoff.pulse_ms == 90 && takeoff.pulse_high > takeoff.pulse_low);
    state.buttons[0] = false;
    arcade_feedback::command landing;
    for (int frame = 0; frame < 38; ++frame)
        landing = feedback.update(state, false);
    assert(landing.pulse_ms == 110 && landing.pulse_low > landing.pulse_high);

    feedback.configure("shinobi4");
    state = {};
    state.buttons[2] = true;
    const arcade_feedback::command magic = feedback.update(state, false);
    assert(magic.pulse_low == 0xb000 && magic.pulse_high == 0xffff);

    feedback.configure("rrvac");
    state = {};
    state.gas = 0x610;
    const arcade_feedback::command driving = feedback.update(state, false);
    assert(driving.set_continuous);
    assert(driving.continuous_low == 0 && driving.continuous_high == 0);
    assert(driving.pulse_ms == 90 && driving.pulse_low >= 0x7000);
    assert(feedback.update(state, false).pulse_ms == 0);

    feedback.configure("raverace");
    state = {};
    state.gas = 0x610;
    state.shift_up = true;
    const arcade_feedback::command revving = feedback.update(state, false);
    assert(revving.set_continuous);
    assert(revving.continuous_low == 0 && revving.continuous_high == 0);
    assert(revving.pulse_ms == 90);

    feedback.configure("daytona");
    state = {};
    state.steering = 0x580;
    state.brake = 0x200;
    const arcade_feedback::command sliding = feedback.update(state, false);
    assert(sliding.pulse_ms == 100);
    assert(sliding.pulse_high > sliding.pulse_low);

    arcade_feedback::publish_impact(0.35f);
    arcade_feedback::publish_impact(0.80f);
    assert(arcade_feedback::take_impact() > 0.79f);
    assert(arcade_feedback::take_impact() == 0.0f);

    arcade_feedback::publish_gameplay_event(
        arcade_feedback::gameplay_event_kind::pellet);
    arcade_feedback::publish_gameplay_event(
        arcade_feedback::gameplay_event_kind::death);
    const arcade_feedback::gameplay_event event =
        arcade_feedback::take_gameplay_event();
    assert(event.kind == arcade_feedback::gameplay_event_kind::death);
    const arcade_feedback::command death =
        arcade_feedback::gameplay_event_command(event);
    const arcade_feedback::command pellet =
        arcade_feedback::gameplay_event_command(
            {arcade_feedback::gameplay_event_kind::pellet, 1});
    assert(death.pulse_ms > pellet.pulse_ms);
    assert(death.pulse_low > pellet.pulse_low);

    const arcade_feedback::command ghost1 =
        arcade_feedback::gameplay_event_command(
            {arcade_feedback::gameplay_event_kind::ghost_eaten, 1});
    const arcade_feedback::command ghost4 =
        arcade_feedback::gameplay_event_command(
            {arcade_feedback::gameplay_event_kind::ghost_eaten, 4});
    assert(ghost4.pulse_low > ghost1.pulse_low);
    assert(ghost4.pulse_ms > ghost1.pulse_ms);

    std::puts("arcade feedback profiles: ok");
    return 0;
}
