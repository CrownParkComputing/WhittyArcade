// system22_cpu.cpp - System 22 MC68020 and main bus implementation
#include "system22_cpu.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "m68k.h"
}

namespace {
struct driving_calibration {
    uint16_t steering_offset;
    uint16_t gas_offset;
    uint16_t brake_offset;
};

constexpr driving_calibration RIDGE_RACER_CALIBRATION{0x160, 884, 809};
constexpr driving_calibration RAVE_RACER_CALIBRATION{0x020, 992, 3008};
constexpr driving_calibration ACE_DRIVER_CALIBRATION{0x800, 992, 3008};

bool in_range(uint32_t address, uint32_t base, std::size_t size) {
    return address >= base && static_cast<uint64_t>(address) <
        static_cast<uint64_t>(base) + size;
}
} // namespace

system22_bus::system22_bus()
    : m_program_rom(SUPER_PROGRAM_ROM_SIZE, 0xff),
      m_main_ram(SUPER_MAIN_RAM_SIZE),
      m_sci_ram(SCI_RAM_SIZE),
      m_eeprom(EEPROM_SIZE, 0xff),
      m_mcu_shared(MCU_SHARED_SIZE),
      m_polygon_ram(POLYGON_RAM_SIZE / sizeof(uint32_t)),
      m_czram(CZ_RAM_SIZE),
      m_mixer(MIXER_RAM_SIZE),
      m_paletteram(PALETTE_RAM_SIZE),
      m_cgram(CG_RAM_SIZE),
      m_textram(TEXT_RAM_SIZE),
      m_vics_data(VICS_DATA_SIZE),
      m_vics_control(VICS_CONTROL_SIZE),
      m_sprite_ram(SPRITE_RAM_SIZE) {
}

void system22_bus::set_super_system22(bool enabled) {
    if (m_super_system22 == enabled) return;
    m_super_system22 = enabled;
    m_program_loaded = false;
    std::fill(m_program_rom.begin(), m_program_rom.end(), 0xff);
    std::fill(m_main_ram.begin(), m_main_ram.end(), 0);
    std::fill(m_syscontrol.begin(), m_syscontrol.end(), 0);
    m_irq_enabled = 0;
    m_irq_state = 0;
    m_asserted_irq_level = 0;
    if (m_irq_handler) m_irq_handler(0);
}

void system22_bus::set_irq_handler(std::function<void(int)> handler) {
    m_irq_handler = std::move(handler);
    if (m_irq_handler) m_irq_handler(m_asserted_irq_level);
}

void system22_bus::set_dsp_control_handler(std::function<void(uint8_t)> handler) {
    m_dsp_control_handler = std::move(handler);
    if (m_dsp_control_handler)
        m_dsp_control_handler(m_syscontrol[m_super_system22 ? 0x1c : 0x1a]);
}

void system22_bus::set_mcu_control_handler(std::function<void(uint8_t)> handler) {
    m_mcu_control_handler = std::move(handler);
    if (m_mcu_control_handler)
        m_mcu_control_handler(m_syscontrol[m_super_system22 ? 0x16 : 0x18]);
}

uint8_t system22_bus::read_mcu_shared_byte(std::size_t index) const {
    return index < m_mcu_shared.size() ? m_mcu_shared[index] : 0xff;
}

void system22_bus::write_mcu_shared_byte(std::size_t index, uint8_t value) {
    if (index < m_mcu_shared.size()) m_mcu_shared[index] = value;
}

uint8_t system22_bus::read_c74_shared_byte(std::size_t index) const {
    // m_mcu_shared is stored in the MC68020's big-endian byte order.  The
    // little-endian C74 sees the same u16 words with the byte lane reversed.
    return read_mcu_shared_byte(index ^ 1u);
}

void system22_bus::write_c74_shared_byte(std::size_t index, uint8_t value) {
    write_mcu_shared_byte(index ^ 1u, value);
}

void system22_bus::write_shared_word(std::size_t offset, uint16_t value) {
    write_mcu_shared_byte(offset, static_cast<uint8_t>(value >> 8));
    write_mcu_shared_byte(offset + 1, static_cast<uint8_t>(value));
}

void system22_bus::update_coinage(const input_state& state) {
    if (!m_coin_state_initialized) {
        m_coin1_previous = state.coin1;
        m_coin2_previous = state.coin2;
        m_coin_state_initialized = true;
    } else {
        if (state.coin1 && !m_coin1_previous) ++m_credits1;
        if (state.coin2 && !m_coin2_previous) ++m_credits2;
        m_coin1_previous = state.coin1;
        m_coin2_previous = state.coin2;
    }
    write_shared_word(0x3a, static_cast<uint16_t>(m_credits1 << 8) | m_credits2);
}

