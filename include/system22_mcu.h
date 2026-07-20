// system22_mcu.h - Namco C74 sound MCU integration.
#pragma once

#include "m37710.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class audio_system;
class system22_bus;

class system22_c74_mcu {
public:
    system22_c74_mcu(system22_bus& bus, audio_system& audio);

    bool initialize(const uint8_t* firmware, std::size_t firmware_size,
                    const uint8_t* data_rom, std::size_t data_rom_size);
    void control(uint8_t value);
    int execute(int cycles);

    bool enabled() const { return m_enabled; }
    uint32_t program_counter() const { return m_cpu.program_counter(); }
    uint64_t c352_read_count() const { return m_c352_read_count; }
    uint64_t c352_write_count() const { return m_c352_write_count; }

private:
    uint8_t read_byte(uint32_t address) const;
    void write_byte(uint32_t address, uint8_t value);
    uint16_t read_word(uint32_t address) const;
    void write_word(uint32_t address, uint16_t value);
    void write_c352_byte(uint32_t byte_offset, uint8_t value);
    void write_c352_word(uint32_t byte_offset, uint16_t value);

    system22_bus& m_bus;
    audio_system& m_audio;
    m37710_cpu_device m_cpu;
    std::vector<uint8_t> m_data_rom;
    std::array<uint8_t, 0x1000> m_c352_registers{};
    mutable uint64_t m_c352_read_count{0};
    uint64_t m_c352_write_count{0};
    bool m_enabled{false};
    bool m_initialized{false};
};
