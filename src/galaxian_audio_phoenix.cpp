// galaxian_audio_phoenix.cpp - Phoenix sound synth, implements
// galaxian_sound_synth. Original code lifted from phoenix_audio.cpp's
// phoenix_sound_synth.

#include "galaxian_audio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace {

struct note_pair {
    uint16_t lead;
    uint16_t bass;
};

// MM6221AA note values are the original TMS36xx ratios scaled by
// Phoenix's 372 Hz oscillator. Names retain MAME's established tune
// table notation.
#define C(n)  static_cast<uint16_t>(372.0 * (1u << ((n)-1)) * 1.18921)
#define Cx(n) static_cast<uint16_t>(372.0 * (1u << ((n)-1)) * 1.25992)
#define D(n)  static_cast<uint16_t>(372.0 * (1u << ((n)-1)) * 1.33484)
#define Dx(n) static_cast<uint16_t>(372.0 * (1u << ((n)-1)) * 1.41421)
#define E(n)  static_cast<uint16_t>(372.0 * (1u << ((n)-1)) * 1.49831)
#define F(n)  static_cast<uint16_t>(372.0 * (1u << ((n)-1)) * 1.58740)
#define Fx(n) static_cast<uint16_t>(372.0 * (1u << ((n)-1)) * 1.68179)
#define G(n)  static_cast<uint16_t>(372.0 * (1u << ((n)-1)) * 1.78180)
#define Gx(n) static_cast<uint16_t>(372.0 * (1u << ((n)-1)) * 1.88775)
#define A(n)  static_cast<uint16_t>(372u * (1u << (n)))
#define Ax(n) static_cast<uint16_t>(372.0 * (1u << (n)) * 1.05946)
#define B(n)  static_cast<uint16_t>(372.0 * (1u << (n)) * 1.12246)

constexpr std::array<note_pair, 32> alarm_tune{{
    {C(3), C(2)}, {G(3),0}, {C(3),0}, {G(3),0},
    {C(3),0}, {G(3),0}, {C(3),0}, {G(3),0},
    {C(3), C(4)}, {G(3),0}, {C(3),0}, {G(3),0},
    {C(3),0}, {G(3),0}, {C(3),0}, {G(3),0},
    {C(3), C(2)}, {G(3),0}, {C(3),0}, {G(3),0},
    {C(3),0}, {G(3),0}, {C(3),0}, {G(3),0},
    {C(3), C(4)}, {G(3),0}, {C(3),0}, {G(3),0},
    {C(3),0}, {G(3),0}, {C(3),0}, {G(3),0},
}};

constexpr std::array<note_pair, 48> fur_elise_tune{{
    {D(3),0}, {Cx(3),0}, {D(3),0}, {Cx(3),0}, {D(3),0}, {A(2),0},
    {C(3),0}, {Ax(2),0}, {G(2),0}, {D(1),0}, {G(1),0}, {Ax(1),0},
    {D(2),0}, {G(2),0}, {A(2),0}, {D(1),0}, {A(1),0}, {D(2),0},
    {Fx(2),0}, {A(2),0}, {Ax(2),0}, {D(1),0}, {G(1),0}, {Ax(1),0},
    {D(3),0}, {Cx(3),0}, {D(3),0}, {Cx(3),0}, {D(3),0}, {A(2),0},
    {C(3),0}, {Ax(2),0}, {G(2),0}, {D(1),0}, {G(1),0}, {Ax(1),0},
    {D(2),0}, {G(2),0}, {A(2),0}, {D(1),0}, {A(1),0}, {D(2),0},
    {Ax(2),0}, {A(2),0}, {0,G(2)}, {D(1),0}, {G(1),0}, {0,0},
}};

