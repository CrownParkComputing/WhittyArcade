// Two Galaxian boards, identical inputs, compared frame by frame.
//
// Lockstep netplay rests entirely on one property: the same board fed the
// same inputs must produce the same state on both machines. Nothing else in
// the scheme matters if that is untrue, and a divergence is invisible from
// the outside - it looks like the network. This runs both boards inside one
// process, so the network, the windowing and the audio device are all out of
// the picture: any difference here belongs to the emulation itself.
//
// Reports the FIRST frame that differs, which is what distinguishes the
// causes: frame 1 means initial state, a later frame means something
// time-dependent or accumulated.
#include "namco/galaxian/galaxian_machine.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

uint64_t hash_frame(const uint32_t* pixels, int width, int height) {
    uint64_t hash = 1469598103934665603ull;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    for (std::size_t at = 0; at < count; ++at) {
        hash ^= pixels[at];
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: galaxian_determinism_test <galaxian.zip>\n");
        return 0;  // Nothing to check without a ROM; not a failure.
    }
    const std::string rom = argv[1];

    auto first = std::make_unique<galaxian_machine>(
        make_galaxian_board_interface());
    auto second = std::make_unique<galaxian_machine>(
        make_galaxian_board_interface());
    if (!first->load_roms(rom) || !second->load_roms(rom)) {
        std::printf("could not load %s - skipping\n", rom.c_str());
        return 0;
    }

    // Identical, entirely neutral input: no player is touching anything, so
    // any divergence is the board's own doing.
    const input_state neutral{};
    constexpr int frames = 600;   // ten seconds of attract mode
    for (int frame = 0; frame < frames; ++frame) {
        first->set_input(neutral);
        second->set_input(neutral);
        first->run_frame();
        second->run_frame();
        const uint64_t a = hash_frame(first->frame_buffer(),
                                      first->screen_width(),
                                      first->screen_height());
        const uint64_t b = hash_frame(second->frame_buffer(),
                                      second->screen_width(),
                                      second->screen_height());
        if (a != b) {
            std::printf("DIVERGED at frame %d: %016llx vs %016llx\n", frame,
                        static_cast<unsigned long long>(a),
                        static_cast<unsigned long long>(b));
            std::printf("The board is not deterministic under identical "
                        "input, so input-only netplay cannot hold sync.\n");
            return 1;
        }
    }
    std::printf("identical across %d frames - the board is deterministic\n",
                frames);
    return 0;
}
