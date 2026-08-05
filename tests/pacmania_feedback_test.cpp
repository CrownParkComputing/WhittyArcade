#include "namco/system1/pacmania_feedback.h"

#include <cassert>
#include <cstdio>

int main() {
    namco::pacmania_feedback_tracker tracker;
    namco::pacmania_feedback_state state{};
    state.lives = 3;
    assert(tracker.update(state).event ==
           namco::pacmania_feedback_event::none);

    ++state.pellet_count;
    state.score_signature[3] = 0x06;
    assert(tracker.update(state).event ==
           namco::pacmania_feedback_event::pellet);

    ++state.pellet_count;
    state.frightened_timer = 0xf0;
    state.score_signature[3] = 0x12;
    assert(tracker.update(state).event ==
           namco::pacmania_feedback_event::power_pellet);

    --state.frightened_timer;
    state.score_signature[2] = 0x12;
    const namco::pacmania_feedback_signal first_ghost =
        tracker.update(state);
    assert(first_ghost.event ==
           namco::pacmania_feedback_event::ghost_eaten);
    assert(first_ghost.level == 1);

    --state.frightened_timer;
    state.score_signature[2] = 0x24;
    const namco::pacmania_feedback_signal second_ghost =
        tracker.update(state);
    assert(second_ghost.event ==
           namco::pacmania_feedback_event::ghost_eaten);
    assert(second_ghost.level == 2);

    --state.lives;
    --state.frightened_timer;
    assert(tracker.update(state).event ==
           namco::pacmania_feedback_event::death);

    tracker.reset();
    state = {};
    assert(tracker.update(state).event ==
           namco::pacmania_feedback_event::none);
    state.lives = 3;
    assert(tracker.update(state).event ==
           namco::pacmania_feedback_event::none);

    std::puts("Pac-Mania semantic feedback: ok");
    return 0;
}
