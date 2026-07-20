// license:BSD-3-Clause
// Geometry-board mapping based on MAME's Model 1 implementation.
#include "model1_tgp_lle.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

uint32_t model1_tgp_lle::read_word(const std::vector<uint8_t>& bytes,
                                   std::size_t word) {
    const std::size_t offset = word * 4;
    if (offset + 3 >= bytes.size()) return 0;
    return uint32_t{bytes[offset]} |
           (uint32_t{bytes[offset + 1]} << 8) |
           (uint32_t{bytes[offset + 2]} << 16) |
           (uint32_t{bytes[offset + 3]} << 24);
}

model1_tgp_lle::model1_tgp_lle(
    const std::vector<uint8_t>& program,
    const std::vector<uint8_t>& copro_data,
    const std::vector<uint8_t>& lookup_tables,
    bool legacy_external_map,
    bool legacy_transfer_order)
    : m_legacy_external_map(legacy_external_map),
      m_legacy_transfer_order(legacy_transfer_order),
      m_use_legacy_core(legacy_external_map || legacy_transfer_order) {
    m_trace = std::getenv("MODEL1_TGP_TRACE") != nullptr;
    m_program.resize(program.size() / 4);
    for (std::size_t i = 0; i < m_program.size(); ++i)
        m_program[i] = read_word(program, i);
    m_copro_data.resize(copro_data.size() / 4);
    for (std::size_t i = 0; i < m_copro_data.size(); ++i)
        m_copro_data[i] = read_word(copro_data, i);
    // The first 256 KiB of this region is the 32-bit arithmetic table ROM.
    const std::size_t table_words = std::min<std::size_t>(
        lookup_tables.size() / 4, 0x10000);
    m_tables.resize(table_words);
    for (std::size_t i = 0; i < m_tables.size(); ++i)
        m_tables[i] = read_word(lookup_tables, i);

    const auto configure_core = [this](auto& core) {
        core.set_program_callbacks([this](uint16_t address) {
            return m_program.empty() ? 0 :
                m_program[address % m_program.size()];
        });
        core.set_data_callbacks(
            [this](uint16_t address) { return data_read(address); },
            [this](uint16_t address, uint32_t value) {
                data_write(address, value);
            });
        core.set_io_callbacks(
            [this](uint16_t address) { return io_read(address); },
            [this](uint16_t address, uint32_t value) {
                io_write(address, value);
            });
        core.set_rf_callbacks(
            [this](uint16_t address) { return rf_read(address); },
            [this](uint16_t address, uint32_t value) {
                rf_write(address, value);
            });
    };
    configure_core(m_core);
    configure_core(m_native_core);
    // vr-tgp.bin (the historical BAD_DUMP/reconstruction) uses the glue
    // chip's RF1/RF2 ports for input/output. RF0 drives status LEDs. Later
    // internal-program images
    // use data-space 0x100/0x400 instead; supporting both is harmless.
    reset();
}

void model1_tgp_lle::reset() {
    std::fill(m_internal_ram.begin(), m_internal_ram.end(), 0);
    std::fill(m_shared_ram.begin(), m_shared_ram.end(), 0);
    m_input_fifo.clear();
    m_output_fifo.clear();
    m_v60_ram_address = 0;
    m_v60_ram_latch[0] = m_v60_ram_latch[1] = 0;
    m_external_base = 0;
    m_external_ports.fill(0);
    m_native_ram_address.fill(0);
    m_native_atan_base.fill(0);
    m_native_sincos_base = 0;
    m_native_inv_base = 0;
    m_native_isqrt_base = 0;
    m_native_data_base = 0;
    m_last_source_pc = 0;
    m_core.reset();
    m_native_core.reset();
}

void model1_tgp_lle::stall_core() {
    if (m_use_legacy_core) m_core.stall();
    else m_native_core.stall();
}

void model1_tgp_lle::set_core_gpio0(bool state) {
    if (m_use_legacy_core) m_core.set_gpio0(state);
    else m_native_core.set_gpio0(state);
}

void model1_tgp_lle::push(uint32_t value, uint32_t source_pc) {
    m_last_source_pc = source_pc;
    m_input_fifo.push_back(value);
    if (m_trace)
        std::fprintf(stderr, "TGP LLE input %08x from %06x depth=%zu\n",
                     value, source_pc & 0xffffff, m_input_fifo.size());
}

uint32_t model1_tgp_lle::pop_output() {
    if (m_output_fifo.empty()) return 0xffffffff;
    const uint32_t value = m_output_fifo.front();
    m_output_fifo.pop_front();
    if (m_trace)
        std::fprintf(stderr, "TGP LLE V60 output %08x depth=%zu\n",
                     value, m_output_fifo.size());
    return value;
}