void system22_bus::update_driving_inputs(const input_state& state) {
    // Ridge Racer's external control C74 publishes these values to the sound
    // C74/main-CPU shared RAM once per vblank. Switches are active low and the
    // standard cabinet selects configuration bit 8 = 0.
    uint16_t flags = m_driving_profile == system22_driving_profile::ace_driver ?
        0xffff : 0xfeff;
    if (state.shift_down) flags &= ~0x0001u;
    if (state.shift_up) flags &= ~0x0002u;
    if (state.view) flags &= ~0x0040u;
    if (state.coin2) flags &= ~0x0200u;
    if (state.test) flags &= ~0x0400u;
    if (state.service) flags &= ~0x0800u;
    if (state.coin1) flags &= ~0x1000u;

    write_shared_word(0x30, flags);
    // The external control MCU applies cabinet/game-specific calibration
    // constants before publishing its ADC readings. Ridge Racer's EEPROM is
    // centred around 0x960; Rave Racer uses 0x820. Applying one game's value
    // globally causes the other to steer continuously at rest.
    const driving_calibration& calibration = [&]() -> const driving_calibration& {
        switch (m_driving_profile) {
        case system22_driving_profile::rave_racer:
            return RAVE_RACER_CALIBRATION;
        case system22_driving_profile::ace_driver:
            return ACE_DRIVER_CALIBRATION;
        case system22_driving_profile::ridge_racer:
        case system22_driving_profile::cyber_commando:
        case system22_driving_profile::time_crisis:
        case system22_driving_profile::dirt_dash:
        case system22_driving_profile::aqua_jet:
            return RIDGE_RACER_CALIBRATION;
        }
        return RIDGE_RACER_CALIBRATION;
    }();
    uint16_t steering = std::clamp<uint16_t>(state.steering, 0x280, 0xd80);
    uint16_t gas = std::clamp<uint16_t>(state.gas, 0x000, 0x610);
    uint16_t brake = std::clamp<uint16_t>(state.brake, 0x000, 0x610);
    if (m_driving_profile == system22_driving_profile::ace_driver) {
        // Ace Driver cabinets use shorter pedal ranges than Ridge/Rave. The
        // host adapter exposes a common 0..0x610 range, so scale at the board
        // boundary before applying the I/O MCU calibration constants.
        gas = static_cast<uint16_t>(static_cast<uint32_t>(gas) * 0x480 / 0x610);
        brake = static_cast<uint16_t>(static_cast<uint32_t>(brake) * 0x240 / 0x610);
    }
    write_shared_word(0x32, static_cast<uint16_t>(
        steering + calibration.steering_offset));
    write_shared_word(0x34, static_cast<uint16_t>(gas + calibration.gas_offset));
    write_shared_word(0x36, static_cast<uint16_t>(
        brake + calibration.brake_offset));

    update_coinage(state);
}

void system22_bus::update_cyber_commando_inputs(const input_state& state) {
    uint16_t flags = 0xffff;
    if (state.shift_down) flags &= ~0x0001u; // Gun trigger
    if (state.shift_up) flags &= ~0x0002u;   // Missile
    if (state.view) flags &= ~0x0040u;
    if (state.coin2) flags &= ~0x0200u;
    if (state.test) flags &= ~0x0400u;
    if (state.service) flags &= ~0x0800u;
    if (state.coin1) flags &= ~0x1000u;

    write_shared_word(0x30, flags);
    write_shared_word(0x32, static_cast<uint16_t>(state.right_stick_y) << 4);
    write_shared_word(0x34, static_cast<uint16_t>(state.left_stick_y) << 4);
    write_shared_word(0x36, static_cast<uint16_t>(state.right_stick_x) << 4);
    write_shared_word(0x38, static_cast<uint16_t>(state.left_stick_x) << 4);
    update_coinage(state);
}

void system22_bus::update_game_inputs(const input_state& state) {
    if (m_driving_profile == system22_driving_profile::time_crisis)
        update_time_crisis_inputs(state);
    else if (m_driving_profile == system22_driving_profile::dirt_dash ||
             m_driving_profile == system22_driving_profile::aqua_jet)
        return; // These Super System 22 M37710s own their cabinet I/O.
    else if (m_driving_profile == system22_driving_profile::cyber_commando)
        update_cyber_commando_inputs(state);
    else
        update_driving_inputs(state);
}

