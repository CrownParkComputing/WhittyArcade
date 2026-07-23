// Boots the real Virtua Cop cabinet-I/O firmware (epr-17181) on the advanced
// model1io2 board and checks it reaches and stays in its service loop. The
// full i960<->Z80 dual-port-RAM handshake needs the live main CPU and is
// exercised on the device; this guards the board bring-up on the host.
#include "model1_io_board.h"
#include "model2_rom.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: model1io2_board_test <vcop.zip>\n");
        return 2;
    }
    model2_rom_load_result rom = model2_rom_loader::load(argv[1]);
    if (!rom) { std::fputs(rom.error.c_str(), stderr); return 1; }
    assert(rom.roms.io_cpu.size() == 0x10000);

    std::vector<uint8_t> dpram(0x800, 0);
    std::array<uint8_t, 0x80> eeprom{};
    eeprom.fill(0xff);
    model1_io_board board(model1_io_board_type::advanced_tmpz84c015,
                          rom.roms.io_cpu, dpram, eeprom);
    assert(board.active());

    input_state input;              // one coin + P1 trigger + a gun position
    input.coin1 = true;
    input.buttons[0] = true;
    input.left_stick_x = 0x90;
    input.left_stick_y = 0x60;

    // ~9.83 MHz TMPZ84C015 => ~163,840 clocks per 60 Hz frame.
    constexpr int frame_clocks = 163840;
    std::array<uint16_t, 240> pc_samples{};
    for (int frame = 0; frame < 240; ++frame) {
        board.set_gun_inputs(input);
        board.execute(frame_clocks);
        pc_samples[frame] = board.program_counter();
    }

    // The firmware must have run and stayed in ROM (0x0000-0x7fff), not run off
    // into I/O space, and it must not be frozen at a single address.
    assert(board.executed_clocks() >= static_cast<uint64_t>(frame_clocks) * 240);
    unsigned in_rom = 0, distinct = 0;
    std::array<uint16_t, 240> seen{};
    for (int frame = 60; frame < 240; ++frame) { // ignore cold-boot frames
        const uint16_t pc = pc_samples[frame];
        if (pc < 0x8000) ++in_rom;
        bool found = false;
        for (unsigned k = 0; k < distinct; ++k) if (seen[k] == pc) { found = true; break; }
        if (!found) seen[distinct++] = pc;
    }
    assert(in_rom == 180);   // every sampled frame is executing firmware ROM
    assert(distinct >= 3);   // the sampled PC moves: not stuck/halted

    std::printf("model1io2 board ran: clocks=%llu final_pc=0x%04x distinct_pc=%u\n",
                static_cast<unsigned long long>(board.executed_clocks()),
                board.program_counter(), distinct);
    return 0;
}
