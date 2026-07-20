#include "arcade_audio_output.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>

int main() {
    using arcade_audio_output::condition_sample;

    // The normal operating region is untouched.
    assert(condition_sample(12000, 100) == 12000);
    assert(condition_sample(-12000, 100) == -12000);
    assert(condition_sample(10000, 200) == 20000);
    assert(condition_sample(1000, 100, 400) == 4000);
    assert(condition_sample(20000, 0, 400) == 0);

    // Above the knee, output remains monotonic and below hard full scale.
    bool limited = false;
    const int16_t first = condition_sample(30000, 100, 100, &limited);
    assert(limited && first > 24576 && first < 32767);
    const int16_t second = condition_sample(60000, 100);
    assert(second > first && second < 32767);
    assert(condition_sample(-60000, 100) == -second);
    assert(condition_sample(std::numeric_limits<int64_t>::max(), 200, 6400) ==
           32767);

    // Reproduce the old Victory Lap failure: 4x line gain at 160% master
    // must retain a shaped peak rather than flat-top at +/-32768.
    std::array<int16_t, 6> samples{{0, 1000, -1000, 6000, -6000, 12000}};
    const arcade_audio_output::metrics result =
        arcade_audio_output::condition_in_place(
            samples.data(), samples.size(), 160, 400);
    assert(result.limited_samples == 3);
    assert(result.peak < 32767);
    assert(samples[1] == 6400 && samples[2] == -6400);

    std::array<int32_t, 3> wide{{100, 30000, -60000}};
    std::array<int16_t, 3> output{};
    const auto converted = arcade_audio_output::condition_from_int32(
        wide.data(), output.data(), output.size(), 100);
    assert(output[0] == 100 && converted.limited_samples == 2);

    // Boards with a tenfold difference in native line level converge on the
    // same programme target. Each instance represents an independent board.
    std::array<int32_t, 960> quiet{};
    std::array<int32_t, 960> loud{};
    for (std::size_t index = 0; index < quiet.size(); ++index) {
        quiet[index] = (index & 1) ? 1000 : -1000;
        loud[index] = (index & 1) ? 10000 : -10000;
    }
    std::array<int16_t, 960> quiet_output{};
    std::array<int16_t, 960> loud_output{};
    arcade_audio_output::loudness_normalizer quiet_board;
    arcade_audio_output::loudness_normalizer loud_board;
    const auto quiet_metrics = quiet_board.process_from_int32(
        quiet.data(), quiet_output.data(), quiet.size(), 2, 48000, 100);
    const auto loud_metrics = loud_board.process_from_int32(
        loud.data(), loud_output.data(), loud.size(), 2, 48000, 100);
    assert(std::abs(quiet_metrics.peak - loud_metrics.peak) <= 1);
    assert(std::abs(quiet_metrics.peak - 6000) <= 1);
    assert(std::abs(quiet_metrics.normalization_gain - 6.0) < 0.001);
    assert(std::abs(loud_metrics.normalization_gain - 0.6) < 0.001);

    // Master and category controls remain outside the measurement. A 50%
    // category mix therefore stays half as loud after normalization.
    std::array<int32_t, 960> half_level{};
    for (std::size_t index = 0; index < half_level.size(); ++index)
        half_level[index] = (index & 1) ? 2000 : -2000;
    std::array<int16_t, 960> half_output{};
    arcade_audio_output::loudness_normalizer half_board;
    const auto half_metrics = half_board.process_from_int32(
        half_level.data(), half_output.data(), half_level.size(), 2, 48000,
        100, 50);
    assert(std::abs(half_metrics.peak - 3000) <= 1);
    assert(std::abs(half_metrics.normalization_gain - 1.5) < 0.001);

    arcade_audio_output::loudness_normalizer muted_board;
    std::array<int16_t, 32> silence{};
    const auto silence_metrics = muted_board.process_in_place(
        silence.data(), silence.size(), 1, 48000, 100, 0);
    assert(silence_metrics.peak == 0);
    assert(muted_board.gain() == 1.0);
    return 0;
}
