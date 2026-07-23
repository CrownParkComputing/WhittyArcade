#include "sega/model1/model1_io_board.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
    std::vector<uint8_t> firmware(0x10000, 0x00);
    // Drive the 315-5338A exactly as Sega's firmware does: set DPRAM address
    // 0x0034, place 0xab in the serial latch, then issue command 7 (write).
    const std::array<uint8_t, 44> program{{
        0xdd, 0x21, 0x00, 0x80,       // ld ix,8000
        0xdd, 0x36, 0x0a, 0x34,       // ld (ix+0a),34
        0xdd, 0x36, 0x09, 0x00,       // ld (ix+09),00
        0xdd, 0x36, 0x0a, 0x00,       // ld (ix+0a),00
        0xdd, 0x36, 0x09, 0x01,       // ld (ix+09),01
        0xdd, 0x36, 0x0a, 0xab,       // ld (ix+0a),ab
        0xdd, 0x36, 0x09, 0x07,       // ld (ix+09),07
        0x76,                           // halt
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00,
    }};
    std::copy(program.begin(), program.end(), firmware.begin());
    std::vector<uint8_t> dpram(0x800, 0);
    std::array<uint8_t, 0x80> eeprom{};
    model1_io_board board(model1_io_board_type::standard_315_5338a,
                          firmware, dpram, eeprom);
    board.execute(2000);
    if (!board.active() || dpram[0x34] != 0xab) {
        std::fprintf(stderr,
                     "Model 1 I/O transaction failed: active=%d value=%02x pc=%04x\n",
                     board.active(), dpram[0x34], board.program_counter());
        return 1;
    }
    return 0;
}
