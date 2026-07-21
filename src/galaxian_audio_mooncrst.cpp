// Moon Cresta discrete sound board.
//
// The game has no separate audio ROM or sound CPU. Its Z80 program ROM drives
// eight sound lines, four background-DAC lines and one pitch latch. This model
// follows the MAME-derived discrete circuit used by the local ZArcade project:
// pitch divider/counter, resistor mixer, LFSR noise, 555-style background and
// FIRE oscillators, and the HIT/FIRE analogue envelopes.

#include "galaxian_audio.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace {

constexpr double kTau = 6.283185307179586;
constexpr double kTtlOut = 4.0;
constexpr double kSoundClock = 1'536'000.0;
constexpr double kRngClock = 61'440.0;
constexpr double kNoiseLatchClock = 7'920.0;
constexpr double kOpenCircuit = 1.0e12;
constexpr double kPcmGain = 24'000.0;
constexpr double kMixGain = 4.2;
constexpr double kHitTrim = 0.10;
constexpr double kSamplePeriod =
    1.0 / galaxian_sound_synth::sample_rate;

constexpr int kHitLine = 3;
constexpr int kFireLine = 5;
constexpr int kVolume1Line = 6;
constexpr int kVolume2Line = 7;

constexpr double kR15 = 100'000.0;
constexpr double kR16 = 220'000.0;
constexpr double kR17 = 470'000.0;
constexpr double kR18 = 1'000'000.0;
constexpr double kR19 = 330'000.0;
constexpr double kR20 = 15'000.0;
constexpr double kR21 = 100'000.0;
constexpr double kR22 = 100'000.0;
constexpr double kR23 = 470'000.0;
constexpr double kR24 = 10'000.0;
constexpr double kR25 = 100'000.0;
constexpr double kR26 = 330'000.0;
constexpr double kR27 = 10'000.0;
constexpr double kR28 = 100'000.0;
constexpr double kR29 = 220'000.0;
constexpr double kR30 = 10'000.0;
constexpr double kR31 = 47'000.0;
constexpr double kR32 = 47'000.0;
constexpr double kR33 = 10'000.0;
constexpr double kGalaxianR34 = 5'100.0;
constexpr double kMoonCrestaR34 = 15'000.0;
constexpr double kR35 = 150'000.0;
constexpr double kR36 = 22'000.0;
constexpr double kR40 = 2'200.0 * 0.6;
constexpr double kR41 = 100'000.0;
constexpr double kR43 = 2'200.0;
constexpr double kR44 = 10'000.0;
constexpr double kR45 = 22'000.0;
constexpr double kR46 = 10'000.0;
constexpr double kR47 = 2'200.0;
constexpr double kR48 = 2'200.0;
constexpr double kR49 = 10'000.0;
constexpr double kR50 = 22'000.0;
constexpr double kR51 = 33'000.0;
constexpr double kR52 = 15'000.0;
constexpr double kR91 = 10'000.0;

constexpr double kC15 = 0.000001;
constexpr double kC17 = 0.00000001;
constexpr double kC18 = 0.00000001;
constexpr double kC19 = 0.00000001;
constexpr double kC20 = 0.0000001;
constexpr double kC21 = 0.0000022;
constexpr double kC25 = 0.000001;
constexpr double kC26 = 0.00000001;
constexpr double kC27 = 0.00000001;
constexpr double kC28 = 0.000047;
constexpr double kC46 = 0.0000001;

constexpr double kBackgroundDacBias = 4.4;
constexpr double kBackground555Supply = 5.0;
constexpr double kBackground555High = 4.5;
constexpr double kBackgroundCurrentSource = 5.0;
constexpr double kBackgroundJunction = 0.7;
constexpr double kBackgroundThreshold = kBackground555Supply * 2.0 / 3.0;
constexpr double kBackgroundTrigger = kBackground555Supply / 3.0;
constexpr double kFireSupply = 5.0;
constexpr double kHitRcSeconds = (kR35 + kR36) * kC21;

constexpr std::array<double, 4> kBackgroundDacResistors{
    kR18, kR17, kR16, kR15};
