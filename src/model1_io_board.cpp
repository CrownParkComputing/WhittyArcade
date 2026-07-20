#include "model1_io_board.h"

#include "z80.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace {

constexpr uint16_t standard_rom_end = 0x3fff;

uint8_t scale_axis(uint8_t value, int minimum, int maximum, bool reverse) {
    const int source = reverse ? 0xff - value : value;
    return static_cast<uint8_t>(minimum + source * (maximum - minimum) / 255);
}

} // namespace

struct model1_io_board::implementation {
    implementation(model1_io_board_type board_type,
                   const std::vector<uint8_t>& program,
                   std::vector<uint8_t>& shared_ram,
                   std::array<uint8_t, 0x80>& operator_eeprom)
        : type(board_type), firmware(program), dpram(shared_ram),
          eeprom(operator_eeprom) {
        valid = type == model1_io_board_type::standard_315_5338a &&
                firmware.size() == 0x10000 && dpram.size() >= 0x800;
        reset();
    }

    void reset() {
        work_ram.fill(0);
        port_value.fill(0xff);
        digital_inputs.fill(0xff);
        analog_inputs.fill(0x7f);
        dip_switches.fill(0xff);
        port_config = 0;
        serial_output = 0;
        serial_address = 0;
        secondary_controls = false;
        adc_shift = 0;
        total_clocks = 0;
        eeprom_cs = false;
        eeprom_clock = false;
        eeprom_di = false;
        eeprom_do = true;
        eeprom_write_enabled = false;
        eeprom_command = 0;
        eeprom_command_bits = 0;
        eeprom_read_shift = 0;
        eeprom_read_bits = 0;
        eeprom_write_shift = 0;
        eeprom_write_bits = 0;
        eeprom_write_address = 0;
        pins = z80_init(&cpu);
    }

    uint16_t eeprom_word(unsigned address) const {
        const std::size_t offset = (address & 0x3f) * 2;
        return static_cast<uint16_t>((uint16_t{eeprom[offset]} << 8) |
                                     eeprom[offset + 1]);
    }

    void set_eeprom_word(unsigned address, uint16_t value) {
        const std::size_t offset = (address & 0x3f) * 2;
        eeprom[offset] = static_cast<uint8_t>(value >> 8);
        eeprom[offset + 1] = static_cast<uint8_t>(value);
    }

    void set_eeprom_lines(uint8_t data) {
        const bool new_clock = (data & 0x80) != 0;
        const bool new_cs = (data & 0x40) != 0;
        eeprom_di = (data & 0x20) != 0;

        if (!new_cs) {
            eeprom_command = 0;
            eeprom_command_bits = 0;
            eeprom_read_bits = 0;
            eeprom_write_bits = 0;
            eeprom_do = true;
        } else if (!eeprom_clock && new_clock) {
            eeprom_rising_edge();
        }
        eeprom_cs = new_cs;
        eeprom_clock = new_clock;
    }

    void eeprom_rising_edge() {
        if (eeprom_read_bits > 0) {
            eeprom_do = (eeprom_read_shift & 0x8000) != 0;
            eeprom_read_shift <<= 1;
            --eeprom_read_bits;
            return;
        }
        if (eeprom_write_bits > 0) {
            eeprom_write_shift = static_cast<uint16_t>(
                (eeprom_write_shift << 1) | (eeprom_di ? 1 : 0));
            if (--eeprom_write_bits == 0 && eeprom_write_enabled)
                set_eeprom_word(eeprom_write_address, eeprom_write_shift);
            return;
        }

        // 93C45/46 in 16-bit mode: start, two-bit opcode, six-bit address.
        if (eeprom_command_bits == 0 && !eeprom_di) return;
        eeprom_command = static_cast<uint16_t>(
            (eeprom_command << 1) | (eeprom_di ? 1 : 0));
        if (++eeprom_command_bits != 9) return;

        const unsigned opcode = (eeprom_command >> 6) & 3;
        const unsigned address = eeprom_command & 0x3f;
        if (opcode == 2) { // READ
            eeprom_read_shift = eeprom_word(address);
            eeprom_read_bits = 16;
            eeprom_do = (eeprom_read_shift & 0x8000) != 0;
        } else if (opcode == 1) { // WRITE
            eeprom_write_address = address;
            eeprom_write_shift = 0;
            eeprom_write_bits = 16;
        } else if (opcode == 3 && eeprom_write_enabled) { // ERASE
            set_eeprom_word(address, 0xffff);
        } else if (opcode == 0) {
            const unsigned control = address >> 4;
            if (control == 3) eeprom_write_enabled = true;   // EWEN
            if (control == 0) eeprom_write_enabled = false;  // EWDS
        }
        eeprom_command = 0;
        eeprom_command_bits = 0;
    }

