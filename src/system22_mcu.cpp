// system22_mcu.cpp - standalone Namco C74 integration.
#include "system22_mcu.h"

#include "system22_audio.h"
#include "system22_cpu.h"

#include <algorithm>

namespace {
constexpr uint32_t C352_BASE = 0x002000;
constexpr uint32_t C352_END = 0x002fff;
constexpr uint32_t SHARED_BASE = 0x004000;
constexpr uint32_t SHARED_END = 0x00bfff;
constexpr uint32_t DATA_ROM_BASE = 0x200000;
constexpr std::size_t DATA_ROM_SIZE = 0x80000;
}

system22_c74_mcu::system22_c74_mcu(system22_bus& bus, audio_system& audio)
    : m_bus(bus), m_audio(audio) {
    m_cpu.set_memory_callbacks(
        [this](uint32_t address) { return read_byte(address); },
        [this](uint32_t address, uint8_t value) { write_byte(address, value); },
        [this](uint32_t address) { return read_word(address); },
        [this](uint32_t address, uint16_t value) { write_word(address, value); });

    m_cpu.set_port_callbacks(
        4,
        [this] { return m_super_system22 ? m_iocontrol : uint8_t{0x10}; },
        [this](uint8_t data) {
            if (m_super_system22) m_iocontrol = data;
        });
    m_cpu.set_port_callbacks(
        5,
        [this] {
            if (!m_super_system22) return uint8_t{0xff};
            const uint16_t inputs = digital_inputs();
            return (m_iocontrol & 0x08) != 0 ?
                static_cast<uint8_t>(inputs) :
                static_cast<uint8_t>(inputs >> 8);
        },
        [this](uint8_t data) {
            if (m_super_system22) m_output_data = data;
        });
    m_cpu.set_port_callbacks(6, [] { return uint8_t{0}; });
    for (unsigned channel = 0; channel < 4; ++channel)
        m_cpu.set_analog_callback(channel, [] { return uint16_t{0}; });
}

void system22_c74_mcu::reset_runtime_state() {
    std::fill(m_c352_registers.begin(), m_c352_registers.end(), 0);
    m_c352_read_count = 0;
    m_c352_write_count = 0;
    m_inputs = input_state{};
    m_iocontrol = 0;
    m_output_data = 0;
    m_enabled = false;
}

bool system22_c74_mcu::initialize(const uint8_t* firmware,
                                  std::size_t firmware_size,
                                  const uint8_t* data_rom,
                                  std::size_t data_rom_size) {
    if (!m_cpu.load_internal_rom(firmware, firmware_size) ||
        !data_rom || data_rom_size != DATA_ROM_SIZE)
        return false;

    m_super_system22 = false;
    m_data_rom.assign(data_rom, data_rom + data_rom_size);
    reset_runtime_state();
    m_initialized = true;
    return true;
}

bool system22_c74_mcu::initialize_super_system22(
        const uint8_t* data_rom, std::size_t data_rom_size) {
    if (!data_rom || data_rom_size != DATA_ROM_SIZE ||
        !m_cpu.load_internal_rom(data_rom + 0xc000, 0x4000))
        return false;

    m_super_system22 = true;
    m_data_rom.assign(data_rom, data_rom + data_rom_size);
    reset_runtime_state();
    m_initialized = true;
    return true;
}

void system22_c74_mcu::control(uint8_t value) {
    const bool enable = value != 0;
    if (enable && !m_enabled && m_initialized) m_cpu.reset();
    m_enabled = enable && m_initialized;
}

int system22_c74_mcu::execute(int cycles) {
    return m_enabled ? m_cpu.execute(cycles) : 0;
}

uint16_t system22_c74_mcu::digital_inputs() const {
    uint16_t value = 0xffff;
    if (m_inputs.coin1) value &= ~0x0001u;
    if (m_inputs.service) value &= ~0x0004u;
    if (m_inputs.test) value &= ~0x0008u;
    if (m_inputs.buttons[0]) value &= ~0x0010u; // gun trigger
    if (m_inputs.buttons[1]) value &= ~0x0020u; // foot pedal
    return value;
}