void system22_bus::update_time_crisis_inputs(const input_state& state) {
    // Keep normal host-axis extremes just inside the calibrated CRT edges.
    // The exact edges mean "off screen" to Time Crisis and are reserved for
    // a future absolute light-gun device path.
    const auto scale = [](uint8_t value, uint16_t low, uint16_t high) {
        constexpr int host_low = 0x47;
        constexpr int host_high = 0xb7;
        const int clamped = std::clamp<int>(value, host_low, host_high);
        return static_cast<uint16_t>(low +
            (clamped - host_low) * (high - low) /
                (host_high - host_low));
    };
    m_gun_x = scale(state.left_stick_x, 69, 693);
    m_gun_y = scale(state.left_stick_y, 44, 283);
}

uint8_t system22_bus::read_cz_byte(std::size_t index) const {
    return index < m_czram.size() ? m_czram[index] : 0;
}

uint16_t system22_bus::read_super_cz_attribute(std::size_t index) const {
    const std::size_t offset = index * 2;
    if (offset + 1 >= m_super_czattr.size()) return 0;
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(m_super_czattr[offset]) << 8) |
        m_super_czattr[offset + 1]);
}

uint16_t system22_bus::read_super_cz_entry(std::size_t bank,
                                           std::size_t index) const {
    if (bank >= 4 || index >= 0x100) return 0;
    const std::size_t offset = bank * 0x200 + index * 2;
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(m_czram[offset]) << 8) |
        m_czram[offset + 1]);
}

uint8_t system22_bus::read_mixer_byte(std::size_t index) const {
    return index < m_mixer.size() ? m_mixer[index] : 0;
}

uint32_t system22_bus::read_polygon_word(std::size_t index) const {
    return index < m_polygon_ram.size() ? m_polygon_ram[index] : 0;
}

void system22_bus::write_polygon_word(std::size_t index, uint32_t value) {
    if (index < m_polygon_ram.size()) m_polygon_ram[index] = value;
}

void system22_bus::update_irq_level() {
    int highest_level = 0;
    const std::size_t source_count = m_super_system22 ? 4 : 5;
    for (std::size_t source = 0; source < source_count; ++source) {
        const uint8_t line = static_cast<uint8_t>(1u << source);
        if ((m_irq_enabled & m_irq_state & line) != 0)
            highest_level = std::max(highest_level,
                                     static_cast<int>(m_syscontrol[source] & 7));
    }

    if (highest_level == m_asserted_irq_level) return;
    m_asserted_irq_level = highest_level;
    if (m_irq_handler) m_irq_handler(highest_level);
}

void system22_bus::write_syscontrol(std::size_t offset, uint8_t value) {
    if (m_super_system22) {
        // Super System 22 enables a source by assigning it a non-zero IRQ
        // level in registers 0-3; registers 4-7 acknowledge those sources.
        if (offset < 4) {
            const uint8_t line = static_cast<uint8_t>(1u << offset);
            if ((value & 7) != 0)
                m_irq_enabled |= line;
            else {
                m_irq_enabled &= static_cast<uint8_t>(~line);
                m_irq_state &= static_cast<uint8_t>(~line);
            }
        } else if (offset < 8) {
            m_irq_state &= static_cast<uint8_t>(
                ~(1u << static_cast<unsigned>(offset - 4)));
        }
    } else {
        // Base System 22 uses bit 4 as the enable flag and bits 0-2 as the
        // level. Registers 5-9 acknowledge sources 0-4.
        if (offset < 5) {
            const uint8_t line = static_cast<uint8_t>(1u << offset);
            if ((value & 0x10) != 0)
                m_irq_enabled |= line;
            else {
                m_irq_enabled &= static_cast<uint8_t>(~line);
                m_irq_state &= static_cast<uint8_t>(~line);
            }
        } else if (offset < 10) {
            const uint8_t line = static_cast<uint8_t>(1u << (offset - 5));
            m_irq_state &= static_cast<uint8_t>(~line);
        }
    }

    m_syscontrol[offset] = value;
    if (std::getenv("RRACER_IRQ_TRACE") && offset < 10)
        std::printf("System controller[%02zx] = %02x, enabled=%02x state=%02x\n",
                    offset, value, m_irq_enabled, m_irq_state);
    update_irq_level();
    if (offset == (m_super_system22 ? 0x16u : 0x18u) &&
        m_mcu_control_handler)
        m_mcu_control_handler(value);
    if (offset == (m_super_system22 ? 0x1cu : 0x1au) &&
        m_dsp_control_handler)
        m_dsp_control_handler(value);
}

