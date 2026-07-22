// system22_cpu.h - System 22 main CPU and memory bus
#pragma once

#include "system22_types.h"
#include "musashi_memory.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

enum class system22_driving_profile : uint8_t {
    ridge_racer,
    rave_racer,
    ace_driver,
    cyber_commando,
    time_crisis,
    dirt_dash,
    aqua_jet,
};

// Byte-accurate, big-endian view of the System 22 main CPU maps.
// Device side effects are deliberately kept behind this bus so the MC68020
// core can remain an off-the-shelf component.
class system22_bus {
public:
    static constexpr std::size_t PROGRAM_ROM_SIZE = 0x200000;
    static constexpr std::size_t SUPER_PROGRAM_ROM_SIZE = 0x400000;
    static constexpr std::size_t MAIN_RAM_SIZE = 0x020000;
    static constexpr std::size_t SUPER_MAIN_RAM_SIZE = 0x040000;
    static constexpr std::size_t SCI_RAM_SIZE = 0x004000;
    static constexpr std::size_t EEPROM_SIZE = 0x002000;
    static constexpr std::size_t MCU_SHARED_SIZE = 0x008000;
    static constexpr std::size_t POLYGON_RAM_SIZE = 0x020000;
    static constexpr std::size_t CZ_RAM_SIZE = 0x008000;
    static constexpr std::size_t MIXER_RAM_SIZE = 0x008000;
    static constexpr std::size_t PALETTE_RAM_SIZE = 0x018000;
    static constexpr std::size_t CG_RAM_SIZE = 0x020000;
    static constexpr std::size_t TEXT_RAM_SIZE = 0x002000;
    static constexpr std::size_t TEXT_ATTR_SIZE = 0x000010;
    static constexpr std::size_t VICS_DATA_SIZE = 0x010000;
    static constexpr std::size_t VICS_CONTROL_SIZE = 0x000080;
    static constexpr std::size_t SPRITE_RAM_SIZE = 0x030000;
    static constexpr std::size_t SPOT_RAM_SIZE = 0x001000;

    system22_bus();

    bool load_program_rom(const uint8_t* data, std::size_t size);
    void set_super_system22(bool enabled);
    bool is_super_system22() const { return m_super_system22; }
    std::size_t program_rom_size() const {
        return m_super_system22 ? SUPER_PROGRAM_ROM_SIZE : PROGRAM_ROM_SIZE;
    }
    void load_eeprom(const uint8_t* data, std::size_t size);
    const uint8_t* eeprom_data() const { return m_eeprom.data(); }
    std::size_t eeprom_size() const { return m_eeprom.size(); }
    bool has_program_rom() const { return m_program_loaded; }

    uint8_t read8(uint32_t address);
    uint16_t read16(uint32_t address);
    uint32_t read32(uint32_t address);

    void write8(uint32_t address, uint8_t value);
    void write16(uint32_t address, uint16_t value);
    void write32(uint32_t address, uint32_t value);

    // The System 22 controller presents five independently configurable IRQ
    // sources to the 68020. The callback receives the highest asserted level,
    // or zero when all sources have been acknowledged.
    void set_irq_handler(std::function<void(int)> handler);
    void set_dsp_control_handler(std::function<void(uint8_t)> handler);
    void set_mcu_control_handler(std::function<void(uint8_t)> handler);
    // Some C370 KEYCUS revisions expose a game-specific ID at register 0.
    // A value of zero keeps the deterministic pseudo-random fallback used by
    // the original Ridge Racer program.
    void set_keycus(uint16_t id, uint8_t register_offset) {
        m_keycus_id = id;
        m_keycus_register = register_offset;
    }
    void clear_keycus() { m_keycus_id = 0; }
    void set_driving_profile(system22_driving_profile profile) {
        m_driving_profile = profile;
    }
    // Two eight-position active-low banks: SW2 in bits 0..7 and SW3 in
    // bits 8..15. A set bit is OFF, matching the physical switches.
    uint16_t dip_switches() const { return m_dip_switches; }
    void set_dip_switches(uint16_t switches) { m_dip_switches = switches; }
    void signal_vblank();

    uint8_t read_mcu_shared_byte(std::size_t index) const;
    void write_mcu_shared_byte(std::size_t index, uint8_t value);
    // The C74 is little-endian while the MC68020 is big-endian.  Both CPUs
    // share 16-bit words, so their byte addresses select opposite lanes.
    uint8_t read_c74_shared_byte(std::size_t index) const;
    void write_c74_shared_byte(std::size_t index, uint8_t value);
    void update_driving_inputs(const input_state& state);
    void update_game_inputs(const input_state& state);
    uint8_t read_cz_byte(std::size_t index) const;
    uint16_t read_super_cz_attribute(std::size_t index) const;
    uint16_t read_super_cz_entry(std::size_t bank,
                                 std::size_t index) const;
    uint8_t read_mixer_byte(std::size_t index) const;
    uint32_t read_polygon_word(std::size_t index) const;
    void write_polygon_word(std::size_t index, uint32_t value);