constexpr std::array<double, 3> kBackgroundMixResistors{
    kR24, kR27, kR30};
constexpr std::array<double, 3> kBackgroundRa{kR22, kR25, kR28};
constexpr std::array<double, 3> kBackgroundRb{kR23, kR26, kR29};
constexpr std::array<double, 3> kBackgroundCapacitors{kC17, kC18, kC19};
constexpr double kBackgroundMixResistance =
    1.0 / (1.0 / kR24 + 1.0 / kR27 + 1.0 / kR30);

enum class discrete_mix_profile : uint8_t {
    mooncrst,
    galaxian,
};

double parallel(std::initializer_list<double> resistors) {
    double inverse = 0.0;
    for (const double resistor : resistors) inverse += 1.0 / resistor;
    return inverse == 0.0 ? kOpenCircuit : 1.0 / inverse;
}

double one_pole_coefficient(double frequency) {
    return 1.0 - std::exp(-kTau * frequency * kSamplePeriod);
}

double rc_charge_coefficient(double rc, double dt = kSamplePeriod) {
    return 1.0 - std::exp(-dt / rc);
}

double overshoot_charge_time(double source, double before, double after,
                             double threshold, double rc) {
    const double denominator = source - before;
    if (denominator <= 0.0 || after <= threshold) return 0.0;
    const double fraction = (after - threshold) / denominator;
    if (fraction <= 0.0 || fraction >= 1.0) return 0.0;
    return std::max(0.0, rc * std::log(1.0 / (1.0 - fraction)));
}

double overshoot_discharge_time(double before, double after, double trigger,
                                double rc) {
    if (before <= 0.0 || after >= trigger) return 0.0;
    const double fraction = (trigger - after) / before;
    if (fraction <= 0.0 || fraction >= 1.0) return 0.0;
    return std::max(0.0, rc * std::log(1.0 / (1.0 - fraction)));
}

double resistor_dac(double r0_inverse, double r1_inverse) {
    const double r0 = 1.0 / r0_inverse;
    const double r1 = 1.0 / r1_inverse;
    return 2.0 * r0 / (r0 + r1) - 1.0;
}

std::array<std::array<double, 16>, 4> make_tone_wave() {
    std::array<std::array<double, 16>, 4> waves{};
    for (int value = 0; value < 16; ++value) {
        double r0a = 1.0 / kOpenCircuit;
        double r1a = 1.0 / kOpenCircuit;
        double r0b = 1.0 / kOpenCircuit;
        double r1b = 1.0 / kOpenCircuit;

        if ((value & 0x01) != 0) {
            r1a += 1.0 / kR51;
            r1b += 1.0 / kR51;
        } else {
            r0a += 1.0 / kR51;
            r0b += 1.0 / kR51;
        }
        if ((value & 0x04) != 0) {
            r1a += 1.0 / kR50;
            r1b += 1.0 / kR50;
        } else {
            r0a += 1.0 / kR50;
            r0b += 1.0 / kR50;
        }
        waves[0][value] = resistor_dac(r0a, r1a);

        if ((value & 0x04) != 0) r1a += 1.0 / kR49;
        else r0a += 1.0 / kR49;
        waves[1][value] = resistor_dac(r0a, r1a);

        if ((value & 0x08) != 0) r1b += 1.0 / kR52;
        else r0b += 1.0 / kR52;
        waves[2][value] = resistor_dac(r0b, r1b);

        if ((value & 0x04) != 0) r0b += 1.0 / kR49;
        else r1b += 1.0 / kR49;
        waves[3][value] = resistor_dac(r0b, r1b);
    }
    return waves;
}

} // namespace

class galaxian_discrete_sound_synth final : public galaxian_sound_synth {
public:
    explicit galaxian_discrete_sound_synth(discrete_mix_profile profile)
        : m_mix_profile(profile) {}

