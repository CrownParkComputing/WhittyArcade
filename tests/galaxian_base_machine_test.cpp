// Native Galaxian integration test. The no-ROM section proves the base board
// memory/input/sound register contract; an optional legal local archive then
// drives the real Z80 program through attract mode and a coin/start sequence.

#include "namco/galaxian/galaxian_audio.h"
#include "namco/galaxian/galaxian_machine.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <unordered_set>

int main(int argc, char** argv) {
    {
        auto board = make_galaxian_board_interface();
        galaxian_board_interface* observed = board.get();
        galaxian_machine machine(std::move(board));

        assert(machine.screen_width() == 224);
        assert(machine.screen_height() == 256);
        assert(machine.refresh_rate() > 60.0);
        assert(machine.refresh_rate() < 61.0);

        input_state input;
        input.coin1 = true;
        input.coin2 = true;
        input.start = true;
        input.p2_start = true;
        input.left_stick_x = 0x00;
        input.buttons[0] = true;
        input.p2_stick_x = 0xff;
        input.p2_buttons[0] = true;
        input.test = true;
        input.service = true;
        std::array<uint8_t, 8> ports{};
        observed->apply_input(input, ports.data(), ports.size());

        const uint8_t in0 = observed->cpu_read(0x6000, 0);
        const uint8_t in1 = observed->cpu_read(0x6800, 0);
        assert((in0 & 0x01) != 0);
        assert((in0 & 0x02) != 0);
        assert((in0 & 0x04) != 0);
        assert((in0 & 0x08) == 0);
        assert((in0 & 0x10) != 0);
        assert((in0 & 0x40) != 0);
        assert((in0 & 0x80) != 0);
        assert((in1 & 0x01) != 0);
        assert((in1 & 0x02) != 0);
        assert((in1 & 0x04) == 0);
        assert((in1 & 0x08) != 0);
        assert((in1 & 0x10) != 0);
        assert(observed->cpu_read(0x7000, 0) == 0x04);
        assert(in0 == ports[0]);
        assert(in1 == ports[1]);

        observed->cpu_write(0x4000, 0x12);
        observed->cpu_write(0x4400, 0x34);  // work RAM mirror
        observed->cpu_write(0x5000, 0x56);
        observed->cpu_write(0x5400, 0x78);  // video RAM mirror
        observed->cpu_write(0x5800, 0x9a);
        observed->cpu_write(0x5900, 0xbc);  // object RAM mirror
        assert(observed->cpu_read(0x4000, 0) == 0x34);
        assert(observed->cpu_read(0x4400, 0) == 0x34);
        assert(observed->cpu_read(0x5000, 0) == 0x78);
        assert(observed->cpu_read(0x5400, 0) == 0x78);
        assert(observed->cpu_read(0x5800, 0) == 0xbc);
        assert(observed->cpu_read(0x5900, 0) == 0xbc);

        std::array<unsigned, 3> writes{};
        machine.set_sound_write_handler(
            [&](unsigned port, uint8_t) {
                if ((port & ~0x03u) == mooncrst_audio_port::lfo_base)
                    ++writes[0];
                else if ((port & ~0x07u) ==
                         mooncrst_audio_port::sound_base)
                    ++writes[1];
                else if (port == mooncrst_audio_port::pitch)
                    ++writes[2];
            });
        observed->cpu_write(0x6002, 1);  // coin lockout, not a sound line
        observed->cpu_write(0x6004, 1);
        observed->cpu_write(0x6803, 1);
        observed->cpu_write(0x7800, 0x80);
        assert(writes[0] == 1 && writes[1] == 1 && writes[2] == 1);
    }

    if (argc < 2) {
        std::puts("Galaxian board contract passed (no ROM integration path)");
        return 0;
    }

    auto machine = std::make_unique<galaxian_machine>(
        make_galaxian_board_interface());
    assert(machine->load_roms(argv[1]));

    auto sound = make_galaxian_sound_synth();
    sound->reset();
    std::size_t sound_line_writes = 0;
    std::size_t pitch_writes = 0;
    std::size_t lfo_writes = 0;
    machine->set_sound_write_handler(
        [&](unsigned port, uint8_t data) {
            sound->write_control(port, data);
            if ((port & ~0x07u) == mooncrst_audio_port::sound_base)
                ++sound_line_writes;
            else if (port == mooncrst_audio_port::pitch)
                ++pitch_writes;
            else if ((port & ~0x03u) == mooncrst_audio_port::lfo_base)
                ++lfo_writes;
        });

    machine->reset();
    std::set<std::size_t> unique_frames;
    std::array<int16_t, 792> audio_samples{};
    int audio_peak = 0;
    std::size_t audible_buffers = 0;
    for (int frame = 0; frame < 1200; ++frame) {
        input_state input;
        input.coin1 = frame >= 300 && frame < 304;
        input.start = frame >= 360 && frame < 364;
        machine->set_input(input);
        machine->run_frame();
        sound->generate(audio_samples.data(),
                        static_cast<int>(audio_samples.size()), 100, 100);

        int buffer_peak = 0;
        for (const int16_t sample : audio_samples) {
            const int magnitude = sample < 0 ? -static_cast<int>(sample) :
                                               static_cast<int>(sample);
            buffer_peak = std::max(buffer_peak, magnitude);
        }
        audio_peak = std::max(audio_peak, buffer_peak);
        if (buffer_peak > 100) ++audible_buffers;

        const uint32_t* pixels = machine->frame_buffer();
        std::size_t hash = 0;
        for (int index = 0;
             index < machine->screen_width() * machine->screen_height();
             ++index)
            hash += pixels[index];
        unique_frames.insert(hash);
    }

    const uint32_t* pixels = machine->frame_buffer();
    std::unordered_set<uint32_t> colors;
    std::size_t visible_pixels = 0;
    for (int index = 0;
         index < machine->screen_width() * machine->screen_height();
         ++index) {
        colors.insert(pixels[index]);
        if (pixels[index] != 0xff000000u) ++visible_pixels;
    }

    std::printf("Galaxian: frames=%zu colors=%zu visible=%zu\n",
                unique_frames.size(), colors.size(), visible_pixels);
    std::printf("Galaxian sound: lines=%zu pitch=%zu lfo=%zu peak=%d audible=%zu\n",
                sound_line_writes, pitch_writes, lfo_writes, audio_peak,
                audible_buffers);
    assert(colors.size() >= 7);
    assert(visible_pixels > 1000);
    assert(unique_frames.size() > 100);
    assert(sound_line_writes > 0);
    assert(pitch_writes > 0);
    assert(lfo_writes > 0);
    assert(audio_peak > 1000);
    assert(audible_buffers > 10);
    return 0;
}
