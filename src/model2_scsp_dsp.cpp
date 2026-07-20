#include "model2_scsp_dsp.h"

#include <algorithm>

namespace {
int32_t sign_extend(uint32_t value, unsigned bits) {
    const uint32_t sign = uint32_t{1} << (bits - 1);
    return static_cast<int32_t>((value ^ sign) - sign);
}

uint16_t pack_float(int32_t value) {
    const int sign = (value >> 23) & 1;
    uint32_t probe = (static_cast<uint32_t>(value) ^
                      (static_cast<uint32_t>(value) << 1)) & 0xffffff;
    int exponent = 0;
    while (exponent < 12 && !(probe & 0x800000)) {
        probe <<= 1;
        ++exponent;
    }
    if (exponent < 12)
        value = (value << exponent) & 0x3fffff;
    else
        value <<= 11;
    value = (value >> 11) & 0x7ff;
    return static_cast<uint16_t>(value | (sign << 15) | (exponent << 11));
}

int32_t unpack_float(uint16_t value) {
    const int sign = value >> 15;
    int exponent = (value >> 11) & 15;
    uint32_t result = (value & 0x7ff) << 11;
    if (exponent > 11) {
        exponent = 11;
        result |= static_cast<uint32_t>(sign) << 22;
    } else {
        result |= static_cast<uint32_t>(sign ^ 1) << 22;
    }
    result |= static_cast<uint32_t>(sign) << 23;
    return sign_extend(result, 24) >> exponent;
}
}

void model2_scsp_dsp::reset(std::vector<uint8_t>* sound_ram) {
    m_sound_ram = sound_ram;
    m_ring_base = 0;
    m_ring_words = 8 * 1024;
    m_coefficients.fill(0);
    m_addresses.fill(0);
    m_program.fill(0);
    m_temporary.fill(0);
    m_memory.fill(0);
    m_inputs.fill(0);
    m_external.fill(0);
    m_effects.fill(0);
    m_decay = 0;
    m_stopped = true;
    m_last_step = 0;
}

void model2_scsp_dsp::set_ring_buffer(uint32_t base, uint32_t words) {
    m_ring_base = base & 0x3f;
    m_ring_words = std::max<uint32_t>(words, 1);
}

void model2_scsp_dsp::write_word(uint16_t address, uint16_t value) {
    if (address < 0x780)
        m_coefficients[(address - 0x700) >> 1] =
            static_cast<int16_t>(value);
    else if (address < 0x7c0)
        m_addresses[(address - 0x780) >> 1] = value;
    else if (address < 0x800)
        m_addresses[(address - 0x7c0) >> 1] = value;
    else if (address < 0xc00) {
        m_program[(address - 0x800) >> 1] = value;
        if (address == 0xbf0) start();
    }
}

uint16_t model2_scsp_dsp::read_word(uint16_t address) const {
    if (address < 0x780)
        return static_cast<uint16_t>(m_coefficients[(address - 0x700) >> 1]);
    if (address < 0x7c0) return m_addresses[(address - 0x780) >> 1];
    if (address < 0x800) return m_addresses[(address - 0x7c0) >> 1];
    if (address < 0xc00) return m_program[(address - 0x800) >> 1];
    if (address < 0xe00) {
        const int32_t value = m_temporary[(address >> 2) & 0x7f];
        return (address & 2) ? static_cast<uint16_t>(value) :
                              static_cast<uint16_t>(value >> 16);
    }
    if (address < 0xe80) {
        const int32_t value = m_memory[(address >> 2) & 0x1f];
        return (address & 2) ? static_cast<uint16_t>(value) :
                              static_cast<uint16_t>(value >> 16);
    }
    if (address < 0xec0) {
        const int32_t value = m_inputs[(address >> 2) & 15];
        return (address & 2) ? static_cast<uint16_t>(value) :
                              static_cast<uint16_t>(value >> 16);
    }
    if (address < 0xee0)
        return static_cast<uint16_t>(m_effects[(address - 0xec0) >> 1]);
    if (address < 0xee4)
        return static_cast<uint16_t>(m_external[(address - 0xee0) >> 1]);
    return 0;
}

uint16_t model2_scsp_dsp::ram_word(uint32_t address) const {
    if (!m_sound_ram || address + 1 >= m_sound_ram->size()) return 0;
    return static_cast<uint16_t>(((*m_sound_ram)[address] << 8) |
                                 (*m_sound_ram)[address + 1]);
}

void model2_scsp_dsp::set_ram_word(uint32_t address, uint16_t value) {
    if (!m_sound_ram || address + 1 >= m_sound_ram->size()) return;
    (*m_sound_ram)[address] = static_cast<uint8_t>(value >> 8);
    (*m_sound_ram)[address + 1] = static_cast<uint8_t>(value);
}

void model2_scsp_dsp::set_sample(int32_t sample, unsigned input) {
    if (input < m_inputs.size()) m_inputs[input] += sample;
}

void model2_scsp_dsp::start() {
    m_stopped = false;
    int step = 127;
    for (; step >= 0; --step) {
        const unsigned base = static_cast<unsigned>(step) * 4;
        if (m_program[base] || m_program[base + 1] ||
            m_program[base + 2] || m_program[base + 3]) break;
    }
    m_last_step = step + 1;
}