constexpr std::array<note_pair, 96> phoenix_theme{{
    {A(2),D(1)}, {0,0}, {A(2),0}, {0,0}, {A(2),0}, {0,0},
    {A(2),A(1)}, {0,0}, {G(2),0}, {0,0}, {F(2),0}, {0,0},
    {F(2),F(1)}, {0,0}, {E(2),F(1)}, {0,0}, {D(2),F(1)}, {0,0},
    {D(2),A(1)}, {0,0}, {F(2),0}, {0,0}, {A(2),0}, {0,0},
    {D(3),D(1)}, {0,0}, {0,D(1)}, {0,F(1)}, {0,A(1)}, {0,D(2)},
    {D(3),D(1)}, {0,0}, {C(3),0}, {0,0}, {Ax(2),0}, {0,0},
    {Ax(2),Ax(1)}, {0,0}, {A(2),0}, {0,0}, {G(2),0}, {0,0},
    {G(2),G(1)}, {0,0}, {A(2),0}, {0,0}, {Ax(2),0}, {0,0},
    {A(2),A(1)}, {0,0}, {Ax(2),0}, {0,0}, {A(2),0}, {0,0},
    {Cx(3),A(1)}, {0,0}, {Ax(2),0}, {0,0}, {A(2),0}, {0,0},
    {A(2),F(1)}, {0,0}, {G(2),0}, {0,0}, {F(2),0}, {0,0},
    {F(2),D(1)}, {0,0}, {E(2),0}, {0,0}, {D(2),0}, {0,0},
    {E(2),E(1)}, {0,0}, {E(2),0}, {0,0}, {E(2),0}, {0,0},
    {E(2),Ax(1)}, {0,0}, {F(2),0}, {0,0}, {E(2),F(1)}, {0,0},
    {D(2),D(1)}, {0,0}, {F(2),A(1)}, {0,0}, {A(2),F(1)}, {0,0},
    {D(3),D(1)}, {0,0}, {0,0}, {0,0}, {0,0}, {0,0},
}};

#undef C
#undef Cx
#undef D
#undef Dx
#undef E
#undef F
#undef Fx
#undef G
#undef Gx
#undef A
#undef Ax
#undef B

note_pair tune_note(int tune, int step) {
    if (step < 0) return {};
    if (tune == 1 && step < static_cast<int>(alarm_tune.size()))
        return alarm_tune[static_cast<std::size_t>(step)];
    if (tune == 2 && step < static_cast<int>(fur_elise_tune.size()))
        return fur_elise_tune[static_cast<std::size_t>(step)];
    if (tune == 3 && step < static_cast<int>(phoenix_theme.size()))
        return phoenix_theme[static_cast<std::size_t>(step)];
    return {};
}

}  // namespace

class phoenix_sound_synth final : public galaxian_sound_synth {
public:
    struct voice {
        double phase{0.0};
        double frequency{0.0};
        double level{0.0};
        double decay_seconds{0.0};
    };

    void reset() override {
        *this = phoenix_sound_synth{};
        m_lead.decay_seconds = 0.50;
        m_bass.decay_seconds = 1.05;
        m_effect_1.decay_seconds = 0.30;
        m_effect_2.decay_seconds = 0.24;
    }

    void write_control(unsigned port, uint8_t data) override {
        if (port == 0) m_control_a = data;
        else if (port == 1) m_control_b = data;
    }

    int active_tune() const override { return m_tune; }

    void generate(int16_t* output, int frames, int music_volume,
                  int effects_volume) override;

private:
    static double square(voice& channel) {
        if (channel.frequency <= 0.0 || channel.level <= 0.0) return 0.0;
        channel.phase += channel.frequency / sample_rate;
        channel.phase -= std::floor(channel.phase);
        return (channel.phase < 0.5 ? 1.0 : -1.0) * channel.level;
    }

    static void decay(voice& channel) {
        if (channel.level <= 0.0) return;
        channel.level = std::max(
            0.0, channel.level - 1.0 / (channel.decay_seconds * sample_rate));
    }

    void update_controls() {
        const int selected_tune = (m_control_b >> 6) & 3;
        if (selected_tune != m_tune) {
            m_tune = selected_tune;
            m_tune_step = 0;
            m_tune_samples = 0;
            m_lead.level = 0.0;
            m_bass.level = 0.0;
        }

        if ((m_control_b & 0x3f) != (m_previous_b & 0x3f)) {
            const int divider = std::max(1, 16 - (m_control_b & 0x0f));
            m_effect_1.frequency = (m_control_b & 0x10 ? 7200.0 : 4300.0) /
                                   (divider * 2.0);
            m_effect_1_envelope = 1.0;
        }
        if ((m_control_a & 0x3f) != (m_previous_a & 0x3f)) {
            const int divider = std::max(1, 16 - (m_control_a & 0x0f));
            m_effect_2.frequency = 2600.0 / divider;
            m_effect_2_envelope = 1.0;
        }
        if ((m_control_a & 0xc0) != (m_previous_a & 0xc0))
            m_noise_envelope = 1.0;
        m_previous_a = m_control_a;
        m_previous_b = m_control_b;
    }