void system22_c74_mcu::signal_irq0() {
    if (m_enabled && m_super_system22)
        m_cpu.set_input(M37710_LINE_IRQ0, true);
}

void system22_c74_mcu::signal_irq2() {
    if (m_enabled && m_super_system22)
        m_cpu.set_input(M37710_LINE_IRQ2, true);
}

uint8_t system22_c74_mcu::read_byte(uint32_t address) const {
    address &= 0x00ffffff;
    if (address >= C352_BASE && address <= C352_END) {
        ++m_c352_read_count;
        const uint32_t byte_offset = address - C352_BASE;
        const uint16_t word = m_audio.read_c352(
            static_cast<uint16_t>(byte_offset >> 1));
        return static_cast<uint8_t>((byte_offset & 1) ? (word >> 8) : word);
    }
    if (address >= SHARED_BASE && address <= SHARED_END)
        return m_bus.read_c74_shared_byte(address - SHARED_BASE);
    if (address >= DATA_ROM_BASE && address < DATA_ROM_BASE + m_data_rom.size())
        return m_data_rom[address - DATA_ROM_BASE];
    return 0xff;
}

void system22_c74_mcu::write_byte(uint32_t address, uint8_t value) {
    address &= 0x00ffffff;
    if (address >= C352_BASE && address <= C352_END) {
        write_c352_byte(address - C352_BASE, value);
    } else if (address >= SHARED_BASE && address <= SHARED_END) {
        m_bus.write_c74_shared_byte(address - SHARED_BASE, value);
    }
}

uint16_t system22_c74_mcu::read_word(uint32_t address) const {
    address &= 0x00ffffff;
    if (address >= C352_BASE && address + 1 <= C352_END) {
        ++m_c352_read_count;
        return m_audio.read_c352(
            static_cast<uint16_t>((address - C352_BASE) >> 1));
    }
    return static_cast<uint16_t>(read_byte(address) |
                                 (static_cast<uint16_t>(read_byte(address + 1)) << 8));
}

void system22_c74_mcu::write_word(uint32_t address, uint16_t value) {
    address &= 0x00ffffff;
    if (address >= C352_BASE && address + 1 <= C352_END) {
        write_c352_word(address - C352_BASE, value);
    } else if (address >= SHARED_BASE && address + 1 <= SHARED_END) {
        m_bus.write_c74_shared_byte(address - SHARED_BASE,
                                    static_cast<uint8_t>(value));
        m_bus.write_c74_shared_byte(address + 1 - SHARED_BASE,
                                    static_cast<uint8_t>(value >> 8));
    }
}

void system22_c74_mcu::write_c352_byte(uint32_t byte_offset, uint8_t value) {
    if (byte_offset >= m_c352_registers.size()) return;
    m_c352_registers[byte_offset] = value;
    const uint32_t base = byte_offset & ~1u;
    uint16_t word = m_audio.read_c352(static_cast<uint16_t>(base >> 1));
    if ((byte_offset & 1) != 0)
        word = static_cast<uint16_t>((word & 0x00ff) |
                                     (static_cast<uint16_t>(value) << 8));
    else
        word = static_cast<uint16_t>((word & 0xff00) | value);
    m_audio.write_c352(static_cast<uint16_t>(base >> 1), word);
    ++m_c352_write_count;
}

void system22_c74_mcu::write_c352_word(uint32_t byte_offset, uint16_t value) {
    if (byte_offset + 1 >= m_c352_registers.size()) return;
    m_c352_registers[byte_offset] = static_cast<uint8_t>(value);
    m_c352_registers[byte_offset + 1] = static_cast<uint8_t>(value >> 8);
    m_audio.write_c352(static_cast<uint16_t>(byte_offset >> 1), value);
    ++m_c352_write_count;
}
