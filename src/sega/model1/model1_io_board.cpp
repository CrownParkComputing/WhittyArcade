#include "sega/model1/model1_io_board.h"

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

// Map an 8-bit host axis (0..255) onto a light gun's calibrated 10-bit ADC
// range. Virtua Cop reads these back through the model1io2 FPGA.
uint16_t scale_gun(uint8_t value, int minimum, int maximum) {
    return static_cast<uint16_t>(minimum +
                                 value * (maximum - minimum) / 255);
}

// Compact Zilog Z80 CTC: four channels in timer mode with Mode-2 vectored
// interrupts. Enough of the TMPZ84C015's on-chip CTC for the Virtua Cop I/O
// firmware, whose service loop is driven by a periodic CTC interrupt. Channels
// 2 and 3 are used by the firmware as the SIO baud-rate clock and never enable
// interrupts, so a plain down-counter models every channel the firmware uses.
// The board is clocked a T-cycle at a time, so this runs 9.83 million times a
// second and its cost lands squarely in the frame budget. Channels therefore
// keep an absolute expiry rather than a per-tick countdown: `tick` then costs
// one add and one compare until a channel is actually due, instead of walking
// all four channels twice per clock.
struct z80_ctc {
    struct channel {
        uint16_t time_constant{256};
        int64_t deadline{};      // clock at which this channel next hits zero
        int32_t counter{};       // remaining clocks, held while stopped
        int prescale{16};
        bool running{};
        bool int_enable{};
        bool tc_follows{};
        bool int_pending{};
    };
    std::array<channel, 4> ch{};
    uint8_t vector_base{};
    int in_service{-1};
    int64_t now{};
    // Sentinel far beyond any real session: nothing is due to expire.
    static constexpr int64_t never = INT64_MAX;
    int64_t next_deadline{never};
    uint8_t pending_mask{};

    void reset() {
        ch = {};
        vector_base = 0;
        in_service = -1;
        now = 0;
        next_deadline = never;
        pending_mask = 0;
    }

    // Remaining clocks on a channel: live while it runs, frozen once stopped.
    int32_t remaining(const channel& c) const {
        return c.running ? static_cast<int32_t>(c.deadline - now) : c.counter;
    }

    void refresh_next_deadline() {
        next_deadline = never;
        for (const channel& c : ch)
            if (c.running && c.deadline < next_deadline)
                next_deadline = c.deadline;
    }

    void write(unsigned index, uint8_t data) {
        channel& c = ch[index & 3];
        if (c.tc_follows) {
            c.time_constant = data ? data : 256;
            c.deadline = now + c.time_constant * c.prescale;
            c.running = true;
            c.tc_follows = false;
            refresh_next_deadline();
            return;
        }
        if (data & 0x01) { // control word
            c.int_enable = (data & 0x80) != 0;
            c.prescale = (data & 0x20) ? 256 : 16;
            c.tc_follows = (data & 0x04) != 0;
            if (data & 0x02) { // software reset
                c.counter = remaining(c);
                c.running = false;
                c.int_pending = false;
                pending_mask &= static_cast<uint8_t>(~(1u << (index & 3)));
                refresh_next_deadline();
            }
        } else if ((index & 3) == 0) {
            vector_base = data & 0xf8; // channel 0 carries the vector base
        }
    }

    uint8_t read(unsigned index) const {
        const channel& c = ch[index & 3];
        const int down = c.prescale ?
            (remaining(c) + c.prescale - 1) / c.prescale : 0;
        return static_cast<uint8_t>(down & 0xff);
    }

    void tick(int cycles) {
        now += cycles;
        if (now < next_deadline) return; // nothing due: the usual case
        for (unsigned index = 0; index < ch.size(); ++index) {
            channel& c = ch[index];
            if (!c.running || c.deadline > now) continue;
            do {
                c.deadline += c.time_constant * c.prescale;
                if (c.int_enable) {
                    c.int_pending = true;
                    pending_mask |= static_cast<uint8_t>(1u << index);
                }
            } while (c.deadline <= now);
        }
        refresh_next_deadline();
    }

    bool interrupt_requested() const {
        return in_service < 0 && pending_mask != 0;
    }

    uint8_t acknowledge() {
        for (int index = 0; index < 4; ++index) {
            if (ch[index].int_pending) {
                ch[index].int_pending = false;
                pending_mask &= static_cast<uint8_t>(~(1u << index));
                in_service = index;
                return static_cast<uint8_t>(vector_base | (index << 1));
            }
        }
        return 0xff;
    }