void system22_bus::signal_vblank() {
    const uint8_t vblank_line = m_super_system22 ? uint8_t{1} :
                                                  uint8_t{1u << 4};
    if ((m_irq_enabled & vblank_line) == 0) return;
    m_irq_state |= vblank_line;
    update_irq_level();
}

bool system22_bus::load_program_rom(const uint8_t* data, std::size_t size) {
    const std::size_t expected = program_rom_size();
    if (!data || size != expected) {
        std::fprintf(stderr, "MC68020 ROM must be exactly 0x%zx bytes (got 0x%zx)\n",
                     expected, size);
        m_program_loaded = false;
        return false;
    }

    std::fill(m_program_rom.begin(), m_program_rom.end(), 0xff);
    std::copy(data, data + size, m_program_rom.begin());
    m_program_loaded = true;
    return true;
}

void system22_bus::load_eeprom(const uint8_t* data, std::size_t size) {
    if (!data || size != m_eeprom.size()) return;
    std::copy(data, data + size, m_eeprom.begin());
}

uint16_t system22_bus::next_keycus_value() {
    // Ridge Racer uses the C370 KEYCUS as a random source rather than checking
    // a fixed ID. A deterministic LFSR keeps recordings and tests repeatable.
    const uint16_t lsb = m_keycus_rng & 1;
    m_keycus_rng >>= 1;
    if (lsb) m_keycus_rng ^= 0xb400;
    return m_keycus_rng;
}

uint8_t system22_bus::read_super_mapped_byte(uint32_t address) {
    address &= 0x00ffffffu;
    if (address < SUPER_PROGRAM_ROM_SIZE)
        return m_program_rom[address];

    if (in_range(address, 0x400000, 0x20)) {
        const uint32_t keycus_address =
            0x400000u + static_cast<uint32_t>(m_keycus_register) * 2u;
        if (m_keycus_id != 0 && (address & ~1u) == keycus_address)
            return (address & 1) ? static_cast<uint8_t>(m_keycus_id) :
                                   static_cast<uint8_t>(m_keycus_id >> 8);
        const uint16_t value = next_keycus_value();
        return (address & 1) ? static_cast<uint8_t>(value) :
                               static_cast<uint8_t>(value >> 8);
    }
    if (in_range(address, 0x410000, SCI_RAM_SIZE))
        return m_sci_ram[address - 0x410000];
    if (in_range(address, 0x420000, 0x10))
        return address == 0x420001 ? 0x04 : 0x00;

    if (in_range(address, 0x430000, 0x10)) {
        uint16_t position = 0;
        const uint32_t offset = address - 0x430000;
        if (offset < 2) position = m_gun_x;
        else if ((offset >= 4 && offset < 6) ||
                 (offset >= 8 && offset < 10)) position = m_gun_y;
        if ((offset & 3u) == 0) return static_cast<uint8_t>(position >> 8);
        if ((offset & 3u) == 1) return static_cast<uint8_t>(position);
        return 0;
    }
    if (in_range(address, 0x440000, 4)) {
        const uint32_t value = 0xff00ffffu |
            (static_cast<uint32_t>(m_dip_switches & 0x00ffu) << 16);
        return static_cast<uint8_t>(value >>
            ((3u - (address - 0x440000u)) * 8u));
    }
    if (in_range(address, 0x460000, 0x4000)) {
        const uint32_t offset = address - 0x460000;
        return (offset & 1u) == 0 ? m_eeprom[offset >> 1] : 0xff;
    }
    if (in_range(address, 0x700000, m_syscontrol.size()))
        return m_syscontrol[address - 0x700000];
    if (in_range(address, 0x810000, m_super_czattr.size()))
        return m_super_czattr[address - 0x810000];
    if (in_range(address, 0x810200, 0x200)) {
        const std::size_t bank = m_super_czattr[11] & 3u;
        return m_czram[bank * 0x200 + address - 0x810200];
    }
    if (in_range(address, 0x824000, 0x400))
        return m_mixer[address - 0x824000];
    if (in_range(address, 0x828000, PALETTE_RAM_SIZE))
        return m_paletteram[address - 0x828000];
    if (in_range(address, 0x880000, CG_RAM_SIZE)) {
        if (in_range(address, 0x89e000, TEXT_RAM_SIZE))
            return m_textram[address - 0x89e000];
        return m_cgram[address - 0x880000];
    }
    if (in_range(address, 0x8a0000, TEXT_ATTR_SIZE))
        return m_textattr[address - 0x8a0000];
    if (in_range(address, 0x900000, VICS_DATA_SIZE))
        return m_vics_data[address - 0x900000];
    if (in_range(address, 0x940000, VICS_CONTROL_SIZE)) {
        const std::size_t offset = address - 0x940000;
        if (offset < 4) return 0;
        uint8_t value = m_vics_control[offset];
        if ((offset == 0x40 || offset == 0x50 ||
             offset == 0x60 || offset == 0x70))
            value &= 0x7f;
        return value;
    }
    if (in_range(address, 0x980000, SPRITE_RAM_SIZE))
        return m_sprite_ram[address - 0x980000];
    if (in_range(address, 0xa04000, MCU_SHARED_SIZE))
        return m_mcu_shared[address - 0xa04000];
    if (in_range(address, 0xc00000, POLYGON_RAM_SIZE)) {
        const uint32_t offset = address - 0xc00000;
        const uint32_t word = m_polygon_ram[offset >> 2];
        return static_cast<uint8_t>(word >> ((3 - (offset & 3)) * 8));
    }
    if (in_range(address, 0xe00000, SUPER_MAIN_RAM_SIZE))
        return m_main_ram[address - 0xe00000];
    return 0;
}

