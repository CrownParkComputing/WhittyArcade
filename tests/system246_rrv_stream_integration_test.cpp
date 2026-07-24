// Live, real-VM regression test for the RRV streamed-engine-audio bug: SFX
// (car revs) play correctly for a while then degrade into noise once the
// first disc-streamed chunk is exhausted (docs/system246_256_board_plan.md).
// Boots the actual Play! core against the real dongle + disc image, holds
// full throttle so the streamed voice keeps working, and inspects the raw
// SPU output (WHITTYARCADE_SPU_DUMP) for the "goes bad after a while"
// signature: a burst of full-scale clipping late in the run that isn't
// present early on, or the stream going permanently silent after having
// been active.
//
// Gated behind a real ROM path (see RRV_TEST_ROM_ZIP in CMakeLists.txt) —
// not registered as a ctest case unless that cache variable points at a real
// rrvac.zip. Never opens a window: this only constructs system246_play_core
// directly (no arcade_frontend/arcade_renderer), and audio_control is paused
// before init so nothing reaches the real output device; the dump captures
// the pre-mix SPU signal regardless of pause state.

#include "system246_rom.h"
#include "system246/play_audio.h"
#include "system246/play_core.h"
#include "system246/play_input.h"
#include "system246/play_media_cache.h"
#include "system246/play_video.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

// Nominal PS2 SPU2 output rate. Only used to size analysis windows in
// samples; the health checks below are relative (early vs. late in the
// run), so exact accuracy doesn't matter.
constexpr std::uint32_t assumed_sample_rate = 48000;
constexpr std::uint32_t window_seconds = 2;
constexpr std::size_t window_samples =
    static_cast<std::size_t>(assumed_sample_rate) * 2 /*stereo*/ *
    window_seconds;

struct window_stats {
    double rms{};
    int peak{};
    double clipped_fraction{};
};

