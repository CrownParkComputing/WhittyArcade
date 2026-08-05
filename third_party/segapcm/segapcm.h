// Standalone Sega 16-channel 8-bit PCM chip.
//
// Behaviour translated from MAME's BSD-3-Clause segapcm device (Hiromitsu
// Shioya / Olivier Galibert) into a non-MAME-framework API that matches
// this codebase's other sound-core style (ymfm, model1_audio,
// ymfm). The register and stream-update logic byte-for-byte mirrors the
// upstream device body in src/devices/sound/segapcm.cpp; the ROM read path
// is replaced with a caller-supplied byte pointer (MAME uses its device_rom
// framework).
//
// License: BSD-3-Clause. See LICENSE.
//
// Chip summary:
//   - 16 channels of 8-bit signed linear PCM.
//   - Per-channel 11-bit address: high 8 bits stored in RAM, low 8 bits are
//     held in a small shadow register `m_low[16]`.
//   - Bank bits in 0x1000-0x1fff selects the upper address bits per
//     chips' bank mode (BANK_256 / BANK_512 / BANK_12M).
//   - Generator frequency = `clock / (16 * (count + 1))`.

#ifndef MANX_SEGAPCM_H
#define MANX_SEGAPCM_H

#include <cstdint>
#include <cstddef>

class segapcm {
public:
    static constexpr int BANK_256    = 11;
    static constexpr int BANK_512    = 12;
    static constexpr int BANK_12M    = 13;
    static constexpr unsigned BANK_MASK7  = 0x70u << 16;
    static constexpr unsigned BANK_MASKF  = 0xf0u << 16;
    static constexpr unsigned BANK_MASKF8 = 0xf8u << 16;

    segapcm() = default;

    void set_bank(int bank);
    void reset();

    // Map the 64K PCM ROM the chip reads from. The pointer must outlive the
    // chip instance.
    void set_rom(const uint8_t* rom, std::size_t bytes) noexcept;

    // Mirror of MAME device_t::write.
    void write(unsigned offset, uint8_t data) noexcept;
    uint8_t read(unsigned offset) const noexcept;

    // Sample-clock-rate configuration.  Default 4 MHz, matching the Sega
    // System 16B and YM2151 rates.
    void set_clock(unsigned clock_hz) noexcept { m_clock = clock_hz; }

    // Mix `frames` 16-bit stereo frames. `out_left` / `out_right` arrays
    // must each hold `frames` int16_t samples; samples are accumulated
    // (added) to allow mixing with ymfm/YM2151 output.
    void sound_stream_update(int16_t* out_left, int16_t* out_right,
                             int frames, int output_sample_rate) noexcept;

private:
    uint8_t        m_ram[0x1000]{};
    uint8_t        m_low[16]{};
    const uint8_t* m_rom{nullptr};
    std::size_t    m_rom_bytes{0};
    int            m_bankshift{12};
    unsigned       m_bankmask{BANK_MASKF};
    unsigned       m_clock{4'000'000};
};

#endif // MANX_SEGAPCM_H