void system22_bus::write_super_mapped_byte(uint32_t address, uint8_t value) {
    address &= 0x00ffffffu;
    if (in_range(address, 0x410000, SCI_RAM_SIZE)) {
        m_sci_ram[address - 0x410000] = value;
        return;
    }
    if (in_range(address, 0x420000, 0x10) ||
        in_range(address, 0x430000, 4))
        return;
    if (in_range(address, 0x460000, 0x4000)) {
        const uint32_t offset = address - 0x460000;
        if ((offset & 1u) == 0) m_eeprom[offset >> 1] = value;
        return;
    }
    if (in_range(address, 0x700000, m_syscontrol.size())) {
        write_syscontrol(address - 0x700000, value);
        return;
    }
    if (in_range(address, 0x810000, m_super_czattr.size())) {
        m_super_czattr[address - 0x810000] = value;
        return;
    }
    if (in_range(address, 0x810200, 0x200)) {
        const uint16_t flags = static_cast<uint16_t>(
            (static_cast<uint16_t>(m_super_czattr[8]) << 8) |
            m_super_czattr[9]);
        const std::size_t offset = address - 0x810200;
        for (std::size_t bank = 0; bank < 4; ++bank)
            if (((flags >> (bank * 4)) & 1u) == 0)
                m_czram[bank * 0x200 + offset] = value;
        return;
    }
    if (in_range(address, 0x820000, 0x300)) return;
    if (in_range(address, 0x824000, 0x400)) {
        m_mixer[address - 0x824000] = value;
        return;
    }
    if (in_range(address, 0x828000, PALETTE_RAM_SIZE)) {
        m_paletteram[address - 0x828000] = value;
        return;
    }
    if (in_range(address, 0x880000, CG_RAM_SIZE)) {
        if (in_range(address, 0x89e000, TEXT_RAM_SIZE)) {
            const std::size_t offset = address - 0x89e000;
            m_textram[offset] = value;
            m_cgram[0x1e000 + offset] = value;
        } else {
            m_cgram[address - 0x880000] = value;
        }
        return;
    }
    if (in_range(address, 0x8a0000, TEXT_ATTR_SIZE)) {
        m_textattr[address - 0x8a0000] = value;
        return;
    }
    if (in_range(address, 0x900000, VICS_DATA_SIZE)) {
        m_vics_data[address - 0x900000] = value;
        return;
    }
    if (in_range(address, 0x940000, VICS_CONTROL_SIZE)) {
        m_vics_control[address - 0x940000] = value;
        return;
    }
    if (in_range(address, 0x980000, SPRITE_RAM_SIZE)) {
        m_sprite_ram[address - 0x980000] = value;
        return;
    }
    if (in_range(address, 0xa04000, MCU_SHARED_SIZE)) {
        m_mcu_shared[address - 0xa04000] = value;
        return;
    }
    if (in_range(address, 0xc00000, POLYGON_RAM_SIZE)) {
        const uint32_t offset = address - 0xc00000;
        uint32_t& word = m_polygon_ram[offset >> 2];
        const unsigned shift = (3 - (offset & 3)) * 8;
        word = (word & ~(0xffu << shift)) |
               (static_cast<uint32_t>(value) << shift);
        return;
    }
    if (in_range(address, 0xe00000, SUPER_MAIN_RAM_SIZE))
        m_main_ram[address - 0xe00000] = value;
}

