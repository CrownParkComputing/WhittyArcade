// galaxian_phoenix_machine_test - native board integration test for Phoenix.
//
// Drives the shared galaxian_machine constructed with the phoenix
// board_interface through a full frame loop, asserts the screen
// dimensions, refresh rate, and (with a real archive) that the
// produced frame contains a meaningful number of distinct colors
// and visible pixels.

#include "galaxian_machine.h"

#include <cassert>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <unordered_set>

// Factory lives in galaxian_machine_phoenix.cpp; declared here so the
// test can construct the board without exposing the class itself.
std::unique_ptr<galaxian_board_interface>
make_phoenix_board_interface();

int main(int argc, char** argv) {
    // Input translation must update the byte the emulated CPU reads, not only
    // the caller's diagnostic port array.
    {
        auto board = make_phoenix_board_interface();
        input_state input;
        input.coin1 = true;
        input.start = true;
        input.p2_start = true;
        input.left_stick_x = 0x00;
        input.buttons[0] = true;
        input.buttons[1] = true;
        std::array<uint8_t, 8> ports{};
        board->apply_input(input, ports.data(), ports.size());
        const uint8_t cpu_port = board->cpu_read(0x7000, 0);
        assert((cpu_port & 0x01) == 0);
        assert((cpu_port & 0x02) == 0);
        assert((cpu_port & 0x04) == 0);
        assert((cpu_port & 0x10) == 0);
        assert((cpu_port & 0x40) == 0);
        assert((cpu_port & 0x80) == 0);
        assert((cpu_port & 0x20) != 0);
        assert(cpu_port == ports[0]);

        input = input_state{};
        input.coin2 = true;  // Phoenix has no second coin line.
        input.left_stick_x = 0xff;
        board->apply_input(input, ports.data(), ports.size());
        const uint8_t right_port = board->cpu_read(0x7000, 0);
        assert((right_port & 0x20) == 0);
        assert((right_port & 0x04) != 0);
    }
    auto machine = std::make_unique<galaxian_machine>(
        make_phoenix_board_interface());

    assert(machine->screen_width() == 208);
    assert(machine->screen_height() == 256);
    assert(machine->refresh_rate() > 61.0);
    assert(machine->refresh_rate() < 61.1);

    if (argc < 2) {
        std::puts("Phoenix board constants passed (no ROM integration path)");
        return 0;
    }

    assert(machine->load_roms(argv[1]));
    machine->reset();
    for (int frame = 0; frame < 180; ++frame) machine->run_frame();

    const uint32_t* pixels = machine->frame_buffer();
    std::unordered_set<uint32_t> colors;
    std::size_t visible_pixels = 0;
    for (int index = 0;
         index < machine->screen_width() * machine->screen_height();
         ++index) {
        colors.insert(pixels[index]);
        if (pixels[index] != 0xff000000u) ++visible_pixels;
    }
    std::printf("Phoenix frame: %zu colors, %zu non-black pixels\n",
                colors.size(), visible_pixels);
    if (argc >= 3) {
        if (std::FILE* image = std::fopen(argv[2], "wb")) {
            std::fprintf(image, "P6\n%d %d\n255\n",
                         machine->screen_width(),
                         machine->screen_height());
            const uint8_t* rgba = reinterpret_cast<const uint8_t*>(pixels);
            for (int index = 0;
                 index < machine->screen_width() * machine->screen_height();
                 ++index)
                std::fwrite(rgba + index * 4, 3, 1, image);
            std::fclose(image);
        }
    }
    assert(colors.size() >= 7);
    assert(visible_pixels > 1000);
    return 0;
}
