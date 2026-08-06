// m68000_cpu dual-instance test.
//
// Musashi's process-global register file made a second 68000 impossible;
// this test runs two m68000_cpu objects side by side on independent RAM
// buses and verifies that neither sees the other's state. It also checks
// autovectored level-4 interrupts, which System 16 boards use for vblank.
#include "m68000_cpu.h"

#include <cassert>
#include <cstdio>
#include <vector>

namespace {

class ram_bus final : public m68000_bus {
public:
    std::vector<uint8_t> ram = std::vector<uint8_t>(0x10000, 0);

    uint8_t read8(uint32_t address) override {
        return ram[address & 0xffff];
    }
    uint16_t read16(uint32_t address) override {
        return static_cast<uint16_t>((read8(address) << 8) |
                                     read8(address + 1));
    }
    void write8(uint32_t address, uint8_t value) override {
        ram[address & 0xffff] = value;
    }
    void write16(uint32_t address, uint16_t value) override {
        write8(address, static_cast<uint8_t>(value >> 8));
        write8(address + 1, static_cast<uint8_t>(value));
    }

    void put16(uint32_t address, uint16_t value) { write16(address, value); }
    void put32(uint32_t address, uint32_t value) {
        put16(address, static_cast<uint16_t>(value >> 16));
        // The low half goes two bytes along, not on top of the high half.
        // Writing both to the same address left the reset vector reading
        // back as 0x00080000 instead of 0x00000008 - so every run of this
        // test started the CPU at half a megabyte and asserted on the first
        // line, and the fault was in the test's own bus rather than in the
        // 68000 it was testing.
        put16(address + 2, static_cast<uint16_t>(value));
    }
    uint16_t peek16(uint32_t address) { return read16(address); }
};

// Program layout per instance:
//   0x00: SSP = 0x00010000, PC = 0x00000008
//   0x08: move.w #imm,d0        303C imm
//   0x0C: move.w d0,(0x200).w   31C0 0200
//   0x10: andi.w #0xF000,sr     027C F000   (lower IPL mask so L4 fires)
//   0x14: bra.s -2              60FE
// Level-4 autovector (vector 28, offset 0x70) -> handler at 0x20:
//   0x20: move.w d0,(0x202).w   31C0 0202
//   0x24: rte                   4E73
void load_program(ram_bus& bus, uint16_t imm) {
    bus.put32(0x00, 0x00010000);
    bus.put32(0x04, 0x00000008);
    bus.put16(0x08, 0x303c); bus.put16(0x0a, imm);
    // 31c0, not 33c0. 0x33C0 is move.w d0,(xxx).L - absolute long, six
    // bytes and a 32-bit address - so this wrote d0 to whatever address the
    // next two words happened to spell, fell through into the middle of the
    // following instruction, and ended up executing the vector table. The
    // absolute-short form the comment describes is 0x31C0.
    bus.put16(0x0c, 0x31c0); bus.put16(0x0e, 0x0200);
    bus.put16(0x10, 0x027c); bus.put16(0x12, 0xf000);
    bus.put16(0x14, 0x60fe);
    bus.put32(0x70, 0x00000020);
    bus.put16(0x20, 0x31c0); bus.put16(0x22, 0x0202);
    bus.put16(0x24, 0x4e73);
}

}  // namespace

int main() {
    ram_bus bus_a, bus_b;
    load_program(bus_a, 0x1234);
    load_program(bus_b, 0x5678);

    m68000_cpu cpu_a(bus_a), cpu_b(bus_b);
    cpu_a.reset();
    cpu_b.reset();

    assert(cpu_a.program_counter() == 0x08);
    assert(cpu_b.program_counter() == 0x08);
    assert((cpu_a.status_register() & 0x0700) == 0x0700); // post-reset mask

    cpu_a.execute(200);
    assert(bus_a.peek16(0x200) == 0x1234);
    assert(bus_b.peek16(0x200) == 0x0000); // untouched: no cross-talk

    cpu_b.execute(200);
    assert(bus_b.peek16(0x200) == 0x5678);
    assert(bus_a.peek16(0x200) == 0x1234);

    // IRQ independence: raising level 4 on A must not affect B.
    cpu_a.set_irq(4);
    cpu_a.execute(200);
    cpu_a.set_irq(0);
    assert(bus_a.peek16(0x202) == 0x1234);
    assert(bus_b.peek16(0x202) == 0x0000);
    cpu_b.execute(200);
    assert(bus_b.peek16(0x202) == 0x0000); // B had no interrupt pending

    // Register independence.
    assert(cpu_a.data_register(0) != cpu_b.data_register(0));

    std::puts("m68000_cpu_test: dual instances are fully independent");
    return 0;
}