    void advance_tune() {
        if (m_tune == 0 || m_tune_step >= 96) return;
        const note_pair notes = tune_note(m_tune, m_tune_step++);
        if (notes.lead) {
            m_lead.frequency = notes.lead;
            m_lead.level = 1.0;
        }
        if (notes.bass) {
            m_bass.frequency = notes.bass;
            m_bass.level = 1.0;
        }
    }

    voice m_lead{};
    voice m_bass{};
    voice m_effect_1{};
    voice m_effect_2{};

    int m_tune{0};
    int m_tune_step{0};
    int m_tune_samples{0};

    double m_effect_1_envelope{0.0};
    double m_effect_2_envelope{0.0};
    double m_effect_2_lfo_phase{0.0};
    double m_noise_envelope{0.0};
    double m_noise_phase{0.0};
    uint32_t m_noise_lfsr{0x1ffff};

    uint8_t m_control_a{0};
    uint8_t m_control_b{0};
    uint8_t m_previous_a{0};
    uint8_t m_previous_b{0};
};

void phoenix_sound_synth::generate(int16_t* output, int frames,
                                   int music_volume, int effects_volume) {
    if (!output || frames <= 0) return;
    update_controls();
    const double music_gain = std::clamp(music_volume, 0, 100) / 100.0;
    const double effects_gain = std::clamp(effects_volume, 0, 100) / 100.0;

    for (int index = 0; index < frames; ++index) {
        if (m_tune != 0 && m_tune_samples-- <= 0) {
            advance_tune();
            m_tune_samples += static_cast<int>(sample_rate * 0.21);
        }

        double music = square(m_lead) * 0.32 + square(m_bass) * 0.23;
        decay(m_lead);
        decay(m_bass);

        m_effect_1.level = m_effect_1_envelope;
        double effects = square(m_effect_1) * 0.22;
        m_effect_1_envelope = std::max(0.0, m_effect_1_envelope -
                                             1.0 / (sample_rate * 0.30));

        m_effect_2_lfo_phase += (4.0 + ((m_control_a >> 4) & 3) * 5.0) /
                                sample_rate;
        m_effect_2_lfo_phase -= std::floor(m_effect_2_lfo_phase);
        const double base_frequency = m_effect_2.frequency;
        m_effect_2.frequency = base_frequency *
            (1.0 + 0.18 * std::sin(m_effect_2_lfo_phase * 6.283185307));
        m_effect_2.level = m_effect_2_envelope;
        effects += square(m_effect_2) * 0.20;
        m_effect_2.frequency = base_frequency;
        m_effect_2_envelope = std::max(0.0, m_effect_2_envelope -
                                             1.0 / (sample_rate * 0.24));

        if (m_noise_envelope > 0.0) {
            const double noise_rate = 600.0 +
                ((m_control_a & 0x40) ? 2800.0 : 0.0) +
                ((m_control_a & 0x80) ? 1500.0 : 0.0);
            m_noise_phase += noise_rate / sample_rate;
            if (m_noise_phase >= 1.0) {
                m_noise_phase -= 1.0;
                const uint32_t feedback =
                    ((m_noise_lfsr >> 16) ^ (m_noise_lfsr >> 17)) & 1;
                m_noise_lfsr = ((m_noise_lfsr << 1) | (feedback ^ 1)) &
                               0x3ffff;
            }
            effects += (m_noise_lfsr & 1 ? 1.0 : -1.0) *
                       m_noise_envelope * 0.18;
            m_noise_envelope = std::max(0.0, m_noise_envelope -
                                               1.0 / (sample_rate * 0.32));
        }

        const int sample = static_cast<int>(
            (music * music_gain + effects * effects_gain) * 10000.0);
        output[index] = static_cast<int16_t>(
            std::clamp(sample, -32768, 32767));
    }
}

std::unique_ptr<galaxian_sound_synth> make_phoenix_sound_synth() {
    return std::make_unique<phoenix_sound_synth>();
}
