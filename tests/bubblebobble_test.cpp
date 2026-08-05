// The Bubble Bobble simulation, and the properties everything built on it
// assumes are true.
//
// Deliberately NOT written with assert(). This repository builds its tests in
// Release, where -DNDEBUG removes assert() entirely and a test file full of
// them passes without checking anything - which has already happened here, in
// 26 of 57 targets. `check` is a real function call and survives every build
// type, and the failure count is the process exit status.

#include "bubblebobble/bb_game.h"

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

bb::player_intent idle() {
    bb::player_intent intent;
    intent.connected = true;
    return intent;
}

// A scripted player. Deterministic by construction - no clock, no rand - so two
// runs of the same script are genuinely the same inputs.
bb::player_intent scripted(int frame) {
    bb::player_intent intent = idle();
    intent.move_x = ((frame / 40) % 2 == 0) ? 1.0f : -1.0f;
    intent.jump = (frame % 37) == 0;
    intent.fire = (frame % 23) == 0;
    return intent;
}

void run(bb::game& g, int frames, bb::player_intent (*script)(int)) {
    for (int frame = 0; frame < frames; ++frame) {
        const bb::player_intent intents[2] = {script(frame), idle()};
        g.step(intents, 2);
    }
}

// Same inputs, same outcome - the claim the lobby's lockstep rests on. A
// desync shows up as two players quietly seeing different screens with no error
// anywhere, so it is pinned here rather than discovered in a match.
void test_determinism() {
    std::printf("determinism\n");
    bb::game a;
    bb::game b;
    run(a, 900, scripted);
    run(b, 900, scripted);
    check(a.checksum() == b.checksum(),
          "two runs of one script agree bit for bit");
    check(a.score() == b.score(), "and agree on the score");

    // And a different script must actually reach a different state, or the
    // check above would pass on a simulation that ignored its input entirely.
    bb::game c;
    run(c, 900, [](int frame) {
        bb::player_intent intent = idle();
        intent.move_x = -1.0f;
        intent.fire = (frame % 11) == 0;
        return intent;
    });
    check(c.checksum() != a.checksum(),
          "a different script reaches a different state");
}

// Where seat one's dragon is being drawn, or -1 while it is not on screen.
// Reading the position off the drawn frame rather than from a getter keeps the
// test honest about what a player can actually see.
float player_y(const bb::game& g) {
    for (const bb::sprite_quad& quad : g.quads())
        if (quad.kind == bb::sprite_kind::player_green) return quad.y;
    return -1.0f;
}

void test_gravity_and_floor() {
    std::printf("gravity and the floor\n");
    bb::game g;
    run(g, 1, [](int) { return idle(); });
    const float resting = player_y(g);
    check(resting > 0.0f, "the dragon starts on screen, resting");

    // A jump has to come back down. This is the check that a plain "is it
    // still alive" test cannot make: with gravity removed the dragon rises for
    // ever and every liveness assertion still passes, which is exactly how
    // this test was vacuous when first written.
    bb::player_intent jump = idle();
    jump.jump = true;
    const bb::player_intent first[2] = {jump, idle()};
    g.step(first, 2);

    float highest = resting;
    float landed = -1.0f;
    for (int frame = 0; frame < 80; ++frame) {
        const bb::player_intent intents[2] = {idle(), idle()};
        g.step(intents, 2);
        const float y = player_y(g);
        if (y < 0.0f) continue; // blinking; say nothing about this frame
        highest = std::min(highest, y);
        if (frame > 8 && std::fabs(y - resting) < 0.5f) landed = y;
    }
    check(highest < resting - 4.0f, "the jump actually left the ground");
    check(landed > 0.0f, "and gravity brought it back to the floor");
    check(!g.game_over(), "a dragon that jumps is still in play");
    check(g.lives(0) >= 1, "and has not lost a life to its own jump");
}

void test_one_way_platforms() {
    std::printf("platforms are solid from above only\n");
    bb::game g;
    // Level one's ledges include the row-15 run at columns 3..13. A tile there
    // is solid; the air above it is not.
    check(g.solid(5, 15), "row 15 carries a ledge");
    check(!g.solid(5, 14), "and nothing sits above it");
    check(g.solid(0, 0), "the frame is solid");
    check(!g.solid(-1, 5), "off-map columns are not solid");
    check(!g.solid(5, 999), "off-map rows are not solid");
}

void test_floor_has_the_gaps_wrapping_needs() {
    std::printf("the floor's gaps\n");
    bb::game g;
    // Falling out of the bottom is a mechanic, and it is only reachable
    // because the floor is not continuous. A solid floor would delete it
    // silently, so the gap is asserted rather than assumed.
    check(!g.solid(1, 24), "the left gap is open");
    check(!g.solid(30, 24), "the right gap is open");
    check(g.solid(10, 24), "and the rest of the floor is not");
}

