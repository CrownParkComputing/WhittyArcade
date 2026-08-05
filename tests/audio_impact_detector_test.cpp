#include "audio_impact_detector.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>

int main() {
    audio_impact_detector detector;
    std::array<float, 256 * 2> block{};
    float phase = 0.0f;
    float strongest_steady = 0.0f;
    for (int pass = 0; pass < 80; ++pass) {
        for (std::size_t frame = 0; frame < 256; ++frame) {
            const float value = 0.12f * std::sin(phase);
            phase += 0.11f;
            block[frame * 2] = value;
            block[frame * 2 + 1] = value;
        }
        strongest_steady = std::max(
            strongest_steady, detector.consume_float(block.data(), 256, 2));
    }
    assert(strongest_steady == 0.0f &&
           "steady engine/music-like audio must not continuously rumble");

    for (std::size_t frame = 0; frame < 256; ++frame) {
        const float sign = (frame & 1) ? -1.0f : 1.0f;
        const float envelope = 1.0f - static_cast<float>(frame) / 300.0f;
        block[frame * 2] = sign * envelope;
        block[frame * 2 + 1] = sign * envelope * 0.9f;
    }
    const float crash = detector.consume_float(block.data(), 256, 2);
    assert(crash >= 0.2f && "a sudden broadband crash must be detected");

    block.fill(0.0f);
    const float immediate_repeat = detector.consume_float(block.data(), 256, 2);
    assert(immediate_repeat == 0.0f && "impact cooldown must suppress chatter");

    detector.reset();
    for (int pass = 0; pass < 12; ++pass) {
        for (std::size_t frame = 0; frame < 256; ++frame) {
            const float value = 0.018f * std::sin(phase);
            phase += 0.08f;
            block[frame * 2] = value;
            block[frame * 2 + 1] = value;
        }
        assert(detector.consume_float(block.data(), 256, 2) == 0.0f);
    }
    for (std::size_t frame = 0; frame < 256; ++frame) {
        const float value = 0.35f * std::sin(phase);
        phase += 0.08f;
        block[frame * 2] = value;
        block[frame * 2 + 1] = value;
    }
    const float tonal_death = detector.consume_float(block.data(), 256, 2);
    assert(tonal_death >= 0.2f &&
           "a sudden tonal death cue must be detected");

    detector.reset();
    for (int pass = 0; pass < 12; ++pass) {
        for (std::size_t frame = 0; frame < 256; ++frame) {
            const float engine = 0.16f * std::sin(phase);
            phase += 0.045f;
            block[frame * 2] = engine;
            block[frame * 2 + 1] = engine;
        }
        assert(detector.consume_float(block.data(), 256, 2) == 0.0f);
    }
    for (std::size_t frame = 0; frame < 256; ++frame) {
        const float engine = 0.16f * std::sin(phase);
        phase += 0.045f;
        const float wall_scrape = (frame & 1) ? -0.13f : 0.13f;
        block[frame * 2] = engine + wall_scrape;
        block[frame * 2 + 1] = engine + wall_scrape;
    }
    const float collision = detector.consume_float(block.data(), 256, 2);
    assert(collision >= 0.2f &&
           "a collision layered over engine audio must be detected");
    std::printf("audio impact detector: crash strength %.2f\n", crash);
}
