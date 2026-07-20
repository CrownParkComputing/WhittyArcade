#include "musashi_memory.h"

namespace {
thread_local musashi_memory* active_cpu = nullptr;
}

void set_active_musashi_memory(musashi_memory* memory) {
    active_cpu = memory;
}

extern "C" unsigned int m68k_read_memory_8(unsigned int address) {
    return active_cpu ? active_cpu->read8(address) : 0;
}

extern "C" unsigned int m68k_read_memory_16(unsigned int address) {
    return active_cpu ? active_cpu->read16(address) : 0;
}

extern "C" unsigned int m68k_read_memory_32(unsigned int address) {
    return active_cpu ? active_cpu->read32(address) : 0;
}

extern "C" unsigned int m68k_read_disassembler_8(unsigned int address) {
    return m68k_read_memory_8(address);
}

extern "C" unsigned int m68k_read_disassembler_16(unsigned int address) {
    return m68k_read_memory_16(address);
}

extern "C" unsigned int m68k_read_disassembler_32(unsigned int address) {
    return m68k_read_memory_32(address);
}

extern "C" void m68k_write_memory_8(unsigned int address,
                                      unsigned int value) {
    if (active_cpu) active_cpu->write8(address, static_cast<uint8_t>(value));
}

extern "C" void m68k_write_memory_16(unsigned int address,
                                       unsigned int value) {
    if (active_cpu) active_cpu->write16(address, static_cast<uint16_t>(value));
}

extern "C" void m68k_write_memory_32(unsigned int address,
                                       unsigned int value) {
    if (active_cpu) active_cpu->write32(address, value);
}
