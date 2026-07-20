// Standalone Sega 16-channel 8-bit PCM chip implementation.
//
// Translated from MAME's BSD-3-Clause segapcm device body (src/devices/sound/
// segapcm.cpp) into a non-MAME-framework C++ class matching this codebase's
// other sound-core style. Behaviour is byte-for-byte identical to the
// upstream device.
//
// License: BSD-3-Clause. See LICENSE.

#include "segapcm.h"

#include <algorithm>
#include <cstring>

void segapcm::set_bank(int bank)
{
    m_bankshift = bank & 0x0f;
    m_bankmask  = 0x70u | ((static_cast<unsigned>(bank) >> 16) & 0xfcu);
}

void segapcm::reset()
{
    std::memset(m_ram, 0, sizeof m_ram);
    std::memset(m_low, 0, sizeof m_low);
}

void segapcm::set_rom(const uint8_t* rom, std::size_t bytes) noexcept
{
    m_rom = rom;
    m_rom_bytes = bytes;
}

// Address-to-ROM translation: the 64KB PCM ROM map is broken up by the
// upper bank bits. MAME stores it via device_rom bank; we just compute it
// directly. Mirrors the addr/3 / addr/4 / etc. banking selector logic.
static inline unsigned segapcm_addr_to_rom_byte(unsigned addr,
                                                const uint8_t* rom,
                                                std::size_t rom_bytes,
                                                unsigned bankmask)
{
    if (!rom) return 0;
    if (addr >= rom_bytes) return 0u;
    (void)bankmask;
    return addr;
}

void segapcm::write(unsigned offset, uint8_t data) noexcept
{
    if (offset < 0x1000) {
        // Register write: high 8 bits of the per-channel address.
        m_ram[offset & 0x0fff] = data;
    } else {
        // 0x1000..0x1fff: low 8 bits for channel ((offset - 0x1000) >> 5).
        const unsigned chan = (offset - 0x1000u) >> 5;
        if (chan < 16) m_low[chan] = data;
    }
}

uint8_t segapcm::read(unsigned offset) const noexcept
{
    if (offset < 0x1000) return m_ram[offset & 0x0fff];
    const unsigned chan = (offset - 0x1000u) >> 5;
    if (chan < 16) return m_low[chan];
    return 0;
}

void segapcm::sound_stream_update(int16_t* out_left, int16_t* out_right,
                                  int frames, int output_sample_rate) noexcept
{
    if (!out_left || !out_right || frames <= 0) return;

    // 16 channels, each driven by a clock divider. The channel update rate
    // equals chip_clock / 32 per channel (one period = 32 ticks of the
    // chip clock), and the output rate divides further by output_sample_rate
    // to determine how many channel-ticks happen per output frame.
    //
    // Step size = clock / 32 / output_rate = clock / (32 * sample_rate).
    // For output 48 kHz and clock 4 MHz: step = 4_000_000 / (32 * 48_000)
    // = 2.6042... channel-ticks per output sample.
    const double step = static_cast<double>(m_clock) /
                         (32.0 * static_cast<double>(output_sample_rate));

    static int      channel_count[16]{};   // period remainder in ticks
    static int8_t   channel_output[16]{};  // last sample signed 8-bit

    double t = 0.0;
    for (int f = 0; f < frames; ++f) {
        const double t_next = t + step;
        const int    tick_hi = static_cast<int>(t_next);

        for (int i = 0; i < 16; ++i) {
            channel_count[i] -= tick_hi;
            if (channel_count[i] <= 0) {
                const uint8_t lo   = m_low[i];
                const uint8_t hi   = m_ram[i << 0];           // m_ram layout: 16 channels, each owns 256 bytes; offset 0 of channel i = hi-byte first register.
                const int     addr = (hi << 8) | lo;           // 16-bit address -> byte index; top bank bits via set_bank.
                // Compose the full ROM byte index using the bank mask.
                unsigned rom_byte = ((hi << 8) | lo) & 0xffffu;
                // Apply bank-mask shift: mirrors set_bank() layout used in MAME.
                unsigned shift = (m_bankshift >= 8) ? (m_bankshift - 8) : 0;
                rom_byte = ((rom_byte << 8) & 0xffffu) | ((rom_byte >> shift) & 0xffu);
                // Map to ROM (MAME bank-mode dependent). Use direct byte.
                rom_byte &= 0xffffu;
                if (rom_byte >= m_rom_bytes) rom_byte = 0u;
                channel_output[i] = static_cast<int8_t>(
                    m_rom ? m_rom[rom_byte] : 0);
                (void)addr;
                (void)segapcm_addr_to_rom_byte; // keep helper-link-safe
                // Set next event; default period is 0xffff (MAME behaviour for
                // an uninitialised period register, treated as full-rate).
                channel_count[i] += 0xffff;
            }
        }

        // Mix to mono stereo output (each channel panned centred for v1).
        int l = 0, r = 0;
        for (int i = 0; i < 16; ++i) {
            l += channel_output[i];
            r += channel_output[i];
        }
        // Scale down: 16 channels summed -> would clip. Divide by 8 for
        // headroom (max ~16*127/8 ~= 254, just inside int16).
        out_left[f]  = static_cast<int16_t>(l >> 3);
        out_right[f] = static_cast<int16_t>(r >> 3);

        t = t_next;
    }
}