    void return_from_interrupt() { in_service = -1; }
};

} // namespace

struct model1_io_board::implementation {
    implementation(model1_io_board_type board_type,
                   const std::vector<uint8_t>& program,
                   std::vector<uint8_t>& shared_ram,
                   std::array<uint8_t, 0x80>& operator_eeprom)
        : type(board_type), firmware(program), dpram(shared_ram),
          eeprom(operator_eeprom) {
        valid = (type == model1_io_board_type::standard_315_5338a ||
                 type == model1_io_board_type::advanced_tmpz84c015) &&
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
        ctc.reset();
        fpga_counter = 0;
        gun_ports.fill(0);
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
        if (type == model1_io_board_type::advanced_tmpz84c015) {
            advanced_execute(clocks);
            return;
        }
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

    // ---- Advanced board (model1io2 / TMPZ84C015), used by Virtua Cop ----

    // The 93C46 serial lines are bit-banged through 315-5338A port F on this
    // board: clk = bit 5, di = bit 6, cs = bit 4. Data-out is read back through
    // the board jumper port (0x8040 bit 6), not the chip.
    void advanced_set_eeprom_lines(uint8_t data) {
        const bool new_clock = (data & 0x20) != 0;
        const bool new_cs = (data & 0x10) != 0;
        eeprom_di = (data & 0x40) != 0;
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

    uint8_t advanced_board_read() const {
        // bits 0-3 board buttons (not pressed = 1), bit4 ROM_EMU jumper,
        // bit5 MODE jumper, bit6 EEPROM data-out, bit7 unused. The reset code
        // needs bits 4 and 5 set to take the normal (0x0700) firmware path.
        return static_cast<uint8_t>(0x0f | 0x10 | 0x20 | 0x80 |
                                    (eeprom_do ? 0x40 : 0x00));
    }

    uint8_t advanced_chip_read(uint8_t offset) {
        if (offset <= 6) {
            if ((port_config & (1u << offset)) == 0)
                return port_value[offset];
            // Ports A/B/C are the IN0/IN1/IN2 digital inputs on this board.
            if (offset <= 2) return digital_inputs[offset];
            return 0xff;
        }
        if (offset == 0x08) return port_config;
        if (offset == 0x0a) return serial_output;
        if (offset == 0x0c)
            return serial_address < dpram.size() ?
                   dpram[serial_address] : 0xff;
        if (offset == 0x0d) return 0x08; // status: transfer finished
        return 0xff;
    }

    void advanced_chip_write(uint8_t offset, uint8_t value) {
        if (offset <= 6) {
            port_value[offset] = value;
            if (offset == 5) advanced_set_eeprom_lines(value); // port F
            else if (offset == 6) secondary_controls = (value & 0x40) != 0;
            return;
        }
        if (offset == 0x08) { port_config = value; return; }
        if (offset == 0x0a) { serial_output = value; return; }
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
        default:
            if ((value & 0xf8) == 0x70) { // write to a low DPRAM register
                const unsigned reg = value & 0x07;
                if (reg < dpram.size()) dpram[reg] = serial_output;
            }
            break;
        }
    }

    uint8_t advanced_fpga_read(uint8_t offset) const {
        // The firmware uploads an FPGA bitstream (0x1400 writes) before the
        // gun registers respond; until then every read returns 0x80.
        if (fpga_counter < 0x1400) return 0x80;
        if (offset < 8)
            return static_cast<uint8_t>(
                gun_ports[offset >> 1] >> (8 * (offset & 1)));
        if (offset == 8) return gun_offscreen;
        return 0xff;
    }

    uint8_t advanced_memory_read(uint16_t address) {
        if (address <= 0x7fff) return firmware[address];
        if (address >= 0x8000 && address <= 0x800f)
            return advanced_chip_read(static_cast<uint8_t>(address & 0x0f));
        if (address == 0x8040) return advanced_board_read();
        if (address == 0x8080) return 0xff; // dsw1
        if (address >= 0x8100 && address <= 0x810f)
            return advanced_fpga_read(static_cast<uint8_t>(address & 0x0f));
        if (address >= 0x8200 && address <= 0x8207) return 0xff; // ADC
        if (address >= 0xe000) return work_ram[address & 0x1fff];
        return 0xff;
    }

    void advanced_memory_write(uint16_t address, uint8_t value) {
        if (address >= 0x8000 && address <= 0x800f) {
            advanced_chip_write(static_cast<uint8_t>(address & 0x0f), value);
            return;
        }
        if (address >= 0x8100 && address <= 0x810f) {
            if (fpga_counter < 0x2000) ++fpga_counter; // bitstream upload
            return;
        }
        if (address >= 0xe000) work_ram[address & 0x1fff] = value;
    }

    void advanced_execute(int clocks) {
        for (int clock = 0; clock < clocks; ++clock) {
            ctc.tick(1);
            if (ctc.interrupt_requested()) pins |= Z80_INT;
            else pins &= ~Z80_INT;

            pins = z80_tick(&cpu, pins);
            if (pins & Z80_MREQ) {
                const uint16_t address = Z80_GET_ADDR(pins);
                if (pins & Z80_RD) {
                    Z80_SET_DATA(pins, advanced_memory_read(address));
                } else if (pins & Z80_WR) {
                    advanced_memory_write(address, Z80_GET_DATA(pins));
                }
            } else if ((pins & (Z80_IORQ | Z80_M1)) ==
                       (Z80_IORQ | Z80_M1)) {
                // Mode-2 interrupt acknowledge: the CTC supplies the vector.
                Z80_SET_DATA(pins, ctc.acknowledge());
            } else if (pins & Z80_IORQ) {
                const uint8_t port = static_cast<uint8_t>(
                    Z80_GET_ADDR(pins) & 0xff);
                if (pins & Z80_RD) {
                    // CTC at 0x10-0x13; SIO/PIO reads idle high.
                    if (port >= 0x10 && port <= 0x13) {
                        Z80_SET_DATA(pins, ctc.read(port & 3));
                    } else {
                        Z80_SET_DATA(pins, 0xff);
                    }
                } else if (pins & Z80_WR) {
                    const uint8_t value = Z80_GET_DATA(pins);
                    if (port >= 0x10 && port <= 0x13) ctc.write(port & 3, value);
                    // SIO (0x18-0x1b) and PIO (0x1c-0x1f) writes are accepted
                    // but not modelled; the firmware only needs the CTC.
                }
            }
            if (pins & Z80_RETI) ctc.return_from_interrupt();
        }
        total_clocks += static_cast<uint64_t>(clocks);
    }

    void set_gun_inputs(const input_state& state) {
        uint8_t in0 = 0xff, in1 = 0xff;
        if (state.coin1) in0 &= ~uint8_t{0x01};
        if (state.coin2) in0 &= ~uint8_t{0x02};
        if (state.test) in0 &= ~uint8_t{0x04};
        if (state.service) in0 &= ~uint8_t{0x08};
        if (state.start) in0 &= ~uint8_t{0x10};   // START1
        if (state.p2_start) in0 &= ~uint8_t{0x20}; // START2
        if (state.buttons[0]) in1 &= ~uint8_t{0x01};    // P1 trigger
        if (state.p2_buttons[0]) in1 &= ~uint8_t{0x02}; // P2 trigger
        digital_inputs[0] = in0;
        digital_inputs[1] = in1;
        digital_inputs[2] = 0xff; // IN2: DIP bank, all off = normal

        // 10-bit gun ADC ranges from MAME's vcop light-gun calibration.
        gun_ports[0] = scale_gun(state.left_stick_y, 0x024, 0x1a9); // P1Y
        gun_ports[1] = scale_gun(state.left_stick_x, 0x083, 0x276); // P1X
        gun_ports[2] = scale_gun(state.p2_stick_y, 0x027, 0x1a9);   // P2Y
        gun_ports[3] = scale_gun(state.p2_stick_x, 0x080, 0x273);   // P2X

        // Off-screen (reload): the reload button forces the gun off-screen.
        gun_offscreen = 0xfc;
        if (state.buttons[1]) gun_offscreen |= 0x01;
        if (state.p2_buttons[1]) gun_offscreen |= 0x02;
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

    // Advanced (model1io2 / TMPZ84C015) board state.
    z80_ctc ctc;
    unsigned fpga_counter{};
    // Gun coordinates in FPGA read order: P1Y, P1X, P2Y, P2X (10-bit each).
    std::array<uint16_t, 4> gun_ports{};
    uint8_t gun_offscreen{0xfc}; // FPGA offset 8: bit0 P1 off-screen, bit1 P2.

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
void model1_io_board::set_gun_inputs(const input_state& state) {
    m_impl->set_gun_inputs(state);
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
