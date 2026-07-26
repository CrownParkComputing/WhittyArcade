// The frame ring behind lockstep netplay. Both machines simulate a frame only
// once both inputs for it are known, so the indexing that files a peer's
// input by frame - and the wraparound that reuses each slot - decides whether
// two cabinets stay in step or silently drift apart.
#include <array>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
}

// Mirrors arcade_input's ring: capacity and the delay a frame is published at.
constexpr int ring = 64;
constexpr int delay = 3;

struct link {
    std::array<int, ring> peer{};
    std::array<bool, ring> known{};
    unsigned frame{0};

    void receive(unsigned at, int value) {
        peer[at % ring] = value;
        known[at % ring] = true;
    }
    bool ready() const { return known[frame % ring]; }
    int consume() {
        const int value = peer[frame % ring];
        known[frame % ring] = false;
        ++frame;
        return value;
    }
};

} // namespace

int main() {
    // A frame cannot run until its input lands, and runs exactly once after.
    {
        link a;
        check(!a.ready(), "frame 0 must wait for the peer");
        a.receive(0, 100);
        check(a.ready(), "frame 0 runs once its input arrives");
        check(a.consume() == 100, "frame 0 uses the peer's frame 0 input");
        check(!a.ready(), "frame 1 must wait in turn");
    }

    // Out-of-order arrival: a later frame landing first must not let an
    // earlier one run with the wrong input.
    {
        link a;
        a.receive(2, 202);
        check(!a.ready(), "a future input must not satisfy this frame");
        a.receive(0, 200);
        a.receive(1, 201);
        check(a.consume() == 200, "frame 0 takes its own input");
        check(a.consume() == 201, "frame 1 takes its own input");
        check(a.consume() == 202, "frame 2 takes the input that arrived first");
    }

    // A duplicate is harmless: it lands in the same slot.
    {
        link a;
        a.receive(0, 55);
        a.receive(0, 55);
        check(a.consume() == 55, "a duplicated packet changes nothing");
    }

    // The publish delay must stay inside the ring, or a frame's input would
    // be overwritten by one a lap later before it is ever consumed.
    check(delay < ring, "the delay must fit inside the ring");

    // A full lap: slot reuse must line up exactly, so frame N and frame
    // N+ring never collide while N is still pending.
    {
        link a;
        for (unsigned f = 0; f < ring * 3; ++f) {
            a.receive(f, static_cast<int>(f) * 7);
            check(a.ready(), "frame " + std::to_string(f) + " should be ready");
            check(a.consume() == static_cast<int>(f) * 7,
                  "frame " + std::to_string(f) + " must use its own input");
        }
        check(a.frame == ring * 3, "every frame advanced exactly once");
    }

    // The opening frames must be seeded, or a session deadlocks before it
    // starts: input is published `delay` frames ahead, so the first packet
    // either side sends describes frame `delay` and nobody ever publishes
    // frames 0..delay-1. Both machines then wait forever. This is not
    // hypothetical - it froze two real cabinets on the first run.
    {
        link a;
        for (int f = 0; f < delay; ++f) a.known[f % ring] = true;
        for (int f = 0; f < delay; ++f) {
            check(a.ready(), "seeded frame " + std::to_string(f) +
                                 " must run without waiting for a peer");
            check(a.consume() == 0, "a seeded frame carries neutral input");
        }
        // From the delay onwards the peer's real packets take over.
        a.receive(delay, 99);
        check(a.ready(), "the first published frame follows the seeded ones");
        check(a.consume() == 99, "and carries the peer's real input");
    }

    // Without seeding, the same link deadlocks - the bug this guards.
    {
        link a;
        a.receive(delay, 99);   // the earliest anyone publishes
        check(!a.ready(), "an unseeded link stalls at frame 0");
        check(a.frame == 0, "and never advances - the deadlock");
    }

    // Running ahead is what desyncs a session: with a gap in the middle, the
    // link must stop at the gap rather than skipping to what did arrive.
    {
        link a;
        a.receive(0, 1);
        a.receive(2, 3);      // frame 1 never arrives
        check(a.consume() == 1, "frame 0 runs");
        check(!a.ready(), "the missing frame must stall, not be skipped");
        check(a.frame == 1, "a stalled link must not advance");
    }

    if (failures == 0) std::printf("netplay lockstep tests passed\n");
    return failures == 0 ? 0 : 1;
}