    uint32_t* polygon_ram_data() { return m_polygon_ram.data(); }
    const uint32_t* polygon_ram_data() const { return m_polygon_ram.data(); }
    const uint8_t* palette_ram_data() const { return m_paletteram.data(); }
    std::size_t palette_ram_size() const { return m_paletteram.size(); }
    const uint8_t* character_ram_data() const { return m_cgram.data(); }
    std::size_t character_ram_size() const { return m_cgram.size(); }
    const uint8_t* text_ram_data() const { return m_textram.data(); }
    std::size_t text_ram_size() const { return m_textram.size(); }
    const uint8_t* text_attr_data() const { return m_textattr.data(); }
    const uint8_t* mixer_data() const { return m_mixer.data(); }
    std::size_t mixer_size() const { return m_mixer.size(); }
    const uint8_t* sprite_ram_data() const { return m_sprite_ram.data(); }
    std::size_t sprite_ram_size() const { return m_sprite_ram.size(); }
    const uint8_t* vics_data() const { return m_vics_data.data(); }
    std::size_t vics_data_size() const { return m_vics_data.size(); }
    const uint8_t* vics_control_data() const { return m_vics_control.data(); }
    std::size_t vics_control_size() const { return m_vics_control.size(); }
    uint16_t gun_x() const { return m_gun_x; }
    uint16_t gun_y() const { return m_gun_y; }
    uint16_t cpu_led_data() const { return m_cpu_led_data; }

private:
    uint8_t read_mapped_byte(uint32_t address);
    void write_mapped_byte(uint32_t address, uint8_t value);
    uint8_t read_super_mapped_byte(uint32_t address);
    void write_super_mapped_byte(uint32_t address, uint8_t value);
    uint16_t next_keycus_value();
    void write_shared_word(std::size_t offset, uint16_t value);
    void update_coinage(const input_state& state);
    void update_cyber_commando_inputs(const input_state& state);
    void update_time_crisis_inputs(const input_state& state);
    void write_syscontrol(std::size_t offset, uint8_t value);
    void update_irq_level();

    std::vector<uint8_t> m_program_rom;
    std::vector<uint8_t> m_main_ram;
    std::vector<uint8_t> m_sci_ram;
    std::vector<uint8_t> m_eeprom;
    std::vector<uint8_t> m_mcu_shared;
    std::vector<uint32_t> m_polygon_ram;
    std::vector<uint8_t> m_czram;
    std::vector<uint8_t> m_mixer;
    std::vector<uint8_t> m_paletteram;
    std::vector<uint8_t> m_cgram;
    std::vector<uint8_t> m_textram;
    std::vector<uint8_t> m_vics_data;
    std::vector<uint8_t> m_vics_control;
    std::vector<uint8_t> m_sprite_ram;
    std::array<uint8_t, 0x10> m_super_czattr{};
    std::array<uint8_t, SPOT_RAM_SIZE> m_spot_ram{};
    std::array<uint8_t, TEXT_ATTR_SIZE> m_textattr{};
    std::array<uint8_t, 0x20> m_syscontrol{};
    std::array<uint16_t, 2> m_portbits{0xffff, 0xffff};
    std::function<void(int)> m_irq_handler;
    std::function<void(uint8_t)> m_dsp_control_handler;
    std::function<void(uint8_t)> m_mcu_control_handler;
    uint8_t m_irq_enabled{0};
    uint8_t m_irq_state{0};
    int m_asserted_irq_level{0};

    uint16_t m_keycus_id{0};
    uint8_t m_keycus_register{0};
    uint16_t m_keycus_rng{0x4d2b};
    uint16_t m_dip_switches{0xffff};
    uint16_t m_gun_x{381};
    uint16_t m_gun_y{163};
    uint16_t m_cpu_led_data{0xffff};
    uint16_t m_spot_address{0};
    uint16_t m_spot_enable{0};
    bool m_super_system22{false};
    system22_driving_profile m_driving_profile{
        system22_driving_profile::ridge_racer};
    bool m_coin_state_initialized{false};
    bool m_coin1_previous{false};
    bool m_coin2_previous{false};
    uint8_t m_credits1{0};
    uint8_t m_credits2{0};
    bool m_program_loaded{false};
};

// Motorola MC68020 wrapper backed by Musashi. Musashi uses a global core
// context, which is sufficient here because System 22 has one 68K-family CPU.
class mc68020_core : public musashi_memory {
public:
    explicit mc68020_core(system22_bus& bus);
    ~mc68020_core();

    mc68020_core(const mc68020_core&) = delete;
    mc68020_core& operator=(const mc68020_core&) = delete;

    bool load_rom(const uint8_t* rom, std::size_t size);
    void reset();
    int execute(int cycles);
    void trigger_irq(int level);

    uint32_t program_counter() const;
    uint32_t stack_pointer() const;
    uint32_t data_register(unsigned index) const;
    uint32_t address_register(unsigned index) const;
    std::string disassemble(uint32_t address, std::size_t* length = nullptr) const;
    bool ready() const { return m_ready; }

    uint8_t read8(uint32_t address) override { return m_bus.read8(address); }
    uint16_t read16(uint32_t address) override { return m_bus.read16(address); }
    uint32_t read32(uint32_t address) override { return m_bus.read32(address); }
    void write8(uint32_t address, uint8_t value) override { m_bus.write8(address, value); }
    void write16(uint32_t address, uint16_t value) override { m_bus.write16(address, value); }
    void write32(uint32_t address, uint32_t value) override { m_bus.write32(address, value); }

private:
    system22_bus& m_bus;
    bool m_ready{false};
};