uint8_t system22_bus::read_mapped_byte(uint32_t address) {
    if (m_super_system22) return read_super_mapped_byte(address);
    if (address < PROGRAM_ROM_SIZE)
        return m_program_rom[address];

    // Main RAM is mirrored with address bit 27 set.
    const uint32_t ram_address = address & ~0x08000000u;
    if (in_range(ram_address, 0x10000000, MAIN_RAM_SIZE))
        return m_main_ram[ram_address - 0x10000000];

    if (in_range(address, 0x20000000, 0x10)) {
        const uint32_t keycus_address =
            0x20000000u + static_cast<uint32_t>(m_keycus_register) * 2u;
        if (m_keycus_id != 0 && (address & ~1u) == keycus_address)
            return (address & 1) ? static_cast<uint8_t>(m_keycus_id) :
                                   static_cast<uint8_t>(m_keycus_id >> 8);
        const uint16_t value = next_keycus_value();
        return (address & 1) ? static_cast<uint8_t>(value) :
                               static_cast<uint8_t>(value >> 8);
    }
    if (in_range(address, 0x20010000, SCI_RAM_SIZE))
        return m_sci_ram[address - 0x20010000];
    if (in_range(address, 0x20020000, 0x10)) {
        // C139 SCI receive status. Non-linked cabinets report the controller
        // ready/idle bit; later System 22 games wait for it during boot.
        return address == 0x20020001 ? 0x04 : 0x00;
    }
    if (in_range(address, 0x40000000, m_syscontrol.size()))
        return m_syscontrol[address - 0x40000000];
    if (in_range(address, 0x50000000, 4)) {
        // The MC68020 sees the two 8-way switch banks in the upper half of
        // this 32-bit port. The lower half is unconnected and reads high.
        const uint32_t value = (static_cast<uint32_t>(m_dip_switches) << 16) |
                               0x0000ffffu;
        return static_cast<uint8_t>(value >>
            ((3u - (address - 0x50000000u)) * 8u));
    }
    if (in_range(address, 0x58000000, EEPROM_SIZE))
        return m_eeprom[address - 0x58000000];
    if (in_range(address, 0x60004000, MCU_SHARED_SIZE))
        return m_mcu_shared[address - 0x60004000];

    if (in_range(address, 0x70000000, POLYGON_RAM_SIZE)) {
        const uint32_t offset = address - 0x70000000;
        const uint32_t word = m_polygon_ram[offset >> 2];
        return static_cast<uint8_t>(word >> ((3 - (offset & 3)) * 8));
    }

    if (in_range(address, 0x90010000, CZ_RAM_SIZE))
        return m_czram[address - 0x90010000];
    if (in_range(address, 0x90020000, MIXER_RAM_SIZE))
        return m_mixer[address - 0x90020000];
    if (in_range(address, 0x90028000, PALETTE_RAM_SIZE))
        return m_paletteram[address - 0x90028000];
    if (in_range(address, 0x9009e000, TEXT_RAM_SIZE))
        return m_textram[address - 0x9009e000];
    if (in_range(address, 0x900a0000, TEXT_ATTR_SIZE))
        return m_textattr[address - 0x900a0000];
    if (in_range(address, 0x90080000, CG_RAM_SIZE))
        return m_cgram[address - 0x90080000];

    return 0;
}