// Minimal uncompressed 24-bit BMP writer: lets a human (or the assistant's
// own image reading) look at what screen the game is actually on, without
// needing any image codec dependency. Diagnostic only.
void save_bmp(const std::string& path, const std::vector<std::uint8_t>& rgba,
              int width, int height) {
    if (width <= 0 || height <= 0) return;
    const int row_bytes = width * 3;
    const int padded_row_bytes = (row_bytes + 3) & ~3;
    const std::uint32_t pixel_data_size =
        static_cast<std::uint32_t>(padded_row_bytes) * height;
    const std::uint32_t file_size = 54 + pixel_data_size;
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return;
    const unsigned char header[54] = {
        'B', 'M',
        static_cast<unsigned char>(file_size), static_cast<unsigned char>(file_size >> 8),
        static_cast<unsigned char>(file_size >> 16), static_cast<unsigned char>(file_size >> 24),
        0, 0, 0, 0,
        54, 0, 0, 0,
        40, 0, 0, 0,
        static_cast<unsigned char>(width), static_cast<unsigned char>(width >> 8),
        static_cast<unsigned char>(width >> 16), static_cast<unsigned char>(width >> 24),
        static_cast<unsigned char>(height), static_cast<unsigned char>(height >> 8),
        static_cast<unsigned char>(height >> 16), static_cast<unsigned char>(height >> 24),
        1, 0, 24, 0,
        0, 0, 0, 0,
        static_cast<unsigned char>(pixel_data_size), static_cast<unsigned char>(pixel_data_size >> 8),
        static_cast<unsigned char>(pixel_data_size >> 16), static_cast<unsigned char>(pixel_data_size >> 24),
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    std::fwrite(header, 1, sizeof(header), file);
    std::vector<unsigned char> row(padded_row_bytes, 0);
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t src = (static_cast<std::size_t>(y) * width + x) * 4;
            row[x * 3 + 0] = rgba[src + 2];
            row[x * 3 + 1] = rgba[src + 1];
            row[x * 3 + 2] = rgba[src + 0];
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
}

std::vector<window_stats> analyze_dump(const std::string& path) {
    std::vector<window_stats> windows;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return windows;
    std::vector<std::int16_t> buffer(window_samples);
    for (;;) {
        const std::size_t read = std::fread(
            buffer.data(), sizeof(std::int16_t), buffer.size(), file);
        if (read == 0) break;
        window_stats stats;
        long double energy = 0;
        std::uint64_t clipped = 0;
        for (std::size_t i = 0; i < read; ++i) {
            const int value = buffer[i];
            stats.peak = std::max(stats.peak, std::abs(value));
            energy += static_cast<long double>(value) * value;
            if (value >= 32767 || value <= -32768) ++clipped;
        }
        stats.rms = read != 0 ?
            static_cast<double>(std::sqrt(energy / static_cast<long double>(read))) : 0.0;
        stats.clipped_fraction = read != 0 ?
            static_cast<double>(clipped) / static_cast<double>(read) : 0.0;
        windows.push_back(stats);
        if (read < buffer.size()) break;
    }
    std::fclose(file);
    return windows;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "Usage: %s <rrvac.zip> [frames] [dump_path]\n", argv[0]);
        return 2;
    }
    const int frame_count = argc >= 3 ? std::atoi(argv[2]) : 4200;
    const std::string dump_path = argc >= 4 ? argv[3] :
        (std::filesystem::temp_directory_path() /
         "system246_rrv_stream_integration_test.raw").string();

    setenv("WHITTYARCADE_TRACE_AUDIO", "1", 1);
    setenv("WHITTYARCADE_SPU_DUMP", dump_path.c_str(), 1);

    const system246_rom_load_result roms =
        system246_rom_loader::load(argv[1], "");
    if (!roms) {
        std::fprintf(stderr, "ROM load failed: %s\n", roms.error.c_str());
        return 3;
    }
    std::string optical_path;
    std::string error;
    if (!prepare_system246_optical_media(roms, optical_path, error)) {
        std::fprintf(stderr, "Optical media prep failed: %s\n",
                     error.c_str());
        return 3;
    }

    auto input_channel = std::make_shared<system246_input_channel>();
    auto audio_control = std::make_shared<system246_audio_control>();
    // Muted at the real output device; the SPU dump above captures the
    // pre-mix signal regardless, so this doesn't affect what's analyzed.
    audio_control->set_paused(true);
    auto video_bridge = std::make_shared<system246_video_bridge>();

    system246_play_core core;
    if (!core.initialize(roms, optical_path, input_channel, audio_control,
                          video_bridge, error)) {
        std::fprintf(stderr, "Core init failed: %s\n", error.c_str());
        return 3;
    }

    std::printf(
        "system246 RRV stream integration test: running %d frames "
        "(dump=%s)\n", frame_count, dump_path.c_str());
    std::fflush(stdout);

    const char* no_input = std::getenv("RRV_TEST_NO_INPUT");
    const bool skip_input = no_input && *no_input && std::strcmp(no_input, "0") != 0;
    const char* screenshot_dir = std::getenv("RRV_TEST_SCREENSHOT_DIR");
    std::vector<std::uint8_t> frame_rgba;
    for (int frame = 0; frame < frame_count; ++frame) {
        input_state state;
        if (!skip_input) {
            // The real boot sequence (self-test hardware check, parental
            // advisory, attract-mode intro) runs on its own for roughly 45
            // real seconds (~2700 frames) before "INSERT COINS" attract state
            // is even reachable -- confirmed via screenshots. Coin/start
            // presses before that are simply too early for the game to ever
            // see. Pulse start repeatedly afterward to click through
            // course/car select screens with their default selections.
            // This cabinet costs 2 coins (confirmed via screenshot: "CREDITS
            // 1/2" after a single pulse) -- two separate rising edges are
            // needed, not one held pulse.
            state.coin1 = (frame >= 2850 && frame < 2880) ||
                          (frame >= 2910 && frame < 2940);
            state.start = (frame >= 3060 && frame < 3120) ||
                          (frame >= 3300 && frame < 3360) ||
                          (frame >= 3540 && frame < 3600) ||
                          (frame >= 3780 && frame < 3840) ||
                          (frame >= 4020 && frame < 4080) ||
                          (frame >= 4260 && frame < 4320);
            if (frame >= 4500) state.gas = 0xFFFF;
            if (frame >= 4500 && ((frame / 300) % 2) == 0) state.shift_up = true;
        }
        input_channel->set_state(state);
        video_bridge->wait_for_frame(std::chrono::milliseconds(250));
        if ((frame % 600) == 0) {
            std::printf("system246 RRV stream integration test: frame=%d\n",
                        frame);
            std::fflush(stdout);
        }
        if (screenshot_dir && (frame % 300) == 0) {
            int width = 0;
            int height = 0;
            if (video_bridge->capture_latest(core.gs_handler(), frame_rgba,
                                             width, height)) {
                save_bmp(std::string(screenshot_dir) + "/frame_" +
                             std::to_string(frame) + ".bmp",
                         frame_rgba, width, height);
            }
        }
    }

    core.shutdown();

    const std::vector<window_stats> windows = analyze_dump(dump_path);
    if (windows.empty()) {
        std::fprintf(stderr,
                     "No SPU samples captured; nothing to analyze.\n");
        return 4;
    }

    std::printf("system246 RRV stream integration test: %zu windows of "
                "%us each\n", windows.size(), window_seconds);
    for (std::size_t i = 0; i < windows.size(); ++i) {
        std::printf("  window %2zu (t=%3zus): peak=%6d rms=%8.1f "
                    "clipped=%.3f%%\n", i, i * window_seconds,
                    windows[i].peak, windows[i].rms,
                    windows[i].clipped_fraction * 100.0);
    }
    std::fflush(stdout);

    // Menus, select screens and the coin/attract sequence are legitimately
    // silent between one-shot blips -- that's not a stall. What the original
    // bug actually does is degrade *within* a sustained streaming segment
    // (a race), so find the longest contiguous run of "something is
    // playing" windows and judge health inside that run specifically.
    constexpr int active_peak_threshold = 30;
    std::size_t best_run_start = 0;
    std::size_t best_run_len = 0;
    std::size_t run_start = 0;
    std::size_t run_len = 0;
    for (std::size_t i = 0; i < windows.size(); ++i) {
        if (windows[i].peak > active_peak_threshold) {
            if (run_len == 0) run_start = i;
            ++run_len;
        } else {
            if (run_len > best_run_len) {
                best_run_len = run_len;
                best_run_start = run_start;
            }
            run_len = 0;
        }
    }
    if (run_len > best_run_len) {
        best_run_len = run_len;
        best_run_start = run_start;
    }

    if (best_run_len == 0) {
        std::fprintf(stderr,
                     "FAIL: streamed SFX never produced audible output at "
                     "all (engine never revved).\n");
        return 1;
    }

    const double sustained_seconds =
        static_cast<double>(best_run_len) * window_seconds;
    std::printf(
        "system246 RRV stream integration test: longest sustained audio "
        "run = %.0fs (windows %zu-%zu)\n", sustained_seconds,
        best_run_start, best_run_start + best_run_len - 1);

    if (sustained_seconds < 15.0) {
        std::fprintf(stderr,
                     "FAIL: never reached a sustained streaming segment "
                     "long enough to judge (longest run only %.0fs) -- "
                     "input script likely didn't reach a race.\n",
                     sustained_seconds);
        return 1;
    }

    const std::size_t half = best_run_len / 2;
    double first_half_clip = 0, second_half_clip = 0;
    double first_half_peak = 0, second_half_peak = 0;
    for (std::size_t i = 0; i < best_run_len; ++i) {
        const window_stats& w = windows[best_run_start + i];
        if (i < half) {
            first_half_clip += w.clipped_fraction;
            first_half_peak = std::max(first_half_peak,
                                       static_cast<double>(w.peak));
        } else {
            second_half_clip += w.clipped_fraction;
            second_half_peak = std::max(second_half_peak,
                                        static_cast<double>(w.peak));
        }
    }
    first_half_clip /= std::max<std::size_t>(half, 1);
    second_half_clip /= std::max<std::size_t>(best_run_len - half, 1);

    bool degraded_to_noise = second_half_clip > 0.05 &&
        second_half_clip > first_half_clip * 5.0 + 0.01;
    bool collapsed_to_near_silence =
        first_half_peak > 256 && second_half_peak < first_half_peak * 0.05;

    if (degraded_to_noise) {
        std::fprintf(stderr,
                     "FAIL: streamed SFX degraded into heavy clipping "
                     "(garbage/noise) partway through the sustained run "
                     "(first half clip=%.3f%%, second half clip=%.3f%%).\n",
                     first_half_clip * 100.0, second_half_clip * 100.0);
        return 1;
    }
    if (collapsed_to_near_silence) {
        std::fprintf(stderr,
                     "FAIL: streamed SFX collapsed to near-silence partway "
                     "through the sustained run (first half peak=%.0f, "
                     "second half peak=%.0f).\n",
                     first_half_peak, second_half_peak);
        return 1;
    }

    std::printf("PASS: streamed SFX stayed healthy for a %.0fs sustained "
                "run.\n", sustained_seconds);
    return 0;
}