    void reset() override {
        m_sound_lines.fill(0);
        m_background_bits.fill(0);
        m_pitch_latch = 0xff;
        m_background_timer_cap.fill(0.0);
        m_background_timer_high.fill(true);
        m_background_cap = 0.0;
        m_background_mixer_cap = 0.0;
        m_pitch_phase = 0.0;
        m_pitch_counter = 0;
        m_pitch_level = 0.0;
        m_rng_phase = 0.0;
        m_noise_latch_phase = 0.0;
        m_noise_register = 0x00001;
        m_rng_output = -1.0;
        m_latched_noise = -1.0;
        m_hit_envelope = 0.0;
        m_hit_highpass_store = 0.0;
        m_hit_lowpass_store = 0.0;
        m_fire_c28_voltage = 0.0;
        m_fire_555_cap = 0.0;
        m_fire_555_high = true;
        m_fire_output_cap = 0.0;
        m_fire_mixer_cap = 0.0;
        m_output_dc = 0.0;
    }

    void write_control(unsigned port, uint8_t data) override {
        const uint8_t value = data & 0x01;
        if (port == mooncrst_audio_port::pitch) {
            m_pitch_latch = data;
        } else if ((port & ~0x07u) == mooncrst_audio_port::sound_base) {
            m_sound_lines[port & 0x07u] = value;
            if ((port & 0x07u) == kHitLine && value != 0)
                m_hit_envelope = std::max(m_hit_envelope, 1.0);
        } else if ((port & ~0x03u) == mooncrst_audio_port::lfo_base) {
            m_background_bits[port & 0x03u] = value;
        }
    }

    int active_tune() const override {
        return m_pitch_latch == 0xff ? 0 : m_pitch_counter + 1;
    }

    void generate(int16_t* output, int frames, int music_volume,
                  int effects_volume) override {
        if (!output || frames <= 0) return;
        const double music_gain =
            std::clamp(music_volume, 0, 100) / 100.0;
        const double effects_gain =
            std::clamp(effects_volume, 0, 100) / 100.0;

        for (int frame = 0; frame < frames; ++frame) {
            const double noise = next_noise();
            const double background = next_background();
            advance_pitch();
            const double hit = next_hit(noise);
            const double fire = fire_high_pass(next_fire(noise));
            const double mixed = mix_output(background, hit, fire,
                                            music_gain, effects_gain);
            const double sample = std::clamp(high_pass_output(mixed),
                                             -0.98, 0.98);
            output[frame] = static_cast<int16_t>(sample * kPcmGain);
        }
    }

private:
    double next_background() {
        const double control_voltage = next_background_control_voltage();
        double mixed = 0.0;
        for (std::size_t voice = 0; voice < 3; ++voice)
            mixed += next_background_555(voice, control_voltage) /
                     kBackgroundMixResistors[voice];
        const double voltage = mixed * kBackgroundMixResistance;
        m_background_mixer_cap +=
            (voltage - m_background_mixer_cap) *
            m_background_mixer_coefficient;
        return m_background_mixer_cap;
    }

    double next_background_control_voltage() {
        const double dac_voltage = background_dac_voltage();
        const double current = std::max(
            0.0, (kBackgroundCurrentSource -
                  (dac_voltage + kBackgroundJunction)) / kR21);
        double dt = kSamplePeriod;
        do {
            const double next = std::min(
                m_background_cap + current * dt / kC15,
                dac_voltage + kBackgroundJunction);
            dt = 0.0;
            if (next >= kBackgroundThreshold && current > 0.0) {
                dt = kC15 * (next - kBackgroundThreshold) / current;
                m_background_cap = kBackgroundTrigger;
            } else {
                m_background_cap = next;
            }
        } while (dt > 0.0);

        const double op_amp =
            m_background_cap *
                (kR33 / parallel({kR31, kR32, kR33})) -
            5.0 * kR33 / kR31;
        return std::clamp(op_amp, 0.0, 5.0);
    }

    double background_dac_voltage() const {
        double total_inverse = 1.0 / kR20 + 1.0 / kR19;
        double total_current = kBackgroundDacBias / kR20;
        unsigned latch = 0;
        for (std::size_t bit = 0; bit < m_background_bits.size(); ++bit)
            latch |= static_cast<unsigned>(m_background_bits[bit]) << bit;

        for (std::size_t bit = 0; bit < kBackgroundDacResistors.size();
             ++bit) {
            const double inverse = 1.0 / kBackgroundDacResistors[bit];
            total_inverse += inverse;
            if (((latch >> bit) & 1u) != 0)
                total_current += kTtlOut / kBackgroundDacResistors[bit];
        }
        return total_current / total_inverse;
    }

