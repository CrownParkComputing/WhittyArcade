#include "arcade_audio_output.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace arcade_audio_output {
namespace {
// -2.5 dBFS leaves enough room for adjacent buffers and host resampling. The
// rational knee is continuous with a slope of one at its start, unlike a hard
// clamp, and asymptotically approaches full scale.
constexpr int64_t soft_knee = 24576;
constexpr int64_t full_scale = 32767;
constexpr int64_t headroom = full_scale - soft_knee;
constexpr double silence_gate_rms = 64.0;
constexpr double minimum_normalization_gain = 0.25;
constexpr double maximum_normalization_gain = 8.0;

int64_t scaled_sample(int64_t sample, int master_percent,
                      int board_gain_percent) {
    const int64_t master = std::clamp(master_percent, 0, 200);
    const int64_t board = std::clamp(board_gain_percent, 0, 6400);
    // Audio samples are small enough that this cannot approach int64 limits,
    // but divide in two stages to keep the contract safe for external tests.
    if (sample > std::numeric_limits<int64_t>::max() /
                     std::max<int64_t>(master, 1) ||
        sample < std::numeric_limits<int64_t>::min() /
                     std::max<int64_t>(master, 1))
        return sample < 0 ? std::numeric_limits<int64_t>::min() :
                            std::numeric_limits<int64_t>::max();
    const int64_t mastered = sample * master / 100;
    if (mastered > std::numeric_limits<int64_t>::max() /
                       std::max<int64_t>(board, 1) ||
        mastered < std::numeric_limits<int64_t>::min() /
                       std::max<int64_t>(board, 1))
        return mastered < 0 ? std::numeric_limits<int64_t>::min() :
                              std::numeric_limits<int64_t>::max();
    return mastered * board / 100;
}

int16_t limit_sample(int64_t scaled, bool* limited) {
    const bool negative = scaled < 0;
    const uint64_t magnitude = negative ?
        static_cast<uint64_t>(-(scaled + 1)) + 1 :
        static_cast<uint64_t>(scaled);
    if (magnitude <= static_cast<uint64_t>(soft_knee)) {
        if (limited) *limited = false;
        return static_cast<int16_t>(scaled);
    }

    const uint64_t excess = magnitude - soft_knee;
    const uint64_t curved =
        excess > std::numeric_limits<uint64_t>::max() - headroom ||
        excess > std::numeric_limits<uint64_t>::max() / headroom ?
            static_cast<uint64_t>(headroom) :
            excess * headroom / (excess + headroom);
    const uint64_t compressed = soft_knee + curved;
    if (limited) *limited = true;
    const int64_t result = static_cast<int64_t>(
        std::min<uint64_t>(compressed, full_scale));
    return static_cast<int16_t>(negative ? -result : result);
}
} // namespace

int16_t condition_sample(int64_t sample, int master_percent,
                         int board_gain_percent, bool* limited) {
    const int64_t scaled = scaled_sample(sample, master_percent,
                                         board_gain_percent);
    return limit_sample(scaled, limited);
}

metrics condition_in_place(int16_t* samples, std::size_t sample_count,
                           int master_percent, int board_gain_percent) {
    metrics result;
    if (!samples) return result;
    for (std::size_t index = 0; index < sample_count; ++index) {
        bool limited = false;
        samples[index] = condition_sample(samples[index], master_percent,
                                          board_gain_percent, &limited);
        result.limited_samples += limited ? 1u : 0u;
        result.peak = std::max(result.peak,
            std::abs(static_cast<int>(samples[index])));
    }
    return result;
}

metrics condition_from_int32(const int32_t* input, int16_t* output,
                             std::size_t sample_count, int master_percent,
                             int board_gain_percent) {
    metrics result;
    if (!input || !output) return result;
    for (std::size_t index = 0; index < sample_count; ++index) {
        bool limited = false;
        output[index] = condition_sample(input[index], master_percent,
                                         board_gain_percent, &limited);
        result.limited_samples += limited ? 1u : 0u;
        result.peak = std::max(result.peak,
            std::abs(static_cast<int>(output[index])));
    }
    return result;
}

void loudness_normalizer::reset() {
    m_active = false;
    m_programme_power = 0.0;
    m_gain = 1.0;
}

double loudness_normalizer::programme_rms() const {
    return m_active ? std::sqrt(m_programme_power) : 0.0;
}