    uint8_t input_port(unsigned index) const {
        if (index >= digital_inputs.size()) return 0xff;
        return secondary_controls ? dip_switches[index] :
                                    digital_inputs[index];
    }

    uint8_t chip_read(uint8_t offset) {
        if (offset <= 6) {
            if ((port_config & (1u << offset)) == 0)
                return port_value[offset];
            switch (offset) {
            case 1: return input_port(0);
            case 2: return input_port(1);
            case 3: return input_port(2);
            case 4: return 0xff;
            case 6:
                return static_cast<uint8_t>((eeprom_do ? 0x80 : 0x00) |
                                            0x70 | 0x0f);
            default: return 0xff;
            }
        }
        if (offset == 0x08) return port_config;
        if (offset == 0x0a) return serial_output;
        if (offset == 0x0c)
            return serial_address < dpram.size() ?
                   dpram[serial_address] : 0xff;
        if (offset == 0x0d) return 0x08;
        return 0xff;
    }

    void output_port(unsigned offset, uint8_t value) {
        if (offset == 0) {
            set_eeprom_lines(value);
            secondary_controls = (value & 1) != 0;
        }
    }

    void chip_write(uint8_t offset, uint8_t value) {
        if (offset <= 6) {
            port_value[offset] = value;
            if ((port_config & (1u << offset)) == 0)
                output_port(offset, value);
            return;
        }
        if (offset == 0x08) {
            const uint8_t became_output = static_cast<uint8_t>(
                port_config & ~value);
            port_config = value;
            for (unsigned port = 0; port < 7; ++port)
                if (became_output & (1u << port))
                    output_port(port, port_value[port]);
            return;
        }
        if (offset == 0x0a) {
            serial_output = value;
            return;
        }
        if (offset != 0x09) return;
        switch (value) {
        case 0x00:
            serial_address = static_cast<uint16_t>(
                (serial_address & 0xff00) | serial_output);
            break;
        case 0x01:
            serial_address = static_cast<uint16_t>(
                (serial_address & 0x00ff) | (uint16_t{serial_output} << 8));
            break;
        case 0x07:
            if (serial_address < dpram.size())
                dpram[serial_address] = serial_output;
            break;
        case 0x87: // Prepare a DPRAM read; data is returned at register C.
        default:
            break;
        }
    }

    uint8_t memory_read(uint16_t address) {
        if (address <= standard_rom_end)
            return firmware[address];
        if (address >= 0x4000 && address <= 0x5fff)
            return work_ram[address - 0x4000];
        if (address >= 0x8000 && address <= 0x800f)
            return chip_read(static_cast<uint8_t>(address & 0x0f));
        if (address >= 0xc000 && address <= 0xc003) {
            const uint8_t bit = static_cast<uint8_t>((adc_shift >> 7) & 1);
            adc_shift <<= 1;
            return bit;
        }
        return 0xff;
    }

    void memory_write(uint16_t address, uint8_t value) {
        if (address >= 0x4000 && address <= 0x5fff) {
            work_ram[address - 0x4000] = value;
            return;
        }
        if (address >= 0x8000 && address <= 0x800f) {
            chip_write(static_cast<uint8_t>(address & 0x0f), value);
            return;
        }
        if (address >= 0xc000 && address <= 0xc003) {
            const unsigned base = secondary_controls ? 4 : 0;
            adc_shift = analog_inputs[base + (address & 3)];
        }
    }

    void execute(int clocks) {
        if (!valid || clocks <= 0) return;
        for (int clock = 0; clock < clocks; ++clock) {
            pins = z80_tick(&cpu, pins);
            if (pins & Z80_MREQ) {
                const uint16_t address = Z80_GET_ADDR(pins);
                if (pins & Z80_RD) {
                    Z80_SET_DATA(pins, memory_read(address));
                } else if (pins & Z80_WR) {
                    memory_write(address, Z80_GET_DATA(pins));
                }
            } else if ((pins & (Z80_IORQ | Z80_M1)) ==
                       (Z80_IORQ | Z80_M1)) {
                // No daisy-chained peripherals on the standard board.
                Z80_SET_DATA(pins, 0xff);
            } else if ((pins & (Z80_IORQ | Z80_RD)) ==
                       (Z80_IORQ | Z80_RD)) {
                Z80_SET_DATA(pins, 0xff);
            }
        }
        total_clocks += static_cast<uint64_t>(clocks);
    }