    double next_background_555(std::size_t voice, double control_voltage) {
        if (m_sound_lines[voice] == 0) {
            m_background_timer_cap[voice] = 0.0;
            m_background_timer_high[voice] = true;
            return 0.0;
        }

        const double threshold = std::max(control_voltage, 0.25);
        const double trigger = threshold * 0.5;
        const double previous_cap = m_background_timer_cap[voice];
        double cap = previous_cap;
        bool high = m_background_timer_high[voice];
        double dt = kSamplePeriod;
        bool output_high = high;

        if (cap >= threshold) high = false;
        if (cap <= trigger) high = true;
        do {
            if (high) {
                const double rc =
                    (kBackgroundRa[voice] + kBackgroundRb[voice]) *
                    kBackgroundCapacitors[voice];
                const double next = cap +
                    (kBackground555Supply - cap) *
                    rc_charge_coefficient(rc, dt);
                dt = 0.0;
                cap = next;
                if (cap >= threshold) {
                    cap = threshold;
                    high = false;
                    output_high = false;
                    dt = overshoot_charge_time(
                        kBackground555Supply, previous_cap, next,
                        threshold, rc);
                }
            } else {
                const double rc = kBackgroundRb[voice] *
                                  kBackgroundCapacitors[voice];
                const double next =
                    cap - cap * rc_charge_coefficient(rc, dt);
                dt = 0.0;
                cap = next;
                if (cap <= trigger) {
                    cap = trigger;
                    high = true;
                    output_high = true;
                    dt = overshoot_discharge_time(previous_cap, next,
                                                   trigger, rc);
                }
            }
        } while (dt > 0.0);

        m_background_timer_cap[voice] = cap;
        m_background_timer_high[voice] = high;
        return output_high ? kBackground555High : 0.0;
    }

    void advance_pitch() {
        const double target = m_pitch_latch != 0xff ? 1.0 : 0.0;
        m_pitch_level += (target - m_pitch_level) * m_gate_coefficient;
        if (m_pitch_level < 0.0001) return;

        const int divider = std::max(256 - static_cast<int>(m_pitch_latch), 1);
        m_pitch_phase +=
            (kSoundClock / divider) / galaxian_sound_synth::sample_rate;
        const int steps = static_cast<int>(std::floor(m_pitch_phase));
        if (steps > 0) {
            m_pitch_phase -= steps;
            m_pitch_counter = (m_pitch_counter + steps) & 0x0f;
        }
    }

    double pitch_node(int bit) const {
        return ((m_pitch_counter >> bit) & 1) != 0 ?
            kTtlOut * m_pitch_level : 0.0;
    }

    double pitch_wave() const {
        const unsigned volume =
            static_cast<unsigned>(m_sound_lines[kVolume1Line]) |
            (static_cast<unsigned>(m_sound_lines[kVolume2Line]) << 1);
        return ((m_tone_wave[volume][m_pitch_counter] + 1.0) * 0.5 *
                kTtlOut) * m_pitch_level;
    }

    double next_hit(double noise) {
        if (m_sound_lines[kHitLine] != 0)
            m_hit_envelope = 1.0;
        else
            m_hit_envelope *= m_hit_release;
        if (m_hit_envelope < 0.0001) return 0.0;

        m_hit_highpass_store +=
            (noise - m_hit_highpass_store) * m_hit_highpass_coefficient;
        const double highpassed = noise - m_hit_highpass_store;
        m_hit_lowpass_store +=
            (highpassed - m_hit_lowpass_store) * m_hit_lowpass_coefficient;
        return std::clamp(m_hit_lowpass_store * m_hit_envelope *
                              kHitTrim * kTtlOut,
                          -kTtlOut, kTtlOut);
    }

