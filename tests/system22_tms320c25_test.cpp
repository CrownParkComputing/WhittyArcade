#include "namco/system22/system22_tms320c25.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
void put_word(std::vector<uint8_t>& rom, std::size_t word, uint16_t value) {
    rom[word * 2] = static_cast<uint8_t>(value >> 8);
    rom[word * 2 + 1] = static_cast<uint8_t>(value);
}

bool expect(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}
} // namespace

int main() {
    std::vector<uint8_t> rom(0x2000, 0);

    // lack #$55; sacl $60; out $60,PA3; b $0003
    put_word(rom, 0, 0xca55);
    put_word(rom, 1, 0x6060);
    put_word(rom, 2, 0xe360);
    put_word(rom, 3, 0xff00);
    put_word(rom, 4, 0x0003);

    tms320c2x_device cpu;
    uint16_t output = 0;
    unsigned writes = 0;
    cpu.set_io_callbacks({}, [&](uint16_t port, uint16_t data) {
        if (port == 3) {
            output = data;
            ++writes;
        }
    });

    if (!expect(!cpu.load_internal_rom(rom.data(), rom.size() - 1),
                "C25 internal ROM size must be exact")) return 1;
    if (!expect(cpu.load_internal_rom(rom.data(), rom.size()),
                "complete C25 internal ROM must load")) return 1;
    cpu.reset();
    if (!expect(cpu.execute(64) > 0, "C25 must consume scheduled clocks")) return 1;
    if (!expect(output == 0x55 && writes > 0,
                "C25 must execute data-memory and output instructions")) return 1;
    if (!expect(cpu.program_counter() == 3 || cpu.program_counter() == 4,
                "C25 branch loop must remain in its expected range")) return 1;

    std::puts("TMS320C25 core test passed");
    return 0;
}