void loudness_normalizer::update(double block_mean_square, double seconds,
                                 int reference_level_percent) {
    if (!(block_mean_square > 0.0) || seconds <= 0.0 ||
        reference_level_percent <= 0)
        return;

    const double reference_scale =
        100.0 / std::clamp(reference_level_percent, 1, 100);
    const double reference_power =
        block_mean_square * reference_scale * reference_scale;
    if (std::sqrt(reference_power) < silence_gate_rms) return;

    if (!m_active) {
        m_programme_power = reference_power;
        m_active = true;
    } else {
        // Loud passages establish headroom quickly. Quieter passages move the
        // programme estimate slowly, preserving the game's intended dynamics
        // and avoiding audible gain pumping between speech and music.
        const double time_constant =
            reference_power > m_programme_power ? 0.15 : 3.0;
        const double blend = 1.0 - std::exp(-seconds / time_constant);
        m_programme_power += blend * (reference_power - m_programme_power);
    }

    const double desired = std::clamp(
        target_rms / std::sqrt(std::max(m_programme_power, 1.0)),
        minimum_normalization_gain, maximum_normalization_gain);
    if (m_gain == 1.0 && m_programme_power == reference_power) {
        // Start at the common level on the first meaningful block instead of
        // fading a naturally quiet board in over several seconds.
        m_gain = desired;
        return;
    }
    const double gain_time_constant = desired < m_gain ? 0.075 : 2.0;
    const double gain_blend =
        1.0 - std::exp(-seconds / gain_time_constant);
    m_gain += gain_blend * (desired - m_gain);
}

int16_t loudness_normalizer::apply(int64_t sample, int master_percent,
                                   bool* limited) const {
    const double scaled = static_cast<double>(sample) * m_gain;
    const double bounded = std::clamp(
        scaled,
        static_cast<double>(std::numeric_limits<int64_t>::min()),
        static_cast<double>(std::numeric_limits<int64_t>::max()));
    return condition_sample(static_cast<int64_t>(std::llround(bounded)),
                            master_percent, 100, limited);
}

metrics loudness_normalizer::process_in_place(
        int16_t* samples, std::size_t sample_count, int channels,
        int sample_rate, int master_percent, int reference_level_percent) {
    metrics result;
    if (!samples || sample_count == 0 || channels <= 0 || sample_rate <= 0)
        return result;

    double sum_squares = 0.0;
    for (std::size_t index = 0; index < sample_count; ++index) {
        const double sample = samples[index];
        sum_squares += sample * sample;
    }
    result.input_rms = std::sqrt(sum_squares / sample_count);
    const double seconds = static_cast<double>(sample_count) /
        (static_cast<double>(channels) * sample_rate);
    update(sum_squares / sample_count, seconds, reference_level_percent);
    result.normalization_gain = m_gain;

    for (std::size_t index = 0; index < sample_count; ++index) {
        bool limited = false;
        samples[index] = apply(samples[index], master_percent, &limited);
        result.limited_samples += limited ? 1u : 0u;
        result.peak = std::max(
            result.peak, std::abs(static_cast<int>(samples[index])));
    }
    return result;
}

metrics loudness_normalizer::process_from_int32(
        const int32_t* input, int16_t* output, std::size_t sample_count,
        int channels, int sample_rate, int master_percent,
        int reference_level_percent) {
    metrics result;
    if (!input || !output || sample_count == 0 || channels <= 0 ||
        sample_rate <= 0)
        return result;

    double sum_squares = 0.0;
    for (std::size_t index = 0; index < sample_count; ++index) {
        const double sample = input[index];
        sum_squares += sample * sample;
    }
    result.input_rms = std::sqrt(sum_squares / sample_count);
    const double seconds = static_cast<double>(sample_count) /
        (static_cast<double>(channels) * sample_rate);
    update(sum_squares / sample_count, seconds, reference_level_percent);
    result.normalization_gain = m_gain;

    for (std::size_t index = 0; index < sample_count; ++index) {
        bool limited = false;
        output[index] = apply(input[index], master_percent, &limited);
        result.limited_samples += limited ? 1u : 0u;
        result.peak = std::max(
            result.peak, std::abs(static_cast<int>(output[index])));
    }
    return result;
}

} // namespace arcade_audio_output
