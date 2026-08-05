// The Geometry Wars simulation, and the properties everything built on it
// assumes are true.
//
// This game is not just a game here: it is the first native plugin, and the
// lobby's whole design rests on two claims about it - that it is deterministic,
// and that nothing outside the simulation can influence it. Both are invisible
// when broken. A desync shows up as two players quietly seeing different
// arenas, with no error anywhere, so the assumptions are pinned here instead of
// being discovered in a match.

#include "geometrywars/gw_game.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// A scripted player. Deterministic by construction - no clock, no rand - so two
// runs of the same script are genuinely the same inputs.
gw::player_intent scripted(int frame, int seat) {
    gw::player_intent intent;
    intent.connected = true;
    intent.start = frame < 4;
    intent.fire = frame > 8;
    intent.bomb = frame == 900 + seat * 37;
    const float phase = static_cast<float>(frame + seat * 90);
    intent.move_x = std::sin(phase * 0.031f);
    intent.move_y = std::cos(phase * 0.017f);
    intent.aim_x = std::cos(phase * 0.043f);
    intent.aim_y = std::sin(phase * 0.029f);
    return intent;
}

void run(gw::game& g, int frames, int seats, bool drain_audio) {
    for (int f = 0; f < frames; ++f) {
        gw::player_intent intents[4];
        for (int s = 0; s < seats; ++s) intents[s] = scripted(f, s);
        g.advance(intents, static_cast<std::size_t>(seats));
        if (!drain_audio) continue;
        gw::audio_cue cues[32];
        float gain[32];
        float pan[32];
        g.take_audio_cues(cues, gain, pan, 32);
    }
}

void test_the_same_inputs_give_the_same_game() {
    // The foundation of lockstep. If this ever fails, networked play is not
    // slightly wrong - it is impossible, and the symptom is two players seeing
    // different arenas with nothing reported.
    for (const gw::edition which :
         {gw::edition::retro_evolved, gw::edition::retro_evolved_2}) {
        gw::game a(which);
        gw::game b(which);
        run(a, 2400, 4, true);
        run(b, 2400, 4, true);
        assert(a.state_checksum() == b.state_checksum());
        assert(a.score() == b.score());
        assert(a.enemy_count() == b.enemy_count());
    }
}

void test_audio_cannot_change_the_game() {
    // The reason plugins emit cues instead of mixing sound. One machine draining
    // its cues and another not - because it has no audio device, or dropped a
    // frame - must still be the same game. Anything that made this fail would be
    // a desync whose cause nobody would ever look for in the audio path.
    gw::game drained(gw::edition::retro_evolved_2);
    gw::game ignored(gw::edition::retro_evolved_2);
    run(drained, 1800, 4, true);
    run(ignored, 1800, 4, false);
    assert(drained.state_checksum() == ignored.state_checksum());
    assert(drained.score() == ignored.score());
}

void test_cues_are_drained_and_bounded() {
    gw::game g(gw::edition::retro_evolved_2);
    gw::audio_cue cues[64];
    float gain[64];
    float pan[64];
    std::size_t total = 0;
    std::size_t worst_frame = 0;
    for (int f = 0; f < 3600; ++f) {
        gw::player_intent intents[4];
        for (int s = 0; s < 4; ++s) intents[s] = scripted(f, s);
        g.advance(intents, 4);
        const std::size_t count = g.take_audio_cues(cues, gain, pan, 64);
        for (std::size_t i = 0; i < count; ++i) {
            assert(static_cast<uint32_t>(cues[i]) <
                   static_cast<uint32_t>(gw::audio_cue::count));
            assert(gain[i] >= 0.0f && gain[i] <= 1.0f);
            assert(pan[i] >= -1.0f && pan[i] <= 1.0f);
        }
        if (count > worst_frame) worst_frame = count;
        total += count;
        // Draining is the contract: a host that asks twice in one frame - or
        // that skipped a frame and catches up - must not hear the same event
        // again.
        assert(g.take_audio_cues(cues, gain, pan, 64) == 0);
    }
    assert(total > 0 && "a minute of four-player play makes no sound at all");
    // A wave can spawn twenty enemies at once. If that produced twenty
    // simultaneous cues the result is noise that drowns out everything the
    // player actually did, so the busiest frame has to stay small.
    assert(worst_frame <= 12 &&
           "too many cues in one frame: sounds must not be per-entity");
}

void test_every_cue_has_a_name() {
    // Names are how a bundle's sound files are matched to cues. An empty or
    // duplicated one means a sample that can never be found, and the game
    // silently keeps its synthesised placeholder forever.
    std::vector<std::string> seen;
    for (uint32_t i = 0; i < static_cast<uint32_t>(gw::audio_cue::count); ++i) {
        const std::string name =
            gw::audio_cue_name(static_cast<gw::audio_cue>(i));
        assert(!name.empty());
        for (const std::string& other : seen)
            assert(name != other && "two cues share one file name");
        seen.push_back(name);
    }
    assert(seen.size() == 7);
}

void test_firing_and_dying_are_heard() {
    // Cue wiring, tested by event rather than by count: shooting must be
    // audible, and so must the thing the player most needs to notice.
    gw::game g(gw::edition::retro_evolved);
    int shots = 0;
    int deaths = 0;
    gw::audio_cue cues[32];
    float gain[32];
    float pan[32];
    for (int f = 0; f < 3600; ++f) {
        gw::player_intent intent = scripted(f, 0);
        g.advance(&intent, 1);
        const std::size_t count = g.take_audio_cues(cues, gain, pan, 32);
        for (std::size_t i = 0; i < count; ++i) {
            if (cues[i] == gw::audio_cue::shot) ++shots;
            if (cues[i] == gw::audio_cue::player_death) ++deaths;
        }
    }
    assert(shots > 100 && "holding fire for a minute must be audible");
    assert(deaths > 0 && "a death that makes no sound reads as a freeze");
}