void test_bubbles_trap_and_pop() {
    std::printf("bubbles trap, and popping pays\n");
    bb::game g;
    const uint64_t opening_score = g.score();

    // A score that merely went up proves nothing - a trap that failed to put
    // the enemy IN the bubble still scores, which is how this test passed
    // against a mutant where bubbles never held anything. The observable that
    // cannot be faked is an occupied bubble appearing on screen.
    bool saw_full_bubble = false;
    int fewest_enemies = 99;
    for (int frame = 0; frame < 1800; ++frame) {
        bb::player_intent intent = idle();
        intent.move_x = ((frame / 90) % 2 == 0) ? 1.0f : -1.0f;
        intent.fire = true;
        intent.jump = (frame % 53) == 0;
        const bb::player_intent intents[2] = {intent, idle()};
        g.step(intents, 2);

        int enemies = 0;
        for (const bb::sprite_quad& quad : g.quads()) {
            if (quad.kind == bb::sprite_kind::bubble_full)
                saw_full_bubble = true;
            if (quad.kind == bb::sprite_kind::enemy) ++enemies;
        }
        fewest_enemies = std::min(fewest_enemies, enemies);
    }

    check(saw_full_bubble, "a bubble caught a Zen-chan and carried it");
    check(fewest_enemies < 3, "the screen emptied as they were caught");
    check(g.score() > opening_score, "and catching them scored");
}

void test_quads_describe_the_screen() {
    std::printf("the drawn screen\n");
    bb::game g;
    run(g, 1, [](int) { return idle(); });
    const auto& quads = g.quads();
    check(!quads.empty(), "a frame produces geometry");

    int walls = 0;
    int blocks = 0;
    int players = 0;
    int enemies = 0;
    bool in_bounds = true;
    for (const bb::sprite_quad& quad : quads) {
        if (quad.kind == bb::sprite_kind::wall) ++walls;
        if (quad.kind == bb::sprite_kind::block) ++blocks;
        if (quad.kind == bb::sprite_kind::player_green) ++players;
        if (quad.kind == bb::sprite_kind::enemy) ++enemies;
        if (quad.x < -bb::tile_size || quad.y < -bb::tile_size ||
            quad.x > bb::world_width || quad.y > bb::world_height)
            in_bounds = false;
    }
    check(walls > 0, "the frame is drawn");
    check(blocks > 0, "the ledges are drawn");
    check(players == 1, "seat one's dragon is on screen");
    check(enemies >= 3, "and the screen's Zen-chans are too");
    check(in_bounds, "nothing is drawn outside the world");
}

void test_seat_two_joins_only_when_asked() {
    std::printf("the second seat\n");
    bb::game g;
    run(g, 120, [](int) { return idle(); });
    const auto& quads = g.quads();
    int blue = 0;
    for (const bb::sprite_quad& quad : quads)
        if (quad.kind == bb::sprite_kind::player_blue) ++blue;
    check(blue == 0, "an unjoined seat puts no dragon on screen");

    // Now press start on seat two and it appears.
    for (int frame = 0; frame < 30; ++frame) {
        bb::player_intent two = idle();
        two.start = true;
        const bb::player_intent intents[2] = {idle(), two};
        g.step(intents, 2);
    }
    blue = 0;
    for (const bb::sprite_quad& quad : g.quads())
        if (quad.kind == bb::sprite_kind::player_blue) ++blue;
    check(blue == 1, "pressing start brings the second dragon in");
}

void test_cues_are_events_not_state() {
    std::printf("audio cues\n");
    bb::game g;
    bool heard_shot = false;
    for (int frame = 0; frame < 60; ++frame) {
        bb::player_intent intent = idle();
        intent.fire = true;
        const bb::player_intent intents[2] = {intent, idle()};
        g.step(intents, 2);
        for (bb::cue what : g.take_cues())
            if (what == bb::cue::shoot) heard_shot = true;
    }
    check(heard_shot, "firing asks for a sound");
    // Drained means drained: asking twice must not replay the frame.
    const std::vector<bb::cue> again = g.take_cues();
    check(again.empty(), "a drained cue is not delivered twice");
}

void test_reset_returns_to_the_opening_screen() {
    std::printf("reset\n");
    bb::game g;
    const uint64_t opening = g.checksum();
    run(g, 300, scripted);
    check(g.checksum() != opening, "play changes the state");
    g.reset();
    check(g.checksum() == opening, "reset returns to the opening state exactly");
}

} // namespace

int main() {
    test_determinism();
    test_gravity_and_floor();
    test_one_way_platforms();
    test_floor_has_the_gaps_wrapping_needs();
    test_bubbles_trap_and_pop();
    test_quads_describe_the_screen();
    test_seat_two_joins_only_when_asked();
    test_cues_are_events_not_state();
    test_reset_returns_to_the_opening_screen();

    std::printf("\n%d check(s), %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
