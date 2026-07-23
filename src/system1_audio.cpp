#include "galaxian_audio.h"

#include <algorithm>
#include <cstdint>

extern "C" int g88_ym2151_sample(void);

namespace {

class system1_sound_synth final : public galaxian_sound_synth {
public:
    void reset() override {}
    void write_control(unsigned, uint8_t) override {}
    void generate(int16_t* output, int frames, int music_volume,
                  int effects_volume) override {
        const int level =
            std::clamp((music_volume * 3 + effects_volume) / 4, 0, 100);
        for (int i = 0; i < frames; ++i)
            output[i] = static_cast<int16_t>(std::clamp(
                g88_ym2151_sample() * level / 100, -32768, 32767));
    }
    int active_tune() const override { return 0; }
};

} // namespace

std::unique_ptr<galaxian_sound_synth> make_system1_sound_synth() {
    return std::make_unique<system1_sound_synth>();
}