uint16_t model1_tgp_lle::read_ram_half(unsigned half) {
    const uint32_t value = m_shared_ram[m_v60_ram_address & 0x1fff];
    const uint16_t result = static_cast<uint16_t>(value >> ((half & 1) * 16));
    if ((half & 1) && (m_v60_ram_address & 0x8000)) ++m_v60_ram_address;
    return result;
}

void model1_tgp_lle::write_ram_half(unsigned half, uint16_t value) {
    m_v60_ram_latch[half & 1] = value;
    if (half & 1) {
        m_shared_ram[m_v60_ram_address & 0x1fff] =
            uint32_t{m_v60_ram_latch[0]} |
            (uint32_t{m_v60_ram_latch[1]} << 16);
        if (m_v60_ram_address & 0x8000) ++m_v60_ram_address;
    }
}

uint32_t model1_tgp_lle::data_read(uint16_t address) {
    if (address == 0x100) {
        if (m_input_fifo.empty()) {
            stall_core();
            return 0;
        }
        const uint32_t value = m_input_fifo.front();
        m_input_fifo.pop_front();
        if (m_trace)
            std::fprintf(stderr, "TGP LLE consume %08x at %04x depth=%zu\n",
                         value, geometry_pc(), m_input_fifo.size());
        return value;
    }
    if (address < 0x100 || (address >= 0x200 && address < 0x400))
        return m_internal_ram[address];
    return 0;
}

void model1_tgp_lle::data_write(uint16_t address, uint32_t value) {
    if (address == 0x400) {
        m_output_fifo.push_back(value);
        if (m_trace)
            std::fprintf(stderr, "TGP LLE produce %08x at %04x depth=%zu\n",
                         value, geometry_pc(), m_output_fifo.size());
        return;
    }
    if (address < 0x100 || (address >= 0x200 && address < 0x400))
        m_internal_ram[address] = value;
}

uint32_t model1_tgp_lle::rf_read(uint16_t address) {
    if (!m_legacy_external_map) return 0;
    if ((address & 0x1f) == 3) return m_external_base;
    if ((address & 0x1f) != 1) return 0;
    if (m_input_fifo.empty()) {
        stall_core();
        return 0;
    }
    const uint32_t value = m_input_fifo.front();
    m_input_fifo.pop_front();
    if (m_trace)
        std::fprintf(stderr, "TGP LLE RF consume %08x at %04x depth=%zu\n",
                     value, geometry_pc(), m_input_fifo.size());
    return value;
}

void model1_tgp_lle::rf_write(uint16_t address, uint32_t value) {
    if (!m_legacy_external_map) return;
    if ((address & 0x1f) == 3) {
        m_external_base = value;
        return;
    }
    if ((address & 0x1f) != 2) return;
    m_output_fifo.push_back(value);
    if (m_trace)
        std::fprintf(stderr, "TGP LLE RF produce %08x at %04x depth=%zu\n",
                     value, geometry_pc(), m_output_fifo.size());
}

uint32_t model1_tgp_lle::table_word(std::size_t index) const {
    return m_tables.empty() ? 0 : m_tables[index % m_tables.size()];
}

uint32_t model1_tgp_lle::copro_word(std::size_t index) const {
    return m_copro_data.empty() ? 0 : m_copro_data[index % m_copro_data.size()];
}

namespace {
uint32_t legacy_scale_exponent(uint32_t value, int adjustment) {
    // The reconstructed 315-5573 program was paired with the original table
    // device's integer exponent adder.  It deliberately does not clamp to an
    // eight-bit exponent before shifting: overflow becomes the float sign bit
    // and underflow wraps in the same 32-bit datapath.  Converting through
    // uint32_t keeps those hardware wrap semantics defined in C++.
    const int exponent = static_cast<int>((value >> 23) & 0xff) + adjustment;
    value &= ~uint32_t{0x7f800000};
    return value | (static_cast<uint32_t>(exponent) << 23);
}
} // namespace