    double next_fire(double noise) {
        const bool enabled = m_sound_lines[kFireLine] != 0;
        const double inverted = enabled ? 0.0 : kTtlOut;
        m_fire_c28_voltage +=
            (inverted - m_fire_c28_voltage) * m_fire_c28_coefficient;

        const double noise_logic = noise > 0.0 ? 1.0 : 0.0;
        const double control_voltage = parallel({kR46, kR48}) *
            (noise_logic * kTtlOut / kR46 + m_fire_c28_voltage / kR48);
        const bool timer_high = next_fire_555(control_voltage);

        const double input = enabled ? kTtlOut : 0.0;
        const double target = std::max(input - 0.7, 0.0);
        double difference = target - m_fire_output_cap;
        if (timer_high) {
            if (difference < 0.0)
                difference *= m_fire_output_coefficient;
            m_fire_output_cap += difference;
            return m_fire_output_cap;
        }
        if (difference > 0.0) m_fire_output_cap = target;
        return 0.0;
    }

    bool next_fire_555(double control_voltage) {
        const double threshold = std::max(control_voltage, 0.25);
        const double trigger = threshold * 0.5;
        const double previous_cap = m_fire_555_cap;
        double cap = previous_cap;
        bool high = m_fire_555_high;
        double dt = kSamplePeriod;
        bool output_high = high;

        if (cap >= threshold) high = false;
        if (cap <= trigger) high = true;
        do {
            if (high) {
                constexpr double rc = (kR44 + kR45) * kC27;
                const double next = cap +
                    (kFireSupply - cap) * rc_charge_coefficient(rc, dt);
                dt = 0.0;
                cap = next;
                if (cap >= threshold) {
                    cap = threshold;
                    high = false;
                    output_high = false;
                    dt = overshoot_charge_time(kFireSupply, previous_cap,
                                               next, threshold, rc);
                }
            } else {
                constexpr double rc = kR45 * kC27;
                const double next =
                    cap - cap * rc_charge_coefficient(rc, dt);
                dt = 0.0;
                cap = next;
                if (cap <= trigger) {
                    cap = trigger;
                    high = true;
                    output_high = true;
                    dt = overshoot_discharge_time(previous_cap, next,
                                                   trigger, rc);
                }
            }
        } while (dt > 0.0);

        m_fire_555_cap = cap;
        m_fire_555_high = high;
        return output_high;
    }

    double fire_high_pass(double input) {
        m_fire_mixer_cap +=
            (input - m_fire_mixer_cap) * m_fire_mixer_coefficient;
        return input - m_fire_mixer_cap;
    }

    double next_noise() {
        m_rng_phase +=
            kRngClock / galaxian_sound_synth::sample_rate;
        int steps = static_cast<int>(std::floor(m_rng_phase));
        m_rng_phase -= steps;
        while (steps-- > 0) {
            const int bit4 = (m_noise_register >> 4) & 0x01;
            const int bit16 = (m_noise_register >> 16) & 0x01;
            const int feedback = bit4 ^ (bit16 ^ 0x01);
            m_noise_register =
                ((m_noise_register << 1) | feedback) & 0x1ffff;
            m_rng_output = feedback != 0 ? 1.0 : -1.0;
        }

        m_noise_latch_phase +=
            kNoiseLatchClock / galaxian_sound_synth::sample_rate;
        int latch_steps =
            static_cast<int>(std::floor(m_noise_latch_phase));
        m_noise_latch_phase -= latch_steps;
        while (latch_steps-- > 0) m_latched_noise = m_rng_output;
        return m_latched_noise;
    }

