#include "i960.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
void write32(std::vector<uint8_t>& memory, uint32_t address, uint32_t value) {
    memory[address] = static_cast<uint8_t>(value);
    memory[address + 1] = static_cast<uint8_t>(value >> 8);
    memory[address + 2] = static_cast<uint8_t>(value >> 16);
    memory[address + 3] = static_cast<uint8_t>(value >> 24);
}
} // namespace

int main() {
    std::vector<uint8_t> memory(0x1000, 0);
    write32(memory, 0x000, 0x00000100); // system address table
    write32(memory, 0x004, 0x00000200); // processor control block
    write32(memory, 0x00c, 0x00000300); // initial instruction pointer
    write32(memory, 0x218, 0x00000400); // initial frame pointer

    // lda 0x123,g0; addo 5,7,g1; b .
    write32(memory, 0x300, 0x8c800123);
    write32(memory, 0x304,
            0x59000000u | (17u << 19) | (7u << 14) |
            0x00001000u | 0x00000800u | 5u);
    write32(memory, 0x308, 0x08000000);

    i80960kb_device cpu;
    cpu.set_program_callbacks(
        [&memory](uint32_t address) {
            return address < memory.size() ? memory[address] : uint8_t{0};
        },
        [&memory](uint32_t address, uint8_t value) {
            if (address < memory.size()) memory[address] = value;
        });
    cpu.standalone_start();

    assert(cpu.program_counter() == 0x300);
    assert(cpu.register_value(I960_FP) == 0x400);
    assert(cpu.register_value(I960_SP) == 0x440);
    const int executed = cpu.standalone_execute(32);
    assert(executed >= 32);
    assert(cpu.register_value(I960_G0) == 0x123);
    assert(cpu.register_value(I960_G1) == 12);
    assert(cpu.program_counter() == 0x308);

    std::size_t length = 0;
    const std::string instruction = cpu.disassemble(0x300, &length);
    assert(length == 4);
    assert(instruction.find("lda") != std::string::npos);
    std::printf("i960 reset/execute OK: IP=%08x %s\n",
                cpu.program_counter(), instruction.c_str());
    return 0;
}