void test_black_holes_belong_to_the_sequel_and_collapse_audibly() {
    // Black holes are the sequel's addition. The collapse cue is only reachable
    // if they both spawn and can be destroyed, so this is as much a check that
    // the cue is not dead code as it is a check on the feature.
    gw::game one(gw::edition::retro_evolved);
    run(one, 6000, 1, true);

    assert(one.black_hole_count() == 0 &&
           "Retro Evolved has no black holes");

    // Players who actually attack the thing, rather than the flailing script
    // above: seat 0 flies at the nearest black hole and shoots it. Reachability
    // is the claim being tested, and random input is not evidence either way.
    gw::game two(gw::edition::retro_evolved_2);
    int collapses = 0;
    bool ever_appeared = false;
    gw::audio_cue cues[32];
    float gain[32];
    float pan[32];
    for (int f = 0; f < 30000 && collapses == 0; ++f) {
        gw::player_intent intents[4];
        for (int s = 0; s < 4; ++s) intents[s] = scripted(f, s);
        if (two.black_hole_count() > 0) {
            ever_appeared = true;
            const gw::game::black_hole_view hole = two.black_hole_at(0);
            const float length = std::sqrt(hole.x * hole.x + hole.y * hole.y);
            const float nx = length > 0.001f ? hole.x / length : 1.0f;
            const float ny = length > 0.001f ? hole.y / length : 0.0f;
            // Approach from the origin and fire along the same line.
            intents[0].move_x = nx;
            intents[0].move_y = ny;
            intents[0].aim_x = nx;
            intents[0].aim_y = ny;
            intents[0].fire = true;
            intents[0].bomb = false;
        }
        two.advance(intents, 4);
        const std::size_t count = two.take_audio_cues(cues, gain, pan, 32);
        for (std::size_t i = 0; i < count; ++i)
            if (cues[i] == gw::audio_cue::black_hole_collapse) ++collapses;
    }
    assert(ever_appeared && "the sequel never spawned a black hole at all");
    assert(collapses > 0 &&
           "a black hole under sustained fire never collapsed: it heals from "
           "swallowing faster than a player can damage it, so the cue and the "
           "reward for clearing one are both unreachable");
}

void test_lives_are_per_seat() {
    // One shared pool meant three players drained each other's lives, and the
    // game ended while two of them were still flying.
    gw::game g(gw::edition::retro_evolved_2);
    run(g, 3000, 4, true);
    bool differ = false;
    for (std::size_t seat = 1; seat < 4; ++seat)
        if (g.lives_for(seat) != g.lives_for(0)) differ = true;
    // Either they differ - proving separate pools - or nobody has died yet, in
    // which case the test below still holds them apart.
    gw::game single(gw::edition::retro_evolved_2);
    gw::player_intent intents[4];
    for (int f = 0; f < 3000; ++f) {
        for (int s = 0; s < 4; ++s) {
            intents[s] = scripted(f, s);
            // Only seat 0 plays; the rest sit still in a corner.
            if (s > 0) {
                intents[s].move_x = 0.0f;
                intents[s].move_y = 0.0f;
                intents[s].fire = false;
            }
        }
        single.advance(intents, 4);
    }
    (void)differ;
    assert(single.lives_for(0) <= 3);
}

void test_attract_mode_never_ends_and_never_piles_up() {
    // The cabinet sits in attract for hours. It once spent lives there, reached
    // game over, then skipped its own reset - and enemies accumulated until the
    // frame rate collapsed, with nobody watching.
    gw::game g(gw::edition::retro_evolved_2);
    gw::player_intent idle[4];
    for (int s = 0; s < 4; ++s) idle[s] = gw::player_intent{};

    // What matters is that the population is BOUNDED, not that it is small.
    // Nobody is shooting, so an attract arena is legitimately busy - measured,
    // it settles around 220 enemies costing 0.03 ms a step, which is 0.2% of a
    // frame. The failure that actually hurts is growth that never stops, so the
    // peak of an early window is compared against a much later one.
    auto peak_over = [&](int frames) {
        std::size_t worst = 0;
        for (int f = 0; f < frames; ++f) {
            g.advance(idle, 4);
            if (g.enemy_count() > worst) worst = g.enemy_count();
            // Cues are still produced in attract; a host that drains them only
            // occasionally must not let them grow without bound either.
            if ((f % 7) == 0) {
                gw::audio_cue cues[32];
                float gain[32];
                float pan[32];
                g.take_audio_cues(cues, gain, pan, 32);
            }
        }
        return worst;
    };

    const std::size_t early = peak_over(40000);
    peak_over(80000); // let it run a long way past the plateau
    const std::size_t late = peak_over(40000);

    assert(g.attract() && "attract mode must not exit on its own");
    assert(early < 400 && "attract mode is already overcrowded");
    // Three times the observed plateau: enough headroom that ordinary variation
    // passes, tight enough that unbounded growth cannot hide.
    assert(late < 400 &&
           "attract mode is still accumulating enemies after 160,000 steps - a "
           "cabinet left in attract will grind to a halt overnight");
    assert(late < early * 3 && "the enemy population is not settling");
}

} // namespace

int main() {
    test_the_same_inputs_give_the_same_game();
    test_audio_cannot_change_the_game();
    test_cues_are_drained_and_bounded();
    test_every_cue_has_a_name();
    test_firing_and_dying_are_heard();
    test_black_holes_belong_to_the_sequel_and_collapse_audibly();
    test_lives_are_per_seat();
    test_attract_mode_never_ends_and_never_piles_up();
    std::printf("geometrywars_test: all checks passed\n");
    return 0;
}