    void set_inputs(model1_rom_set game, const input_state& state) {
        digital_inputs.fill(0xff);
        analog_inputs.fill(0x7f);
        uint8_t& in0 = digital_inputs[0];
        uint8_t& in1 = digital_inputs[1];
        if (state.coin1) in0 &= ~uint8_t{0x01};
        if (state.coin2) in0 &= ~uint8_t{0x02};
        if (state.test) in0 &= ~uint8_t{0x04};
        if (state.service) in0 &= ~uint8_t{0x08};
        if (state.start) in0 &= ~uint8_t{0x10};

        if (game == model1_rom_set::virtua_fighter) {
            if (state.view) in1 &= ~uint8_t{0x01};
            if (state.view2) in1 &= ~uint8_t{0x02};
            if (state.view3) in1 &= ~uint8_t{0x04};
            if (state.left_stick_y > 0xaf) in1 &= ~uint8_t{0x10};
            if (state.left_stick_y < 0x50) in1 &= ~uint8_t{0x20};
            if (state.left_stick_x > 0xaf) in1 &= ~uint8_t{0x40};
            if (state.left_stick_x < 0x50) in1 &= ~uint8_t{0x80};
            return;
        }
        if (game == model1_rom_set::star_wars_arcade) {
            if (state.view) in1 &= ~uint8_t{0x01};
            if (state.view2) in1 &= ~uint8_t{0x02};
            if (state.view3) in1 &= ~uint8_t{0x10};
            analog_inputs[0] = scale_axis(state.left_stick_x, 27, 227, true);
            analog_inputs[1] = scale_axis(state.left_stick_y, 27, 227, false);
            analog_inputs[2] = static_cast<uint8_t>(200 -
                std::clamp<int>(state.gas, 0, 0x610) * 172 / 0x610);
            analog_inputs[4] = scale_axis(state.right_stick_x, 27, 227, true);
            analog_inputs[5] = scale_axis(state.right_stick_y, 27, 227, false);
            return;
        }

        if (state.view) in0 &= ~uint8_t{0x20};
        if (state.view2) in0 &= ~uint8_t{0x40};
        if (state.view3) in0 &= ~uint8_t{0x80};
        if (state.view4) in1 &= ~uint8_t{0x01};
        if (state.shift_down) in1 &= ~uint8_t{0x10};
        if (state.shift_up) in1 &= ~uint8_t{0x20};
        const int steering = static_cast<int>(state.steering) - 0x280;
        analog_inputs[0] = static_cast<uint8_t>(std::clamp(
            steering * 255 / (0xd80 - 0x280), 0, 255));
        analog_inputs[1] = static_cast<uint8_t>(0x30 +
            std::clamp<int>(state.gas, 0, 0x610) * (0xff - 0x30) / 0x610);
        analog_inputs[2] = static_cast<uint8_t>(0x30 +
            std::clamp<int>(state.brake, 0, 0x610) * (0xff - 0x30) / 0x610);
    }

    model1_io_board_type type;
    const std::vector<uint8_t>& firmware;
    std::vector<uint8_t>& dpram;
    std::array<uint8_t, 0x80>& eeprom;
    bool valid{};
    z80_t cpu{};
    uint64_t pins{};
    uint64_t total_clocks{};
    std::array<uint8_t, 0x2000> work_ram{};
    std::array<uint8_t, 7> port_value{};
    std::array<uint8_t, 3> digital_inputs{};
    std::array<uint8_t, 8> analog_inputs{};
    std::array<uint8_t, 3> dip_switches{};
    uint8_t port_config{};
    uint8_t serial_output{};
    uint16_t serial_address{};
    bool secondary_controls{};
    uint8_t adc_shift{};

    bool eeprom_cs{};
    bool eeprom_clock{};
    bool eeprom_di{};
    bool eeprom_do{true};
    bool eeprom_write_enabled{};
    uint16_t eeprom_command{};
    unsigned eeprom_command_bits{};
    uint16_t eeprom_read_shift{};
    unsigned eeprom_read_bits{};
    uint16_t eeprom_write_shift{};
    unsigned eeprom_write_bits{};
    unsigned eeprom_write_address{};
};

model1_io_board::model1_io_board(model1_io_board_type type,
                                 const std::vector<uint8_t>& firmware,
                                 std::vector<uint8_t>& dual_port_ram,
                                 std::array<uint8_t, 0x80>& eeprom)
    : m_impl(std::make_unique<implementation>(type, firmware, dual_port_ram,
                                               eeprom)) {}

model1_io_board::~model1_io_board() = default;

void model1_io_board::reset() { m_impl->reset(); }
void model1_io_board::execute(int clocks) { m_impl->execute(clocks); }
void model1_io_board::set_inputs(model1_rom_set game,
                                 const input_state& state) {
    m_impl->set_inputs(game, state);
}
void model1_io_board::set_dip_switches(
        const std::array<uint8_t, 3>& switches) {
    m_impl->dip_switches = switches;
}
bool model1_io_board::active() const { return m_impl->valid; }
uint16_t model1_io_board::program_counter() const { return m_impl->cpu.pc; }
uint64_t model1_io_board::executed_clocks() const {
    return m_impl->total_clocks;
}
