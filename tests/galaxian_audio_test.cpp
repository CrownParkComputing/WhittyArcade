// galaxian_audio_test - merged Galaxian-family sound synth test
//
// Exercises Phoenix, Moon Cresta and UniWar S synth profiles through
// the shared galaxian_sound_synth interface, calling the per-game
// factory functions in galaxian_audio.h. Pure unit test: no audio
// system, no OpenAL. The two old tests (phoenix_audio_test, mooncrst
// audio_test) were structurally identical; consolidating them into one
// binary makes the symmetry obvious in one read.

#include "galaxian_audio.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>

namespace {

int peak(const int16_t* samples, std::size_t count) {
    int result = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const int v = samples[i] < 0 ? -static_cast<int>(samples[i])
                                    : static_cast<int>(samples[i]);
        if (v > result) result = v;
    }
    return result;
}

std::size_t zero_crossings(const int16_t* samples, std::size_t count) {
    std::size_t result = 0;
    for (std::size_t index = 1; index < count; ++index) {
        const bool previous_negative = samples[index - 1] < 0;
        const bool current_negative = samples[index] < 0;
        if (previous_negative != current_negative) ++result;
    }
    return result;
}

bool silent(const int16_t* samples, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index)
        if (samples[index] != 0) return false;
    return true;
}

}  // namespace

int main() {
    constexpr int kFrames = 4800;
    std::array<int16_t, kFrames> samples{};

    // --- Phoenix path ---
    {
        auto synth = make_phoenix_sound_synth();
        synth->reset();
        std::fill(samples.begin(), samples.end(), 0);
        synth->generate(samples.data(), kFrames, 100, 100);

        // Background music stimulus: control_b bit 7 set selects
        // phoenix_theme (tune index 3). The first lead voice
        // (A(2) = 744 Hz) and bass voice (D(1) = 186 Hz) are
        // scheduled on advance_tune; effect voices are off until a
        // (control & 0x3f) transition is observed.
        synth->write_control(0, 0x00);
        synth->write_control(1, 0xc0);
        std::fill(samples.begin(), samples.end(), 0);
        synth->generate(samples.data(), kFrames, 100, 100);
        assert(synth->active_tune() == 3);
        assert(peak(samples.data(), samples.size()) > 1000);
        std::printf("Phoenix: active_tune=%d peak=%d\n", synth->active_tune(),
                    peak(samples.data(), samples.size()));
    }

    // --- Moon Cresta path ---
    {
        auto synth = make_mooncrst_sound_synth();
        synth->reset();
        std::fill(samples.begin(), samples.end(), 0);
        synth->generate(samples.data(), kFrames, 100, 100);
        // Reset hardware is silent. Audio starts only when the emulated Z80
        // program writes a latch; there is no synthetic always-running tune.
        assert(silent(samples.data(), samples.size()));

        // B800 is the ROM-driven pitch divider latch. A lower divider value
        // must yield a different edge rate, proving that the actual latch
        // data controls the tone rather than a hard-coded melody table.
        synth->write_control(mooncrst_audio_port::pitch, 0x80);
        std::fill(samples.begin(), samples.end(), 0);
        synth->generate(samples.data(), kFrames, 100, 100);
        const int pitch_peak = peak(samples.data(), samples.size());
        const std::size_t slow_edges = zero_crossings(samples.data(),
                                                      samples.size());
        assert(pitch_peak > 1000);
        assert(slow_edges > 20);

        synth->reset();
        synth->write_control(mooncrst_audio_port::pitch, 0xc0);
        std::fill(samples.begin(), samples.end(), 0);
        synth->generate(samples.data(), kFrames, 100, 100);
        const std::size_t fast_edges = zero_crossings(samples.data(),
                                                      samples.size());
        assert(fast_edges > slow_edges + 20);

        // A004-A007 control the background DAC and A800-A802 gate its three
        // 555 voices. These register families must remain disjoint.
        synth->reset();
        synth->write_control(mooncrst_audio_port::lfo_base | 0u, 1);
        synth->write_control(mooncrst_audio_port::sound_base | 0u, 1);
        std::fill(samples.begin(), samples.end(), 0);
        synth->generate(samples.data(), kFrames, 100, 100);
        const int background_peak = peak(samples.data(), samples.size());
        assert(background_peak > 100);

        // HIT and FIRE are produced by the LFSR/analogue envelope paths and
        // obey the effects-volume control independently of music.
        synth->reset();
        synth->write_control(mooncrst_audio_port::sound_base | 3u, 1);
        std::fill(samples.begin(), samples.end(), 0);
        synth->generate(samples.data(), kFrames, 0, 100);
        const int hit_peak = peak(samples.data(), samples.size());
        assert(hit_peak > 100);

        synth->reset();
        synth->write_control(mooncrst_audio_port::sound_base | 5u, 1);
        std::fill(samples.begin(), samples.end(), 0);
        synth->generate(samples.data(), kFrames, 0, 100);
        const int fire_peak = peak(samples.data(), samples.size());
        assert(fire_peak > hit_peak);

        synth->reset();
        synth->write_control(mooncrst_audio_port::pitch, 0x80);
        std::fill(samples.begin(), samples.end(), 0);
        synth->generate(samples.data(), kFrames, 0, 100);
        assert(silent(samples.data(), samples.size()));

        std::printf("Moon Cresta discrete: pitch=%d edges=%zu/%zu "
                    "background=%d hit=%d fire=%d\n",
                    pitch_peak, slow_edges, fast_edges, background_peak,
                    hit_peak, fire_peak);
    }

    // --- UniWar S / base Galaxian discrete mixer path ---
    {
        auto synth = make_uniwars_sound_synth();
        synth->reset();
        std::fill(samples.begin(), samples.end(), 0);
        synth->generate(samples.data(), kFrames, 100, 100);
        assert(silent(samples.data(), samples.size()));

        synth->write_control(mooncrst_audio_port::pitch, 0x80);
        synth->write_control(mooncrst_audio_port::sound_base | 6u, 1);
        std::fill(samples.begin(), samples.end(), 0);
        synth->generate(samples.data(), kFrames, 100, 100);
        const int pitch_peak = peak(samples.data(), samples.size());
        const std::size_t pitch_edges =
            zero_crossings(samples.data(), samples.size());
        assert(pitch_peak > 1000);
        assert(pitch_edges > 20);

        synth->reset();
        synth->write_control(mooncrst_audio_port::lfo_base | 0u, 1);
        synth->write_control(mooncrst_audio_port::sound_base | 0u, 1);
        std::fill(samples.begin(), samples.end(), 0);
        synth->generate(samples.data(), kFrames, 100, 100);
        const int background_peak = peak(samples.data(), samples.size());
        assert(background_peak > 100);

        std::printf("UniWar S discrete: pitch=%d edges=%zu background=%d\n",
                    pitch_peak, pitch_edges, background_peak);
    }

    std::puts("Galaxian audio: all synth profiles exercised via shared base");
    return 0;
}
