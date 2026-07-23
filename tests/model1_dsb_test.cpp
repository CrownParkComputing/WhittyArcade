#include "sega/model1/model1_dsb.h"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    std::vector<uint8_t> program(0x20000, 0);
    // Set start=0, end=0x100 and issue a one-shot play command entirely via
    // the emulated Z80 I/O bus.
    const uint8_t code[] = {
        0x3e, 0x00, 0xd3, 0xe2, 0xd3, 0xe3, 0xd3, 0xe4,
        0xd3, 0xe5, 0x3e, 0x01, 0xd3, 0xe6, 0x3e, 0x00,
        0xd3, 0xe7, 0x3e, 0x01, 0xd3, 0xe0, 0x76,
    };
    for (std::size_t index = 0; index < sizeof(code); ++index)
        program[index] = code[index];
    std::vector<uint8_t> mpeg(0x1000, 0xff);
    // A byte-aligned Layer-II sync candidate is enough for the register test;
    // decoding is separately exercised by the real Star Wars integration run.
    mpeg[0] = 0xff;
    mpeg[1] = 0xfc;

    model1_dsb board;
    assert(board.initialize(program, mpeg));
    board.execute(1000);
    assert(board.executed_clocks() == 1000);
    assert(board.trigger_count() == 1);
    assert(board.playing());

    board.receive_uart(0x55);
    assert(board.received_bytes() == 1);
    return 0;
}
