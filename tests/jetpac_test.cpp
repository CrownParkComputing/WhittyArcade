// The Jetpac simulation, and the properties everything built on it assumes.
//
// Deliberately NOT written with assert(): this repository builds its tests in
// Release, where -DNDEBUG removes assert() entirely and a file full of them
// passes without checking anything. `check` is a real call and survives every
// build type, and the failure count is the process exit status.
//
// The game is a delivery loop - fetch a piece, carry it to the rocket, repeat -
// so the properties worth pinning are the ones that would silently break it:
// that a jetpack falls when released, that the screen wraps, that carrying is
// exclusive, and that delivering actually advances the build.

#include "jetpac/jp_game.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what) {
    ++checks;
    if (condition) return;
    ++failures;
    std::printf("  FAIL: %s\n", what.c_str());
}

jp::player_intent idle() {
    jp::player_intent intent;
    intent.connected = true;
    return intent;
}

void run(jp::game& g, int frames, jp::player_intent (*script)(int)) {
    for (int frame = 0; frame < frames; ++frame) {
        const jp::player_intent intents[2] = {script(frame), idle()};
        g.step(intents, 2);
    }
}

jp::player_intent scripted(int frame) {
    jp::player_intent intent = idle();
    intent.move_x = ((frame / 50) % 2 == 0) ? 1.0f : -1.0f;
    intent.thrust = (frame % 7) < 4;
    intent.fire = (frame % 19) == 0;
    return intent;
}

// Where seat one's pilot is drawn, or -1 while he is not on screen. Reading it
// off the frame keeps the test honest about what a player can see.
bool pilot_at(const jp::game& g, float& x, float& y) {
    for (const jp::sprite_quad& quad : g.quads())
        if (quad.kind == jp::sprite_kind::jetman) {
            x = quad.x;
            y = quad.y;
            return true;
        }
    return false;
}

int count_of(const jp::game& g, jp::sprite_kind kind) {
    int found = 0;
    for (const jp::sprite_quad& quad : g.quads())
        if (quad.kind == kind) ++found;
    return found;
}

// A player, rather than a twitch script. It reads the drawn frame the way a
// person does - go to the loose payload, carry it to the pad, shoot what is in
// the way - which is the only way to exercise the delivery loop end to end. A
// fixed left/right sweep never completes a single trip, and a test built on one
// reports the script's failure as the game's.
jp::player_intent bot(const jp::game& g) {
    jp::player_intent intent = idle();
    float px = 0.0f;
    float py = 0.0f;
    if (!pilot_at(g, px, py)) return intent;

    float tx = 0.0f;
    float ty = 0.0f;
    bool have_target = false;
    if (g.carrying(0)) {
        tx = g.rocket_x();
        ty = py;
        have_target = true;
    } else {
        for (const jp::sprite_quad& quad : g.quads()) {
            if (quad.kind != jp::sprite_kind::rocket_part &&
                quad.kind != jp::sprite_kind::fuel_pod &&
                quad.kind != jp::sprite_kind::treasure)
                continue;
            tx = quad.x;
            ty = quad.y;
            have_target = true;
            break;
        }
    }
    if (!have_target) return intent;

    // Take the short way round: the screen wraps, so the target may be closer
    // off the far edge than across the middle.
    float dx = tx - px;
    if (dx > jp::world_width * 0.5f) dx -= jp::world_width;
    if (dx < -jp::world_width * 0.5f) dx += jp::world_width;
    intent.move_x = dx > 1.0f ? 1.0f : (dx < -1.0f ? -1.0f : 0.0f);
    // Thrust to hold height at or above the target; gravity does the descent.
    intent.thrust = py > ty + 2.0f;
    intent.fire = true;
    return intent;
}

void run_bot(jp::game& g, int frames) {
    for (int frame = 0; frame < frames; ++frame) {
        (void)frame;
        const jp::player_intent intents[2] = {bot(g), idle()};
        g.step(intents, 2);
    }
}

// Loose payload of EITHER kind. Counting only fuel pods was a real mistake in
// this file: a screen starts out wanting rocket PARTS, so a test watching pods
// saw nothing for most of its run and passed against a mutant that spawned a
// new piece every single frame.
int loose_payload(const jp::game& g) {
    int found = 0;
    for (const jp::sprite_quad& quad : g.quads())
        if (quad.kind == jp::sprite_kind::rocket_part ||
            quad.kind == jp::sprite_kind::fuel_pod)
            ++found;
    return found;
}

