// Standalone Yamaha SCSP DSP core.
// Adapted from MAME's BSD-3-Clause SCSPDSP implementation by ElSemi and
// R. Belmont; stripped of the MAME device/address-space framework.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class model2_scsp_dsp {
public:
    void reset(std::vector<uint8_t>* sound_ram);
    void set_ring_buffer(uint32_t base, uint32_t words);
    void write_word(uint16_t address, uint16_t value);
    uint16_t read_word(uint16_t address) const;
    void set_sample(int32_t sample, unsigned input);
    void step();

    const std::array<int16_t, 16>& effects() const { return m_effects; }

private:
    uint16_t ram_word(uint32_t address) const;
    void set_ram_word(uint32_t address, uint16_t value);
    void start();

    std::vector<uint8_t>* m_sound_ram{};
    uint32_t m_ring_base{};
    uint32_t m_ring_words{8 * 1024};
    std::array<int16_t, 64> m_coefficients{};
    std::array<uint16_t, 32> m_addresses{};
    std::array<uint16_t, std::size_t{128} * 4> m_program{};
    std::array<int32_t, 128> m_temporary{};
    std::array<int32_t, 32> m_memory{};
    std::array<int32_t, 16> m_inputs{};
    std::array<int16_t, 2> m_external{};
    std::array<int16_t, 16> m_effects{};
    uint32_t m_decay{};
    bool m_stopped{true};
    int m_last_step{};
};
