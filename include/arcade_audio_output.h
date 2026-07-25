// arcade_audio_output.h - common loudness and headroom for all boards.
#pragma once

#include <cstddef>
#include <cstdint>

namespace arcade_audio_output {

struct metrics {
    int peak{};
    std::size_t limited_samples{};
    double input_rms{};
    double normalization_gain{1.0};
};

// Process-wide output mute. Every board's samples pass through the
// conditioning below, so this silences the whole process without any board
// needing to take part. Used by the arcade wall, where all columns keep
// running but only the focused one is heard.
void set_output_muted(bool muted);
bool output_muted();

// Applies UI master gain and a board's fixed analogue-line calibration, then
// uses a continuous soft knee instead of a hard int16 clamp. Values below the
// knee remain bit-exact; dense multi-voice peaks retain headroom without the
// flat-topped discontinuities heard as crackle.
int16_t condition_sample(int64_t sample, int master_percent,
                         int board_gain_percent = 100,
                         bool* limited = nullptr);

metrics condition_in_place(int16_t* samples, std::size_t sample_count,
                           int master_percent,
                           int board_gain_percent = 100);

metrics condition_from_int32(const int32_t* input, int16_t* output,
                             std::size_t sample_count, int master_percent,
                             int board_gain_percent = 100);

// Stateful programme-level normalizer used at the final output of every
// emulated board. It gives unrelated arcade sound hardware one common RMS
// target without flattening individual samples or including the user's master
// setting in the measurement. The reference-level argument describes gain
// already applied by the Music/Effects controls, allowing those controls to
// remain effective instead of being cancelled by automatic gain.
class loudness_normalizer {
public:
    static constexpr double target_rms = 6000.0;

    metrics process_in_place(int16_t* samples, std::size_t sample_count,
                             int channels, int sample_rate,
                             int master_percent,
                             int reference_level_percent = 100);
    metrics process_from_int32(const int32_t* input, int16_t* output,
                               std::size_t sample_count, int channels,
                               int sample_rate, int master_percent,
                               int reference_level_percent = 100);

    void reset();
    double gain() const { return m_gain; }
    double programme_rms() const;

private:
    void update(double block_mean_square, double seconds,
                int reference_level_percent);
    int16_t apply(int64_t sample, int master_percent, bool* limited) const;

    bool m_active{};
    double m_programme_power{};
    double m_gain{1.0};
};

} // namespace arcade_audio_output