void system22_bus::write_mapped_byte(uint32_t address, uint8_t value) {
    if (m_super_system22) {
        write_super_mapped_byte(address, value);
        return;
    }
    const uint32_t ram_address = address & ~0x08000000u;
    if (in_range(ram_address, 0x10000000, MAIN_RAM_SIZE)) {
        m_main_ram[ram_address - 0x10000000] = value;
        return;
    }
    if (in_range(address, 0x20010000, SCI_RAM_SIZE)) {
        m_sci_ram[address - 0x20010000] = value;
        return;
    }
    if (in_range(address, 0x20020000, 0x10)) {
        // Link control/FIFO writes have no observable effect in a standalone
        // cabinet until C139 networking is implemented.
        return;
    }
    if (in_range(address, 0x40000000, m_syscontrol.size())) {
        write_syscontrol(address - 0x40000000, value);
        return;
    }
    if (in_range(address, 0x58000000, EEPROM_SIZE)) {
        m_eeprom[address - 0x58000000] = value;
        return;
    }
    if (in_range(address, 0x60004000, MCU_SHARED_SIZE)) {
        m_mcu_shared[address - 0x60004000] = value;
        return;
    }
    if (in_range(address, 0x70000000, POLYGON_RAM_SIZE)) {
        const uint32_t offset = address - 0x70000000;
        uint32_t& word = m_polygon_ram[offset >> 2];
        const unsigned shift = (3 - (offset & 3)) * 8;
        word = (word & ~(0xffu << shift)) | (static_cast<uint32_t>(value) << shift);
        return;
    }
    if (in_range(address, 0x90010000, CZ_RAM_SIZE)) {
        m_czram[address - 0x90010000] = value;
        return;
    }
    if (in_range(address, 0x90020000, MIXER_RAM_SIZE)) {
        m_mixer[address - 0x90020000] = value;
        return;
    }
    if (in_range(address, 0x90028000, PALETTE_RAM_SIZE)) {
        m_paletteram[address - 0x90028000] = value;
        return;
    }
    if (in_range(address, 0x9009e000, TEXT_RAM_SIZE)) {
        const std::size_t offset = address - 0x9009e000;
        m_textram[offset] = value;
        m_cgram[0x1e000 + offset] = value;
        return;
    }
    if (in_range(address, 0x900a0000, TEXT_ATTR_SIZE)) {
        m_textattr[address - 0x900a0000] = value;
        return;
    }
    if (in_range(address, 0x90080000, CG_RAM_SIZE))
        m_cgram[address - 0x90080000] = value;
}

uint8_t system22_bus::read8(uint32_t address) {
    return read_mapped_byte(address);
}

uint16_t system22_bus::read16(uint32_t address) {
    if (m_super_system22) address &= 0x00ffffffu;
    if (m_super_system22 && address == 0x8a000a)
        return 0x8000; // C305 status used as a ready bit by Dirt Dash.
    if (m_super_system22 && address == 0x8a000e)
        return 0x0000; // C305 trigger/status register used by Time Crisis.
    const uint32_t keycus_base = m_super_system22 ? 0x400000u : 0x20000000u;
    const uint32_t keycus_size = m_super_system22 ? 0x20u : 0x10u;
    const uint32_t keycus_address = keycus_base +
        static_cast<uint32_t>(m_keycus_register) * 2u;
    if (m_keycus_id != 0 && address == keycus_address)
        return m_keycus_id;
    if (in_range(address, keycus_base, keycus_size) && (address & 1) == 0)
        return next_keycus_value();
    const uint32_t portbit_base = m_super_system22 ? 0x450008u : 0x50000008u;
    if (address == portbit_base || address == portbit_base + 2) {
        const std::size_t index = (address - portbit_base) / 2;
        const uint16_t result = m_portbits[index] & 1u;
        m_portbits[index] = static_cast<uint16_t>(
            (m_portbits[index] >> 1) | 0x8000u);
        return result;
    }
    if (m_super_system22 && in_range(address, 0x860000, 8) &&
        (address & 1u) == 0) {
        const std::size_t offset = (address - 0x860000) >> 1;
        if (offset == 2) {
            const std::size_t index = m_spot_address & 0x0ffeu;
            const uint16_t result = static_cast<uint16_t>(
                (static_cast<uint16_t>(m_spot_ram[index]) << 8) |
                m_spot_ram[index + 1]);
            m_spot_address = static_cast<uint16_t>(m_spot_address + 2);
            return result;
        }
        return 0;
    }
    return static_cast<uint16_t>((static_cast<uint16_t>(read_mapped_byte(address)) << 8) |
                                 read_mapped_byte(address + 1));
}

uint32_t system22_bus::read32(uint32_t address) {
    if (m_super_system22) {
        address &= 0x00ffffffu;
        if (in_range(address, 0x400000, 0x20) && (address & 1u) == 0)
            return (static_cast<uint32_t>(read16(address)) << 16) |
                   read16(address + 2);
    }
    return (static_cast<uint32_t>(read_mapped_byte(address)) << 24) |
           (static_cast<uint32_t>(read_mapped_byte(address + 1)) << 16) |
           (static_cast<uint32_t>(read_mapped_byte(address + 2)) << 8) |
           read_mapped_byte(address + 3);
}

void system22_bus::write8(uint32_t address, uint8_t value) {
    write_mapped_byte(address, value);
}

