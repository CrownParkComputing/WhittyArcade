#include "capcom/gng/gng_machine.h"
#include "capcom/gng/gng_controls.h"
#include "capcom/gng/gng_rom.h"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    assert(gng::machine::width == 256);
    assert(gng::machine::height == 224);
    assert(gng::machine::cycles_per_frame == 100608);
    assert(gng::encode_player_port(0xb7, 0x7f, false, false) == 0xfe);
    assert(gng::encode_player_port(0x47, 0x7f, false, false) == 0xfd);
    assert(gng::encode_player_port(0x7f, 0xb7, false, false) == 0xfb);
    assert(gng::encode_player_port(0x7f, 0x47, false, false) == 0xf7);
    assert(gng::rom_loader::identify_set("/does/not/exist") ==
           gng::rom_set::unknown);
    if (argc < 2) return 0;

    const gng::rom_load_result loaded = gng::rom_loader::load(argv[1]);
    if (!loaded) {
        std::cerr << loaded.error << '\n';
        return 1;
    }
    assert(loaded.set == gng::rom_set::world_set_1);
    gng::machine machine;
    assert(machine.initialize(loaded.data));
    bool sound_reset_asserted = true;
    bool sound_reset_released = false;
    unsigned sound_commands = 0;
    machine.set_sound_callbacks(
        [&](uint8_t) { ++sound_commands; },
        [&](bool asserted) {
            sound_reset_asserted = asserted;
            if (!asserted) sound_reset_released = true;
        });
    machine.set_inputs(0xff, 0xff, 0xff, 0xdf, 0xfb);
    for (int frame = 0; frame < 600; ++frame) machine.run_frame();
    assert(sound_commands != 0);
    if (machine.stopped()) {
        std::cerr << "MC6809 stopped at PC " << std::hex
                  << machine.program_counter() << '\n';
        return 1;
    }
    assert(!sound_reset_asserted && sound_reset_released);
    // Exercise the active-low cabinet inputs through the real attract loop:
    // coin pulse, then start, then enough frames to enter live gameplay.
    machine.set_inputs(0xbf, 0xff, 0xff, 0xdf, 0xfb);
    machine.run_frame();
    machine.set_inputs(0xff, 0xff, 0xff, 0xdf, 0xfb);
    for (int frame = 0; frame < 30; ++frame) machine.run_frame();
    machine.set_inputs(0xfe, 0xff, 0xff, 0xdf, 0xfb);
    machine.run_frame();
    machine.set_inputs(0xff, 0xff, 0xff, 0xdf, 0xfb);
    for (int frame = 0; frame < 600; ++frame) machine.run_frame();
    std::size_t colored = 0;
    for (uint32_t pixel : machine.framebuffer())
        if ((pixel & 0x00ffffffu) != 0) ++colored;
    assert(colored > 10000);
    if (argc >= 3) {
        std::ofstream image(argv[2], std::ios::binary);
        image << "P6\n" << gng::machine::width << ' '
              << gng::machine::height << "\n255\n";
        for (uint32_t pixel : machine.framebuffer()) {
            const char rgb[3]{static_cast<char>(pixel),
                              static_cast<char>(pixel >> 8),
                              static_cast<char>(pixel >> 16)};
            image.write(rgb, sizeof(rgb));
        }
    }
    std::cout << "gng booted, accepted coin/start and ran gameplay; pc="
              << std::hex
              << machine.program_counter() << ", colored=" << std::dec
              << colored << '\n';
    return 0;
}
