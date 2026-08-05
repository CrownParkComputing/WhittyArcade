// Sega System 16B ROM board 171-5797 contract test (no ROMs required).
//
// E-SWAT is the first game here on the 5797, which differs from the 5704
// in how the 315-5195 mapper's low three regions are wired: one 512 KiB
// program window instead of two 256 KiB ones, a 16 KiB "bank/math" window
// carrying a 315-5248 multiplier, a 315-5250 compare/timer and the tile
// bank register, and a second 315-5250 in its own region.
//
// Everything below drives the board through the same byte bus the 68000
// uses, so it exercises the real mapper decode rather than the chips in
// isolation.

#include "sega/system16b/system16b_machine.h"
#include "sega/system16b/system16b_rom.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

// Region bases this test programs into the mapper, matching the layout
// E-SWAT itself programs: region 0 = ROM at 0x000000 (512 KiB), region 1 =
// bank/math at 0x3e0000 (64 KiB), region 2 = the second compare/timer.
constexpr uint32_t kBankMathBase  = 0x3e0000;
constexpr uint32_t kCmpTimer2Base = 0x300000;

// 315-5195 region control: low two bits select the region size, and the
// following register holds the base in 64 KiB units.
void map_region(system16b::board& board, unsigned region, uint8_t size_code,
                uint32_t base) {
    board.mapper_regs_[0x10 + 2 * region] = size_code;
    board.mapper_regs_[0x11 + 2 * region] =
        static_cast<uint8_t>((base >> 16) & 0xff);
}

uint16_t read_word(system16b::board& board, uint32_t address) {
    return static_cast<uint16_t>(board.cpu_read_16(address));
}

void write_word(system16b::board& board, uint32_t address, uint16_t value) {
    board.cpu_write_16(address, value);
}

}  // namespace