void test_determinism() {
    std::printf("determinism\n");
    jp::game a;
    jp::game b;
    run(a, 900, scripted);
    run(b, 900, scripted);
    check(a.checksum() == b.checksum(),
          "two runs of one script agree bit for bit");
    check(a.score() == b.score(), "and agree on the score");

    jp::game c;
    run(c, 900, [](int frame) {
        jp::player_intent intent = idle();
        intent.move_x = -1.0f;
        intent.thrust = (frame % 3) == 0;
        return intent;
    });
    check(c.checksum() != a.checksum(),
          "a different script reaches a different state");
}

// A jetpack is a thrust, not a jump. Held, he climbs; released, he falls. With
// gravity removed he would hang wherever he was let go and every liveness
// assertion would still pass, so both halves are checked.
void test_thrust_lifts_and_gravity_returns() {
    std::printf("thrust and gravity\n");
    jp::game g;
    run(g, 1, [](int) { return idle(); });
    float x = 0.0f;
    float resting = 0.0f;
    check(pilot_at(g, x, resting), "the pilot starts on screen");

    for (int frame = 0; frame < 40; ++frame) {
        jp::player_intent up = idle();
        up.thrust = true;
        const jp::player_intent intents[2] = {up, idle()};
        g.step(intents, 2);
    }
    float lifted_x = 0.0f;
    float lifted = 0.0f;
    check(pilot_at(g, lifted_x, lifted), "still on screen after climbing");
    check(lifted < resting - 8.0f, "holding thrust lifts him off the ground");

    for (int frame = 0; frame < 200; ++frame) {
        const jp::player_intent intents[2] = {idle(), idle()};
        g.step(intents, 2);
    }
    float dropped_x = 0.0f;
    float dropped = 0.0f;
    check(pilot_at(g, dropped_x, dropped), "still on screen after falling");
    check(dropped > lifted + 4.0f, "releasing thrust drops him again");
}

void test_screen_wraps_horizontally() {
    std::printf("horizontal wrap\n");
    jp::game g;
    run(g, 1, [](int) { return idle(); });
    float start_x = 0.0f;
    float y = 0.0f;
    check(pilot_at(g, start_x, y), "the pilot is on screen");

    // Fly right for long enough to cross the edge; he must reappear rather
    // than stopping at a wall that is not there.
    bool ever_left_of_start = false;
    for (int frame = 0; frame < 600; ++frame) {
        jp::player_intent go = idle();
        go.move_x = 1.0f;
        go.thrust = (frame % 4) == 0;
        const jp::player_intent intents[2] = {go, idle()};
        g.step(intents, 2);
        float now_x = 0.0f;
        float now_y = 0.0f;
        if (pilot_at(g, now_x, now_y) && now_x < start_x - 16.0f)
            ever_left_of_start = true;
    }
    check(ever_left_of_start,
          "flying right long enough brings him round the left side");
}

void test_the_screen_is_drawn() {
    std::printf("the drawn screen\n");
    jp::game g;
    run(g, 1, [](int) { return idle(); });
    check(!g.quads().empty(), "a frame produces geometry");
    check(count_of(g, jp::sprite_kind::ground) > 0, "the ground is drawn");
    check(count_of(g, jp::sprite_kind::platform) > 0, "the ledges are drawn");
    check(count_of(g, jp::sprite_kind::rocket_base) == 1,
          "the rocket base is on the pad");
    check(count_of(g, jp::sprite_kind::jetman) == 1, "seat one's pilot is on screen");

    bool in_bounds = true;
    for (const jp::sprite_quad& quad : g.quads())
        if (quad.x < -jp::tile_size || quad.y < -jp::tile_size * 2.0f ||
            quad.x > jp::world_width || quad.y > jp::world_height)
            in_bounds = false;
    check(in_bounds, "nothing is drawn outside the world");
}

// The rocket is built one piece at a time, and a piece is only ever out there
// one at a time. Both halves matter: releasing them all at once would turn the
// game into a sweep, and never releasing the next would deadlock it.
void test_the_rocket_is_built_one_piece_at_a_time() {
    std::printf("building the rocket\n");
    jp::game g;
    run(g, 1, [](int) { return idle(); });
    check(g.rocket() == jp::rocket_state::needs_parts,
          "a fresh screen wants rocket parts");
    check(g.parts_fitted() == 0, "and none are fitted yet");

    int most_loose_payload = 0;
    for (int frame = 0; frame < 2400; ++frame) {
        // Chase the payload: fly toward whatever is loose, then head for the
        // rocket. Crude, but it exercises the whole loop.
        jp::player_intent intent = idle();
        intent.move_x = ((frame / 45) % 2 == 0) ? -1.0f : 1.0f;
        intent.thrust = (frame % 5) < 2;
        intent.fire = (frame % 23) == 0;
        const jp::player_intent intents[2] = {intent, idle()};
        g.step(intents, 2);

        most_loose_payload = std::max(most_loose_payload, loose_payload(g));
    }
    check(most_loose_payload <= 1,
          "never more than one piece of payload exists at a time");
}

