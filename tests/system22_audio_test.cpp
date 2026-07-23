#include "namco/system22/system22_audio.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
    c352_audio chip;
    std::vector<uint8_t> samples(256);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = (i & 1) ? 0xc0 : 0x40;
    chip.set_sample_rom(samples.data(), samples.size());

    chip.write(0, 0xffff);  // front left/right volume
    chip.write(2, 0xffff);  // frequency
    chip.write(4, 0x0000);  // wave bank
    chip.write(5, 0x0000);  // start
    chip.write(6, 0x00ff);  // end
    chip.write(7, 0x0000);  // loop
    chip.write(3, 0x4006);  // key on, loop, no interpolation
    chip.write(0x202, 0);   // execute key events

    if ((chip.read(3) & 0x8000) == 0) {
        std::fprintf(stderr, "FAIL: key-on did not start C352 voice\n");
        return 1;
    }

    std::vector<int16_t> output(1024 * 2);
    chip.generate_samples(output.data(), 1024);
    if (std::all_of(output.begin(), output.end(), [](int16_t value) {
            return value == 0;
        })) {
        std::fprintf(stderr, "FAIL: active C352 voice produced silence\n");
        return 1;
    }

    std::fill(output.begin(), output.end(), 0);
    chip.generate_samples(output.data(), 1024, 0, 100);
    if (!std::all_of(output.begin(), output.end(), [](int16_t value) {
            return value == 0;
        })) {
        std::fprintf(stderr, "FAIL: music volume did not mute looping voices\n");
        return 1;
    }

    // A long, non-looping voice models the C74's speech playback. It must
    // remain busy until the programmed 16-bit end address is actually
    // reached, then key off cleanly rather than being truncated early.
    c352_audio speech_chip;
    std::vector<uint8_t> speech_samples(0x10000);
    for (std::size_t i = 0; i < speech_samples.size(); ++i)
        speech_samples[i] = static_cast<uint8_t>((i & 0x7f) + 1);
    speech_chip.set_sample_rom(speech_samples.data(), speech_samples.size());
    speech_chip.write(0, 0xffff);
    speech_chip.write(2, 0xffff);
    speech_chip.write(4, 0x0000);
    speech_chip.write(5, 0x0000);
    speech_chip.write(6, 0xffff);
    speech_chip.write(7, 0x0000);
    speech_chip.write(3, 0x4004);  // key on, PCM, no interpolation
    speech_chip.write(0x202, 0);
    std::vector<int16_t> speech_output(32000 * 2);
    speech_chip.generate_samples(speech_output.data(), 32000);
    if ((speech_chip.read(3) & 0x8000) == 0) {
        std::fprintf(stderr, "FAIL: long C352 speech voice ended early\n");
        return 1;
    }
    speech_output.resize(40000 * 2);
    speech_chip.generate_samples(speech_output.data(), 40000);
    if ((speech_chip.read(3) & 0x8000) != 0) {
        std::fprintf(stderr, "FAIL: completed C352 speech voice remained busy\n");
        return 1;
    }

    // Some System 22 music/effect programs use the C352 rear outputs.  The
    // host stereo mix must preserve those channels rather than silencing them.
    c352_audio rear_chip;
    rear_chip.set_sample_rom(samples.data(), samples.size());
    rear_chip.write(0, 0x0000);  // front pair muted
    rear_chip.write(1, 0xffff);  // rear left/right audible
    rear_chip.write(2, 0xffff);
    rear_chip.write(4, 0x0000);
    rear_chip.write(5, 0x0000);
    rear_chip.write(6, 0x00ff);
    rear_chip.write(7, 0x0000);
    rear_chip.write(3, 0x4006);
    rear_chip.write(0x202, 0);
    std::fill(output.begin(), output.end(), 0);
    rear_chip.generate_samples(output.data(), 1024);
    if (std::all_of(output.begin(), output.end(), [](int16_t value) {
            return value == 0;
        })) {
        std::fprintf(stderr, "FAIL: rear C352 outputs were lost in stereo mix\n");
        return 1;
    }

    std::puts("C352 voice/mixer test passed");
    return 0;
}