uint32_t model1_tgp_lle::io_read(uint16_t address) {
    if (!m_legacy_external_map) {
        if ((address & ~uint16_t{0x18}) == 0) {
            return m_native_ram_address[(address >> 3) & 3];
        }
        if ((address & ~uint16_t{0x18}) == 1) {
            uint32_t& ram_address =
                m_native_ram_address[(address >> 3) & 3];
            const std::size_t index = m_legacy_transfer_order &&
                (ram_address & 0x40000) ?
                0x1000 | (ram_address & 0x1fff) : ram_address & 0x1fff;
            const uint32_t value = m_shared_ram[index];
            ram_address += m_legacy_transfer_order ? 1 :
                           ((ram_address & 0x40000) ? 4 : 1);
            return value;
        }
        if (address >= 0x20 && address <= 0x23) {
            const uint32_t angle = m_native_sincos_base +
                uint32_t{address - 0x20} * 0x4000;
            uint32_t index = angle & 0x3fff;
            if (angle & 0x4000)
                index = std::min<uint32_t>(0x4000 - index, 0x3fff);
            uint32_t value = table_word(index);
            if (angle & 0x8000) value ^= 0x80000000;
            return value;
        }
        if (address >= 0x24 && address <= 0x27) {
            uint32_t index = m_native_atan_base[3] & 0xffff;
            if (index & 0xc000) index = 0x3fff;
            uint32_t result = table_word(index | 0x4000);

            // The original table has three packed BCD-like correction bits;
            // the surrounding Sega logic removes their carry artifacts.
            const uint16_t delta = static_cast<uint16_t>(
                (result >> 16) + result);
            if (delta & 0x001)
                result -= (result & 0x00f) == 0x00e ? 1 : 0x00010000;
            if (delta & 0x010)
                result -= (result & 0x0f0) == 0x0e0 ? 0x10 : 0x00100000;
            if (delta & 0x100)
                result -= (result & 0xf00) == 0xe00 ? 0x100 : 0x01000000;

            const bool sign0 = (m_native_atan_base[0] & 0x80000000) != 0;
            const bool sign1 = (m_native_atan_base[1] & 0x80000000) != 0;
            const bool sign2 = (m_native_atan_base[2] & 0x80000000) != 0;
            if (sign0 ^ sign1 ^ sign2) result >>= 16;
            if (sign2) result += 0x4000;
            if ((sign0 && !sign2) || (sign1 && sign2)) result += 0x8000;
            return result & 0xffff;
        }
        if (address >= 0x28 && address <= 0x29) {
            const uint32_t lane = address & 1;
            const uint32_t index = ((m_native_inv_base >> 9) & 0x3ffe) | lane;
            uint32_t result = table_word(index | 0x8000);
            const uint8_t base_exponent =
                static_cast<uint8_t>(m_native_inv_base >> 23);
            const uint8_t exponent = static_cast<uint8_t>(
                (result >> 23) + (0x7f - base_exponent));
            result = (result & 0x807fffff) | (uint32_t{exponent} << 23);
            if (m_native_inv_base & 0x80000000) result ^= 0x80000000;
            return result;
        }
        if (address >= 0x2a && address <= 0x2b) {
            const uint32_t lane = address & 1;
            const uint32_t index = 0x2000 ^
                (((m_native_isqrt_base >> 10) & 0x3ffe) | lane);
            uint32_t result = table_word(index | 0xc000);
            const uint8_t base_exponent = static_cast<uint8_t>(
                (m_native_isqrt_base >> 24) & 0x7f);
            const uint8_t exponent = static_cast<uint8_t>(
                (result >> 23) + (0x3f - base_exponent));
            result = (result & 0x807fffff) | (uint32_t{exponent} << 23);
            if (!lane) result &= 0x7fffffff;
            return result;
        }
        if (address >= 0x8000) {
            const std::size_t index =
                (m_native_data_base & ~uint32_t{0x7fff}) | address;
            return copro_word(index);
        }
        return 0;
    }

    // This reconstructed program uses RF3 as the upper half of its unified
    // external address. Its map exposes shared RAM at 0x00400000 and the
    // 512K-word geometry-data ROM at 0xff800000.
    if (m_external_base != 0) {
        const uint32_t effective =
            (m_external_base & 0xffff0000) | uint32_t{address};
        if (effective >= 0x00400000 && effective <= 0x00407fff)
            return m_shared_ram[effective - 0x00400000];
        if (effective >= 0xff800000 && effective <= 0xff87ffff)
            return copro_word(effective - 0xff800000);
        return 0;
    }
    if (address < 0x0020 || address > 0x002f) return 0;

    if (address <= 0x0023) {
        const uint32_t angle = m_external_ports[0] +
            uint32_t{address - 0x20} * 0x4000;
        const uint32_t phase = angle & 0x7fff;
        uint32_t result = 0;
        if (phase == 0x4000) {
            result = 0x3f800000;
        } else if (phase != 0) {
            uint32_t index = angle & 0x3fff;
            if (angle & 0x4000) index = 0x4000 - index;
            result = table_word(index);
        }
        if (angle & 0x8000) result |= 0x80000000;
        return result;
    }

    if (address == 0x0027) {
        const uint32_t value = m_external_ports[7];
        const uint32_t a = m_external_ports[4];
        const uint32_t b = m_external_ports[5];
        const uint32_t exponent = (value >> 23) & 0xff;
        uint32_t result = 0;

        if (exponent == 0) {
            if ((a & 0x7fffffff) <= (b & 0x7fffffff))
                return (b & 0x80000000) ? 0xc000 : 0x4000;
            return (a & 0x80000000) ? 0x8000 : 0;
        }

        const unsigned half = ((a ^ b) & 0x80000000) ? 16 : 0;
        uint32_t index;
        if ((exponent & 0x70) != 0x70) {
            index = 0;
        } else if (exponent < 0x70 || exponent > 0x7e) {
            index = 0x3fff;
        } else {
            const unsigned exponent_difference = exponent > 0x71 ?
                exponent - 0x71 : 0;
            const uint32_t base = uint32_t{1} << exponent_difference;
            const uint32_t mask = base - 1;
            index = base + ((value >> (23 - exponent_difference)) & mask);
        }
        result = (table_word(index | 0x4000) >> half) & 0xffff;
        if ((a & 0x7fffffff) <= (b & 0x7fffffff)) result = 0x4000 - result;
        if ((a & 0x80000000) && (b & 0x80000000))
            result |= 0x8000;
        else if ((a & 0x80000000) && !(b & 0x80000000))
            result &= 0x7fff;
        else if (!(a & 0x80000000) && (b & 0x80000000))
            result |= 0x8000;
        return result & 0xffff;
    }

    if (address >= 0x0028 && address <= 0x0029) {
        const uint32_t source = m_external_ports[8];
        const uint32_t lane = address - 0x28;
        const uint32_t index = ((source >> 9) & 0x3ffe) | lane;
        uint32_t result = table_word(index | 0x8000) & 0x7fffffff;
        if (lane && (source & 0x80000000)) result |= 0x80000000;
        return legacy_scale_exponent(
            result, 0x7f - static_cast<int>((source >> 23) & 0xff));
    }

    if (address >= 0x002a && address <= 0x002b) {
        const uint32_t source = m_external_ports[0x0a];
        const uint32_t lane = address - 0x2a;
        const uint32_t index = 0x2000 ^
            (((source >> 10) & 0x3ffe) | lane);
        uint32_t result = table_word(index | 0xc000) & 0x7fffffff;
        if (lane && (source & 0x80000000)) result |= 0x80000000;
        return legacy_scale_exponent(
            result, 0x3f - static_cast<int>((source >> 24) & 0x7f));
    }
    return m_external_ports[address - 0x20];
}