    double mix_output(double background, double hit, double fire,
                      double music_gain, double effects_gain) const {
        if (m_mix_profile == discrete_mix_profile::galaxian) {
            const double pitch = pitch_wave();
            const double volume1 =
                m_sound_lines[kVolume1Line] != 0 ? kR49 : 0.0;
            const double volume2 =
                m_sound_lines[kVolume2Line] != 0 ? kR52 : 0.0;
            const double pre_conductance =
                1.0 / kR51 + 1.0 / (kR50 + volume1) + 1.0 / kR50 +
                1.0 / (kR50 + volume2) + 1.0 / kGalaxianR34;
            const double pre_voltage =
                (pitch / kR51 + pitch / (kR50 + volume1) +
                 pitch / kR50 + pitch / (kR50 + volume2) +
                 background / kGalaxianR34) /
                pre_conductance;

            const double final_conductance =
                1.0 / kGalaxianR34 + 1.0 / kR40 + 1.0 / kR43 +
                1.0 / kR91;
            const double final_voltage =
                (pre_voltage / kGalaxianR34 * music_gain +
                 hit / kR40 * effects_gain +
                 fire / kR43 * effects_gain) /
                final_conductance;
            return (final_voltage / kTtlOut) * kMixGain;
        }

        double conductance = 0.0;
        double music_current = 0.0;
        double effects_current = 0.0;
        const auto add = [&](double voltage, double resistance,
                             bool music) {
            const double inverse = 1.0 / resistance;
            conductance += inverse;
            (music ? music_current : effects_current) += voltage * inverse;
        };

        add(pitch_node(0), kR51, true);
        add(pitch_node(2),
            m_sound_lines[kVolume1Line] != 0 ? kR49 : kOpenCircuit,
            true);
        add(pitch_node(2), kR50, true);
        add(pitch_node(3),
            m_sound_lines[kVolume2Line] != 0 ? kR52 : kOpenCircuit,
            true);
        add(background, kMoonCrestaR34, true);
        add(hit, kR40, false);
        add(fire, kR43, false);

        if (conductance == 0.0) return 0.0;
        const double mixed_voltage =
            (music_current * music_gain + effects_current * effects_gain) /
            conductance;
        return (mixed_voltage / kTtlOut) * kMixGain;
    }

    double high_pass_output(double sample) {
        m_output_dc +=
            (sample - m_output_dc) * m_output_highpass_coefficient;
        return sample - m_output_dc;
    }

    discrete_mix_profile m_mix_profile;
    std::array<uint8_t, 8> m_sound_lines{};
    std::array<uint8_t, 4> m_background_bits{};
    uint8_t m_pitch_latch{0xff};
    std::array<double, 3> m_background_timer_cap{};
    std::array<bool, 3> m_background_timer_high{{true, true, true}};
    double m_background_cap{};
    double m_background_mixer_cap{};
    double m_pitch_phase{};
    int m_pitch_counter{};
    double m_pitch_level{};
    double m_rng_phase{};
    double m_noise_latch_phase{};
    int m_noise_register{0x00001};
    double m_rng_output{-1.0};
    double m_latched_noise{-1.0};
    double m_hit_envelope{};
    double m_hit_highpass_store{};
    double m_hit_lowpass_store{};
    double m_fire_c28_voltage{};
    double m_fire_555_cap{};
    bool m_fire_555_high{true};
    double m_fire_output_cap{};
    double m_fire_mixer_cap{};
    double m_output_dc{};
    const std::array<std::array<double, 16>, 4> m_tone_wave{
        make_tone_wave()};

    const double m_gate_coefficient{one_pole_coefficient(90.0)};
    const double m_hit_highpass_coefficient{one_pole_coefficient(290.0)};
    const double m_hit_lowpass_coefficient{one_pole_coefficient(1'550.0)};
    const double m_background_mixer_coefficient{
        rc_charge_coefficient(kBackgroundMixResistance * kC20)};
    const double m_fire_c28_coefficient{
        rc_charge_coefficient(kR47 * kC28)};
    const double m_fire_output_coefficient{
        rc_charge_coefficient(kR41 * kC25)};
    const double m_fire_mixer_coefficient{
        rc_charge_coefficient(parallel({kR43, 10'000.0}) * kC26)};
    const double m_output_highpass_coefficient{
        rc_charge_coefficient(100'000.0 * kC46)};
    const double m_hit_release{
        std::exp(-1.0 /
                 (galaxian_sound_synth::sample_rate * kHitRcSeconds))};
};

std::unique_ptr<galaxian_sound_synth> make_mooncrst_sound_synth() {
    return std::make_unique<galaxian_discrete_sound_synth>(
        discrete_mix_profile::mooncrst);
}

std::unique_ptr<galaxian_sound_synth> make_uniwars_sound_synth() {
    return std::make_unique<galaxian_discrete_sound_synth>(
        discrete_mix_profile::galaxian);
}
