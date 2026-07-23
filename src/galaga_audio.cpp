#include "galaxian_audio.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace {

class galaga_wsg_synth final : public galaxian_sound_synth {
public:
    explicit galaga_wsg_synth(const std::array<uint8_t, 0x100>& waveform)
        : waves(waveform) {}

    void reset() override {
        registers.fill(0);
        phase.fill(0);
    }

    void write_control(unsigned port, uint8_t data) override {
        registers[port & 0x1f] = data & 0x0f;
    }

    void generate(int16_t* output, int frames, int music_volume,
                  int effects_volume) override {
        if (!output || frames <= 0) return;
        const std::array<uint32_t, 3> frequency{
            static_cast<uint32_t>(reg(0x10) | (reg(0x11) << 4) |
                (reg(0x12) << 8) | (reg(0x13) << 12) |
                (reg(0x14) << 16)),
            static_cast<uint32_t>((reg(0x16) << 4) | (reg(0x17) << 8) |
                (reg(0x18) << 12) | (reg(0x19) << 16)),
            static_cast<uint32_t>((reg(0x1b) << 4) | (reg(0x1c) << 8) |
                (reg(0x1d) << 12) | (reg(0x1e) << 16)),
        };
        const std::array<uint8_t, 3> wave{
            static_cast<uint8_t>(reg(0x05) & 7),
            static_cast<uint8_t>(reg(0x0a) & 7),
            static_cast<uint8_t>(reg(0x0f) & 7),
        };
        const std::array<uint8_t, 3> volume{
            reg(0x15), reg(0x1a), reg(0x1f)};
        const int level = std::clamp((music_volume + effects_volume) / 2,
                                     0, 100);
        for (int sample_index = 0; sample_index < frames; ++sample_index) {
            int mixed = 0;
            for (unsigned voice = 0; voice < 3; ++voice) {
                // The custom chip is clocked at 96 kHz, twice host rate.
                phase[voice] =
                    (phase[voice] + frequency[voice] * 2) & 0xfffff;
                const unsigned index = (phase[voice] >> 15) & 0x1f;
                mixed += (static_cast<int>(
                    waves[wave[voice] * 32 + index] & 0x0f) - 8) *
                    volume[voice];
            }
            output[sample_index] = static_cast<int16_t>(
                std::clamp(mixed * 64 * level / 100, -32768, 32767));
        }
    }

    int active_tune() const override { return 0; }

private:
    uint8_t reg(unsigned index) const { return registers[index]; }
    std::array<uint8_t, 0x100> waves{};
    std::array<uint8_t, 0x20> registers{};
    std::array<uint32_t, 3> phase{};
};

} // namespace

std::unique_ptr<galaxian_sound_synth> make_galaga_sound_synth(
        const std::array<uint8_t, 0x100>& waveform) {
    return std::make_unique<galaga_wsg_synth>(waveform);
}