void model1_tgp_lle::io_write(uint16_t address, uint32_t value) {
    if (!m_legacy_external_map) {
        if ((address & ~uint16_t{0x18}) == 0) {
            m_native_ram_address[(address >> 3) & 3] = value;
            return;
        }
        if ((address & ~uint16_t{0x18}) == 1) {
            uint32_t& ram_address =
                m_native_ram_address[(address >> 3) & 3];
            const std::size_t index = m_legacy_transfer_order &&
                (ram_address & 0x40000) ?
                0x1000 | (ram_address & 0x1fff) : ram_address & 0x1fff;
            m_shared_ram[index] = value;
            ram_address += m_legacy_transfer_order ? 1 :
                           ((ram_address & 0x40000) ? 4 : 1);
            return;
        }
        if (address >= 0x20 && address <= 0x23) {
            m_native_sincos_base = value;
            return;
        }
        if (address >= 0x24 && address <= 0x27) {
            m_native_atan_base[address - 0x24] = value;
            return;
        }
        if (address >= 0x28 && address <= 0x29) {
            m_native_inv_base = value;
            return;
        }
        if (address >= 0x2a && address <= 0x2b) {
            m_native_isqrt_base = value;
            return;
        }
        if (address == 0x2e) m_native_data_base = value;
        return;
    }

    if (m_external_base != 0) {
        const uint32_t effective =
            (m_external_base & 0xffff0000) | uint32_t{address};
        if (effective >= 0x00400000 && effective <= 0x00407fff)
            m_shared_ram[effective - 0x00400000] = value;
        return;
    }
    if (address < 0x0020 || address > 0x002f) return;
    m_external_ports[address - 0x20] = value;
    if (address == 0x0024 || address == 0x0025) {
        set_core_gpio0((m_external_ports[4] & 0x7fffffff) <=
                       (m_external_ports[5] & 0x7fffffff));
    }
}