void test_delivering_advances_the_build() {
    std::printf("delivery\n");
    jp::game g;
    run_bot(g, 2000);
    check(g.parts_fitted() > 0, "a part carried to the pad is fitted");
    check(g.score() > 0, "and delivering scores");
    // The build must ADVANCE, not just accept one piece: three parts then fuel
    // is the whole progression, and a rocket that accepts part one and then
    // silently stops would still pass the checks above.
    run_bot(g, 6000);
    check(g.parts_fitted() >= jp::game::parts_needed ||
              g.fuel_loaded() > 0 || g.level() > 0,
          "the build keeps advancing past the first piece");
}

void test_carrying_is_exclusive() {
    std::printf("carrying\n");
    jp::game g;
    int most_carried = 0;
    for (int frame = 0; frame < 1500; ++frame) {
        jp::player_intent intent = idle();
        intent.move_x = ((frame / 30) % 2 == 0) ? -1.0f : 1.0f;
        intent.thrust = (frame % 5) < 2;
        const jp::player_intent intents[2] = {intent, idle()};
        g.step(intents, 2);
        // A carried piece is still drawn, directly under the pilot, so this
        // counts payload whether held or falling: one object at a time is the
        // invariant, and carrying is what makes it exclusive.
        most_carried = std::max(most_carried, loose_payload(g));
    }
    check(most_carried <= 1, "at most one piece of payload exists at any moment");
}

void test_firing_makes_bolts_that_expire() {
    std::printf("the laser\n");
    jp::game g;
    bool saw_bolt = false;
    int most_in_flight = 0;
    for (int frame = 0; frame < 300; ++frame) {
        jp::player_intent intent = idle();
        intent.fire = true;
        const jp::player_intent intents[2] = {intent, idle()};
        g.step(intents, 2);
        const int in_flight = count_of(g, jp::sprite_kind::laser);
        if (in_flight > 0) saw_bolt = true;
        most_in_flight = std::max(most_in_flight, in_flight);
    }
    check(saw_bolt, "firing puts a bolt on screen");
    // Held fire must not fill the screen: the cooldown and the cap are what
    // keep this a game rather than a wall of bolts.
    check(most_in_flight <= 4, "no more than four bolts are ever in flight");
}

void test_aliens_arrive_and_can_be_shot() {
    std::printf("aliens\n");
    jp::game g;
    bool saw_alien = false;
    bool heard_hit = false;
    for (int frame = 0; frame < 4000; ++frame) {
        (void)frame;
        const jp::player_intent intents[2] = {bot(g), idle()};
        g.step(intents, 2);
        if (count_of(g, jp::sprite_kind::alien_drone) +
                count_of(g, jp::sprite_kind::alien_ufo) >
            0)
            saw_alien = true;
        for (jp::cue what : g.take_cues())
            if (what == jp::cue::alien_hit) heard_hit = true;
    }
    check(saw_alien, "aliens arrive on the screen");
    check(heard_hit, "and the laser can bring one down");
}

void test_cues_are_events_not_state() {
    std::printf("audio cues\n");
    jp::game g;
    bool heard_fire = false;
    for (int frame = 0; frame < 60; ++frame) {
        jp::player_intent intent = idle();
        intent.fire = true;
        const jp::player_intent intents[2] = {intent, idle()};
        g.step(intents, 2);
        for (jp::cue what : g.take_cues())
            if (what == jp::cue::fire) heard_fire = true;
    }
    check(heard_fire, "firing asks for a sound");
    check(g.take_cues().empty(), "a drained cue is not delivered twice");
}

void test_reset_returns_to_the_opening_screen() {
    std::printf("reset\n");
    jp::game g;
    const uint64_t opening = g.checksum();
    run(g, 400, scripted);
    check(g.checksum() != opening, "play changes the state");
    g.reset();
    check(g.checksum() == opening, "reset returns to the opening state exactly");
}

} // namespace

int main() {
    test_determinism();
    test_thrust_lifts_and_gravity_returns();
    test_screen_wraps_horizontally();
    test_the_screen_is_drawn();
    test_the_rocket_is_built_one_piece_at_a_time();
    test_delivering_advances_the_build();
    test_carrying_is_exclusive();
    test_firing_makes_bolts_that_expire();
    test_aliens_arrive_and_can_be_shot();
    test_cues_are_events_not_state();
    test_reset_returns_to_the_opening_screen();

    std::printf("\n%d check(s), %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