void system22_bus::write16(uint32_t address, uint16_t value) {
    if (m_super_system22) address &= 0x00ffffffu;
    if (m_super_system22 && address == 0x430000) {
        m_cpu_led_data = value;
        return;
    }
    const uint32_t portbit_base = m_super_system22 ? 0x450008u : 0x50000008u;
    if (address == portbit_base || address == portbit_base + 2) {
        // The write value is ignored; it reloads the corresponding serial
        // debug-input latch. No developer controls are connected by default.
        m_portbits[(address - portbit_base) / 2] = 0xffff;
        return;
    }
    if (m_super_system22 && in_range(address, 0x860000, 8) &&
        (address & 1u) == 0) {
        const std::size_t offset = (address - 0x860000) >> 1;
        if (offset == 0) {
            m_spot_address = value;
        } else if (offset == 1) {
            const std::size_t index = m_spot_address & 0x0ffeu;
            m_spot_ram[index] = static_cast<uint8_t>(value >> 8);
            m_spot_ram[index + 1] = static_cast<uint8_t>(value);
            m_spot_address = static_cast<uint16_t>(m_spot_address + 2);
        } else if (offset == 3) {
            m_spot_enable = value;
        }
        return;
    }
    write_mapped_byte(address, static_cast<uint8_t>(value >> 8));
    write_mapped_byte(address + 1, static_cast<uint8_t>(value));
}

void system22_bus::write32(uint32_t address, uint32_t value) {
    write_mapped_byte(address, static_cast<uint8_t>(value >> 24));
    write_mapped_byte(address + 1, static_cast<uint8_t>(value >> 16));
    write_mapped_byte(address + 2, static_cast<uint8_t>(value >> 8));
    write_mapped_byte(address + 3, static_cast<uint8_t>(value));
}

mc68020_core::mc68020_core(system22_bus& bus) : m_bus(bus) {
    set_active_musashi_memory(this);
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68020);
    m_bus.set_irq_handler([this](int level) { trigger_irq(level); });
}

mc68020_core::~mc68020_core() {
    m_bus.set_irq_handler({});
    set_active_musashi_memory(nullptr);
}

bool mc68020_core::load_rom(const uint8_t* rom, std::size_t size) {
    set_active_musashi_memory(this);
    if (!m_bus.load_program_rom(rom, size)) return false;
    reset();
    return m_ready;
}

void mc68020_core::reset() {
    set_active_musashi_memory(this);
    if (!m_bus.has_program_rom()) {
        m_ready = false;
        return;
    }
    m68k_pulse_reset();
    m_ready = true;
}

int mc68020_core::execute(int cycles) {
    if (!m_ready || cycles <= 0) return 0;
    set_active_musashi_memory(this);
    return m68k_execute(cycles);
}

void mc68020_core::trigger_irq(int level) {
    if (!m_ready) return;
    set_active_musashi_memory(this);
    m68k_set_irq(std::clamp(level, 0, 7));
}

uint32_t mc68020_core::program_counter() const {
    set_active_musashi_memory(const_cast<mc68020_core*>(this));
    return m68k_get_reg(nullptr, M68K_REG_PC);
}

uint32_t mc68020_core::stack_pointer() const {
    set_active_musashi_memory(const_cast<mc68020_core*>(this));
    return m68k_get_reg(nullptr, M68K_REG_SP);
}

uint32_t mc68020_core::data_register(unsigned index) const {
    if (index > 7) return 0;
    set_active_musashi_memory(const_cast<mc68020_core*>(this));
    return m68k_get_reg(nullptr,
                        static_cast<m68k_register_t>(M68K_REG_D0 + index));
}

uint32_t mc68020_core::address_register(unsigned index) const {
    if (index > 7) return 0;
    set_active_musashi_memory(const_cast<mc68020_core*>(this));
    return m68k_get_reg(nullptr,
                        static_cast<m68k_register_t>(M68K_REG_A0 + index));
}

std::string mc68020_core::disassemble(uint32_t address, std::size_t* length) const {
    set_active_musashi_memory(const_cast<mc68020_core*>(this));
    std::array<unsigned char, 32> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = m_bus.read8(address + static_cast<uint32_t>(i));

    char instruction[128]{};
    const unsigned int bytes_used = m68k_disassemble_raw(
        instruction, address, bytes.data(), bytes.data(), M68K_CPU_TYPE_68020);
    if (length) *length = bytes_used;
    return instruction;
}
