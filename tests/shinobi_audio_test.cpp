#include "shinobi_audio.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

namespace {

void write_register(shinobi_sound_synth& synth, uint8_t address,
                    uint8_t value) {
    synth.write_control(0, address);
    synth.write_control(1, value);
}

}  // namespace

int main() {
    shinobi_sound_synth synth;
    synth.reset();

    // Shinobi's Z80 waits on YM2151 timer A at start-up. Verify that the
    // host timer expires, that register 0x14 clears its status immediately,
    // and that the running timer subsequently expires again.
    write_register(synth, 0x10, 0x7d);
    write_register(synth, 0x11, 0x00);
    write_register(synth, 0x14, 0x15);  // load/enable A + clear status A
    assert((synth.read_status() & 0x01) == 0);

    std::vector<int16_t> samples(1200 * 2);
    synth.advance_timer_clocks(600 * 64);
    assert((synth.read_status() & 0x01) != 0);

    write_register(synth, 0x14, 0x15);
    assert((synth.read_status() & 0x01) == 0);
    synth.advance_timer_clocks(100 * 64);
    assert((synth.read_status() & 0x01) == 0);
    synth.advance_timer_clocks(500 * 64);
    assert((synth.read_status() & 0x01) != 0);

    // Exercise the complete register-to-sample path with a simple audible
    // four-operator voice. This catches a silent YM output independently of
    // OpenAL and the game program.
    synth.reset();
    write_register(synth, 0x20, 0xc7);  // channel 0, stereo, algorithm 7
    write_register(synth, 0x28, 0x4a);  // key code
    for (uint8_t op : {uint8_t{0}, uint8_t{8}, uint8_t{16}, uint8_t{24}}) {
        write_register(synth, static_cast<uint8_t>(0x40 + op), 0x01);
        write_register(synth, static_cast<uint8_t>(0x60 + op), 0x00);
        write_register(synth, static_cast<uint8_t>(0x80 + op), 0x1f);
        write_register(synth, static_cast<uint8_t>(0xa0 + op), 0x00);
        write_register(synth, static_cast<uint8_t>(0xc0 + op), 0x00);
        write_register(synth, static_cast<uint8_t>(0xe0 + op), 0x0f);
    }
    write_register(synth, 0x08, 0x78);  // key on all operators, channel 0
    std::fill(samples.begin(), samples.end(), 0);
    synth.generate(samples.data(), 1200);
    assert(std::any_of(samples.begin(), samples.end(),
                       [](int16_t sample) { return sample != 0; }));
    return 0;
}