void model2_scsp_dsp::step() {
    if (m_stopped) return;
    m_effects.fill(0);
    int32_t accumulator = 0;
    int32_t memory_value = 0;
    int32_t fraction = 0;
    int32_t y_register = 0;
    uint32_t address_register = 0;

    for (int step_index = 0; step_index < m_last_step; ++step_index) {
        const uint16_t* instruction = &m_program[step_index * 4];
        const unsigned tra = (instruction[0] >> 8) & 0x7f;
        const bool temp_write = instruction[0] & 0x80;
        const unsigned temp_address = instruction[0] & 0x7f;
        const bool x_input = instruction[1] & 0x8000;
        const unsigned y_select = (instruction[1] >> 13) & 3;
        const unsigned input_address = (instruction[1] >> 6) & 0x3f;
        const bool memory_writeback = instruction[1] & 0x20;
        const unsigned memory_address = instruction[1] & 0x1f;
        const bool table = instruction[2] & 0x8000;
        const bool ram_write = instruction[2] & 0x4000;
        const bool ram_read = instruction[2] & 0x2000;
        const bool effect_write = instruction[2] & 0x1000;
        const unsigned effect_address = (instruction[2] >> 8) & 15;
        const bool address_load = instruction[2] & 0x80;
        const bool fraction_load = instruction[2] & 0x40;
        const unsigned shift = (instruction[2] >> 4) & 3;
        const bool y_load = instruction[2] & 8;
        const bool negate = instruction[2] & 4;
        const bool zero = instruction[2] & 2;
        const bool accumulator_b = instruction[2] & 1;
        const bool no_float = instruction[3] & 0x8000;
        const unsigned coefficient = (instruction[3] >> 9) & 0x3f;
        const unsigned ram_address = (instruction[3] >> 2) & 0x1f;
        const bool add_address = instruction[3] & 2;
        const bool next_address = instruction[3] & 1;

        int32_t input = 0;
        if (input_address <= 0x1f)
            input = m_memory[input_address];
        else if (input_address <= 0x2f)
            input = m_inputs[input_address - 0x20] << 4;
        else if (input_address <= 0x31)
            input = m_external[input_address - 0x30] << 8;
        else
            continue;
        input = sign_extend(static_cast<uint32_t>(input), 24);
        if (memory_writeback) {
            m_memory[memory_address] = memory_value;
            if (input_address == memory_address) input = memory_value;
        }

        int32_t b = 0;
        if (!zero) {
            b = accumulator_b ? accumulator :
                sign_extend(static_cast<uint32_t>(
                    m_temporary[(tra + m_decay) & 0x7f]), 24);
            if (negate) b = -b;
        }
        const int32_t x = x_input ? input : sign_extend(
            static_cast<uint32_t>(m_temporary[(tra + m_decay) & 0x7f]), 24);
        int32_t y = 0;
        if (y_select == 0) y = fraction;
        else if (y_select == 1) y = m_coefficients[coefficient] >> 3;
        else if (y_select == 2) y = (y_register >> 11) & 0x1fff;
        else y = (y_register >> 4) & 0x0fff;
        if (y_load) y_register = input;

        int32_t shifted = 0;
        if (shift == 0) shifted = std::clamp(accumulator, -0x800000, 0x7fffff);
        else if (shift == 1)
            shifted = std::clamp(accumulator * 2, -0x800000, 0x7fffff);
        else if (shift == 2)
            shifted = sign_extend(static_cast<uint32_t>(accumulator * 2), 24);
        else shifted = sign_extend(static_cast<uint32_t>(accumulator), 24);

        y = sign_extend(static_cast<uint32_t>(y), 13);
        accumulator = static_cast<int32_t>(
            (static_cast<int64_t>(x) * y >> 12) + b);
        if (temp_write)
            m_temporary[(temp_address + m_decay) & 0x7f] = shifted;
        if (fraction_load)
            fraction = shift == 3 ? shifted & 0xfff :
                                    (shifted >> 11) & 0x1fff;

        if (ram_read || ram_write) {
            uint32_t address = m_addresses[ram_address];
            if (!table) address += m_decay;
            if (add_address) address += address_register & 0xfff;
            if (next_address) ++address;
            address &= table ? 0xffff : (m_ring_words - 1);
            address = ((address + (m_ring_base << 12)) << 1);
            if (ram_read && (step_index & 1))
                memory_value = no_float ? ram_word(address) << 8 :
                                          unpack_float(ram_word(address));
            if (ram_write && (step_index & 1))
                set_ram_word(address, no_float ?
                    static_cast<uint16_t>(shifted >> 8) : pack_float(shifted));
        }
        if (address_load)
            address_register = shift == 3 ? (shifted >> 12) & 0xfff :
                                            static_cast<uint32_t>(input >> 16);
        if (effect_write)
            m_effects[effect_address] = static_cast<int16_t>(
                m_effects[effect_address] + (shifted >> 8));
    }
    --m_decay;
    m_inputs.fill(0);
}