int main() {
    system16b::board board;
    // set_game installs the 5797 region layout; reset_extras clears the
    // custom chips, exactly as the machine runtime sequences them.
    board.set_game(system16b::system16b_rom_set::eswat);
    board.reset_extras();

    map_region(board, 0, 0x02, 0x000000);  // 512 KiB program window
    map_region(board, 1, 0x00, kBankMathBase);
    map_region(board, 2, 0x00, kCmpTimer2Base);

    // ---- 315-5248 multiplier ----------------------------------------
    // Two signed 16-bit operands at word offsets 0 and 1; offsets 2 and 3
    // read back the high and low halves of the signed product.
    write_word(board, kBankMathBase + 0x0000, 0x0007);
    write_word(board, kBankMathBase + 0x0002, 0x0006);
    assert(read_word(board, kBankMathBase + 0x0000) == 0x0007);
    assert(read_word(board, kBankMathBase + 0x0002) == 0x0006);
    assert(read_word(board, kBankMathBase + 0x0004) == 0x0000);  // 42 >> 16
    assert(read_word(board, kBankMathBase + 0x0006) == 0x002a);  // 42 & 0xffff

    // The operands are signed, so a negative multiplicand must sign-extend
    // into the high half rather than being treated as a large positive.
    write_word(board, kBankMathBase + 0x0000, 0xffff);           // -1
    write_word(board, kBankMathBase + 0x0002, 0x0002);           //  2
    assert(read_word(board, kBankMathBase + 0x0004) == 0xffff);  // -2 >> 16
    assert(read_word(board, kBankMathBase + 0x0006) == 0xfffe);  // -2 low

    // A full-width product must not be truncated on the way through.
    write_word(board, kBankMathBase + 0x0000, 0x4000);
    write_word(board, kBankMathBase + 0x0002, 0x0004);
    assert(read_word(board, kBankMathBase + 0x0004) == 0x0001);  // 0x10000
    assert(read_word(board, kBankMathBase + 0x0006) == 0x0000);

    // ---- 315-5250 compare/timer -------------------------------------
    // regs 0 and 1 are the bounds in either order, reg 2 the value under
    // test. Writing the value clamps it into reg 7 and reports which side
    // it fell off in reg 3.
    const uint32_t cmp = kBankMathBase + 0x1000;
    write_word(board, cmp + 0x0000, 0x0010);   // bound1 = 16
    write_word(board, cmp + 0x0002, 0x0020);   // bound2 = 32
    write_word(board, cmp + 0x0004, 0x0018);   // value  = 24, inside
    assert(read_word(board, cmp + 0x000e) == 0x0018);  // reg7 = value
    assert(read_word(board, cmp + 0x0006) == 0x0000);  // reg3 = in range

    write_word(board, cmp + 0x0004, 0x0008);   // below the range
    assert(read_word(board, cmp + 0x000e) == 0x0010);  // clamped to min
    assert(read_word(board, cmp + 0x0006) == 0x8000);

    write_word(board, cmp + 0x0004, 0x0040);   // above the range
    assert(read_word(board, cmp + 0x000e) == 0x0020);  // clamped to max
    assert(read_word(board, cmp + 0x0006) == 0x4000);

    // Bounds are compared as signed values and may arrive in either order.
    write_word(board, cmp + 0x0000, 0x0020);
    write_word(board, cmp + 0x0002, 0xfff0);   // -16
    write_word(board, cmp + 0x0004, 0xffff);   // -1, inside [-16, 32]
    assert(read_word(board, cmp + 0x0006) == 0x0000);
    write_word(board, cmp + 0x0004, 0xffe0);   // -32, below
    assert(read_word(board, cmp + 0x000e) == 0xfff0);
    assert(read_word(board, cmp + 0x0006) == 0x8000);

    // Reg 4 accumulates one history bit per compare, LSB first, and a
    // write to it restarts the sequence.
    write_word(board, cmp + 0x0008, 0x0000);   // reset the bit history
    assert(read_word(board, cmp + 0x0008) == 0x0000);
    write_word(board, cmp + 0x0000, 0x0000);
    write_word(board, cmp + 0x0002, 0x0010);
    write_word(board, cmp + 0x0004, 0x0008);   // in range  -> bit 0 = 1
    write_word(board, cmp + 0x0004, 0x00ff);   // out       -> bit 1 = 0
    write_word(board, cmp + 0x0004, 0x0004);   // in range  -> bit 2 = 1
    assert(read_word(board, cmp + 0x0008) == 0x0005);
    write_word(board, cmp + 0x0008, 0x0000);
    assert(read_word(board, cmp + 0x0008) == 0x0000);

    // Offsets 5 and 6 alias the bound and value registers rather than
    // reading storage of their own.
    assert(read_word(board, cmp + 0x000a) == read_word(board, cmp + 0x0002));
    assert(read_word(board, cmp + 0x000c) == read_word(board, cmp + 0x0004));

    // ---- tile bank register -----------------------------------------
    // The board decodes it at bank/math sub-offset 0x2000, low byte only,
    // so the two tilemap slots sit at 0x2001 and 0x2003 and take three
    // bits of page number each.
    board.byte_write(kBankMathBase + 0x2001, 0x05);
    board.byte_write(kBankMathBase + 0x2003, 0x02);
    assert(board.tile_banks_[0] == 5);
    assert(board.tile_banks_[1] == 2);
    // Only three bits are wired.
    board.byte_write(kBankMathBase + 0x2001, 0xff);
    assert(board.tile_banks_[0] == 7);
    // The even byte carries no data, so it must not disturb a slot.
    board.byte_write(kBankMathBase + 0x2002, 0x01);
    assert(board.tile_banks_[1] == 2);

    // ---- region 2: the board's second compare/timer -------------------
    // It is a genuinely separate chip: driving it must not disturb the
    // first one's registers.
    write_word(board, cmp + 0x0000, 0x0064);
    write_word(board, cmp + 0x0002, 0x00c8);
    write_word(board, kCmpTimer2Base + 0x0000, 0x0001);
    write_word(board, kCmpTimer2Base + 0x0002, 0x0003);
    write_word(board, kCmpTimer2Base + 0x0004, 0x0009);   // above [1, 3]
    assert(read_word(board, kCmpTimer2Base + 0x000e) == 0x0003);
    assert(read_word(board, kCmpTimer2Base + 0x0006) == 0x4000);
    assert(read_word(board, cmp + 0x0000) == 0x0064);
    assert(read_word(board, cmp + 0x0002) == 0x00c8);

    // ---- region 0 is one 512 KiB program window ----------------------
    // On the 5704 the second 256 KiB of the program is reached through
    // region 1; on the 5797 it must be visible in region 0, which is what
    // makes E-SWAT's upper half executable at all.
    board.program_rom()[0x00002] = 0xa5;
    board.program_rom()[0x7fffe] = 0x5a;
    assert(board.byte_read(0x000002) == 0xa5);
    assert(board.byte_read(0x07fffe) == 0x5a);

    // ---- reset clears the custom chips --------------------------------
    // reset_extras also zeroes the mapper register file, so the regions
    // have to be programmed again before the chips are reachable.
    board.reset_extras();
    map_region(board, 0, 0x02, 0x000000);
    map_region(board, 1, 0x00, kBankMathBase);
    map_region(board, 2, 0x00, kCmpTimer2Base);
    assert(read_word(board, kBankMathBase + 0x0006) == 0x0000);  // product
    assert(read_word(board, cmp + 0x000e) == 0x0000);
    assert(read_word(board, kCmpTimer2Base + 0x0006) == 0x0000);

    std::printf("system16b_5797_test: all checks passed\n");
    return 0;
}
