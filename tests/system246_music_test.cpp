#include "system246/rrv_music.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace music = system246::rrv_music;

namespace {

std::array<std::uint8_t, music::channel_interleave> make_page(
        std::uint8_t packed_samples) {
    std::array<std::uint8_t, music::channel_interleave> page{};
    for (std::size_t offset = 0; offset < page.size(); offset += 16) {
        page[offset] = 0x00; // predictor 0, shift 0
        page[offset + 1] = 0x00;
        std::fill(page.begin() + offset + 2,
                  page.begin() + offset + 16, packed_samples);
    }
    return page;
}

void test_known_ps2_adpcm_values_and_channel_pages() {
    static_assert(music::sample_rate == 48000);
    static_assert(music::source_channel_count == 2);
    const auto left = make_page(0x11);
    const auto right = make_page(0x22);
    std::vector<std::uint8_t> encoded;
    encoded.insert(encoded.end(), left.begin(), left.end());
    encoded.insert(encoded.end(), right.begin(), right.end());

    music::decoded_audio decoded;
    std::string error;
    assert(music::decode_adpcm(
        encoded.data(), encoded.size(), decoded, error));
    assert(error.empty());
    assert(decoded.frame_count() == 16 * 28);
    assert(decoded.invalid_blocks == 0);
    assert(decoded.decoded_blocks == 2 * 16);
    assert(decoded.source_peak[0] == 4096);
    assert(decoded.source_peak[1] == 8192);
    assert(decoded.stereo_samples[0] == 4096);
    assert(decoded.stereo_samples[1] == 8192);
}

void test_signed_nibbles() {
    const auto negative = make_page(0xFF);
    std::vector<std::uint8_t> encoded;
    for (unsigned channel = 0; channel < music::source_channel_count;
         ++channel)
        encoded.insert(encoded.end(), negative.begin(), negative.end());

    music::decoded_audio decoded;
    std::string error;
    assert(music::decode_adpcm(
        encoded.data(), encoded.size(), decoded, error));
    // Play!'s `(current + 32) / 64` rounds negative values toward zero.
    assert(decoded.stereo_samples.front() == -4095);
    assert(decoded.source_peak[0] == 4095);
}

void test_invalid_predictor_is_rejected() {
    std::vector<std::uint8_t> encoded(
        music::channel_interleave * music::source_channel_count, 0);
    for (std::size_t offset = 0; offset < encoded.size(); offset += 16)
        encoded[offset] = 0xF0;
    music::decoded_audio decoded;
    std::string error;
    assert(!music::decode_adpcm(
        encoded.data(), encoded.size(), decoded, error));
    assert(!error.empty());
}

void test_disc_mix_leaves_headroom_for_spu_effects() {
    assert(music::mix_disc_sample(1234, 32767, 0) == 1234);
    assert(music::mix_disc_sample(12000, 32767, 100) == 20191);
    assert(music::mix_disc_sample(-12000, -32768, 100) == -20192);
    assert(music::mix_disc_sample(12000, 32767, 50) == 16095);
    // The runtime mix stays wide; final board normalization owns the only
    // limiter, so a loud effect plus music cannot clip prematurely here.
    assert(music::mix_disc_sample(32000, 32767, 100) == 40191);
}

void test_disc_reads_select_any_part_of_a_track() {
    const music::track_descriptor* track = music::find_track(32);
    assert(track);
    const std::uint32_t begin = track->absolute_sector();
    assert(music::find_track_at_absolute_sector(begin) == track);
    assert(music::find_track_at_absolute_sector(begin + 1) == track);
    assert(music::find_track_at_absolute_sector(
               begin + track->sector_count - 1) == track);
    const music::track_descriptor* first_track = music::find_track(0);
    assert(first_track);
    assert(music::find_track_overlapping_absolute_sector_range(
               first_track->absolute_sector() - 1, 2) == first_track);
    assert(music::find_track_overlapping_absolute_sector_range(
               begin + 7, 16) == track);
    assert(!music::find_track_overlapping_absolute_sector_range(begin, 0));
}

void test_complete_game_bgm_table_and_one_based_mapping() {
    assert(music::tracks().size() == 41);
    const music::track_descriptor* first =
        music::find_track_for_game_bgm(1);
    const music::track_descriptor* last =
        music::find_track_for_game_bgm(41);
    assert(first && first->index == 0);
    assert(first->relative_sector == 0x00000);
    assert(first->sector_count == 0x051C);
    assert(last && last->index == 40);
    assert(last->relative_sector == 0x17004);
    assert(last->sector_count == 0x097E);
    assert(!music::find_track_for_game_bgm(-1));
    assert(!music::find_track_for_game_bgm(0));
    assert(!music::find_track_for_game_bgm(42));
}

void run_synthetic_tests() {
    test_known_ps2_adpcm_values_and_channel_pages();
    test_signed_nibbles();
    test_invalid_predictor_is_rejected();
    test_disc_mix_leaves_headroom_for_spu_effects();
    test_disc_reads_select_any_part_of_a_track();
    test_complete_game_bgm_table_and_one_based_mapping();
}

void print_tracks() {
    std::printf("Ridge Racer V disc music streams:\n");
    for (const auto& track : music::tracks()) {
        const double seconds =
            static_cast<double>(track.sector_count) * music::sector_size /
            (music::sample_rate * 16.0 / 28.0 *
             music::source_channel_count);
        std::printf("  %2u  %-12s sector=%u count=%u  %6.1f s\n",
                    track.index, track.name, track.absolute_sector(),
                    track.sector_count, seconds);
    }
}

std::uint32_t parse_track(const char* text) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    assert(end && *end == '\0');
    assert(value <= std::numeric_limits<std::uint32_t>::max());
    return static_cast<std::uint32_t>(value);
}

music::decoded_audio decode_from_media(const std::string& path,
                                       std::uint32_t track_index,
                                       double max_seconds) {
    const music::track_descriptor* track = music::find_track(track_index);
    if (!track) {
        std::fprintf(stderr, "Unknown RRV music track %u.\n", track_index);
        std::exit(2);
    }
    music::media_reader media;
    std::string error;
    if (!media.open(path, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        std::exit(2);
    }
    music::decoded_audio decoded;
    if (!music::decode_track(
            media, *track, decoded, error, max_seconds)) {
        std::fprintf(stderr, "Track %u decode failed: %s\n",
                     track_index, error.c_str());
        std::exit(2);
    }
    std::printf(
        "track=%u name=\"%s\" frames=%zu seconds=%.3f "
        "rms=%.1f/%.1f peak=%d/%d invalid=%zu/%zu "
        "loop=%s:%zu-%zu\n",
        track->index, track->name, decoded.frame_count(),
        static_cast<double>(decoded.frame_count()) / decoded.rate,
        decoded.source_rms[0], decoded.source_rms[1],
        decoded.source_peak[0], decoded.source_peak[1],
        decoded.invalid_blocks, decoded.decoded_blocks,
        decoded.looping ? "yes" : "no", decoded.loop_start_frame,
        decoded.loop_end_frame);
    return decoded;
}

void verify_real_track(const std::string& path, std::uint32_t track_index) {
    const music::decoded_audio decoded =
        decode_from_media(path, track_index, 3.0);
    assert(decoded.frame_count() >= music::sample_rate * 2);
    assert(decoded.invalid_blocks == 0);
    assert(std::all_of(decoded.source_rms.begin(), decoded.source_rms.end(),
                       [](double rms) { return rms > 8.0; }));
    assert(std::all_of(decoded.source_peak.begin(), decoded.source_peak.end(),
                       [](std::int32_t peak) { return peak > 32; }));
    assert(decoded.stereo_samples.size() == decoded.frame_count() * 2);

    // Exercise the exact asynchronous selector and mixer used by WhittyArcade,
    // not only the decoder in isolation.
    const music::track_descriptor* track = music::find_track(track_index);
    assert(track);
    music::player runtime_player;
    std::string error;
    assert(runtime_player.open(path, error));
    runtime_player.observe_game_bgm(
        static_cast<std::int32_t>(track->index + 1));
    std::vector<std::int32_t> mixed(4096, 0);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool heard_music = false;
    while (std::chrono::steady_clock::now() < deadline) {
        std::fill(mixed.begin(), mixed.end(), 0);
        runtime_player.mix(mixed.data(), mixed.size(), music::sample_rate);
        heard_music = std::any_of(
            mixed.begin(), mixed.end(),
            [](std::int32_t sample) { return sample != 0; });
        if (heard_music) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    runtime_player.stop();
    assert(heard_music);
}

void play(const music::decoded_audio& decoded) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL audio init failed: %s\n", SDL_GetError());
        std::exit(2);
    }
    SDL_AudioSpec spec{};
    spec.freq = static_cast<int>(decoded.rate);
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
        std::fprintf(stderr, "SDL audio device failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        std::exit(2);
    }
    const std::size_t bytes =
        decoded.stereo_samples.size() * sizeof(std::int16_t);
    assert(bytes <= std::numeric_limits<std::int32_t>::max());
    if (!SDL_PutAudioStreamData(stream, decoded.stereo_samples.data(),
                                static_cast<int>(bytes))) {
        std::fprintf(stderr, "SDL queue failed: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        std::exit(2);
    }
    std::printf("Playing one decoded RRV track; Ctrl-C stops early.\n");
    SDL_ResumeAudioStreamDevice(stream);
    while (SDL_GetAudioStreamQueued(stream) > 0) SDL_Delay(20);
    SDL_DestroyAudioStream(stream);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void usage(const char* executable) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s                         # synthetic unit tests\n"
        "  %s --list\n"
        "  %s --verify CHD_OR_ISO [TRACK]\n"
        "  %s --play CHD_OR_ISO TRACK\n"
        "  %s --wav CHD_OR_ISO TRACK OUTPUT.wav\n",
        executable, executable, executable, executable, executable);
}

} // namespace

int main(int argc, char** argv) {
    run_synthetic_tests();
    if (argc == 1) return 0;

    const std::string command = argv[1];
    if (command == "--list" && argc == 2) {
        print_tracks();
        return 0;
    }
    if (command == "--verify" && (argc == 3 || argc == 4)) {
        verify_real_track(argv[2], argc == 4 ? parse_track(argv[3]) : 0);
        return 0;
    }
    if (command == "--play" && argc == 4) {
        play(decode_from_media(argv[2], parse_track(argv[3]), 0.0));
        return 0;
    }
    if (command == "--wav" && argc == 5) {
        const music::decoded_audio decoded =
            decode_from_media(argv[2], parse_track(argv[3]), 0.0);
        std::string error;
        if (!music::write_wav(argv[4], decoded, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 2;
        }
        std::printf("Wrote %s\n", argv[4]);
        return 0;
    }
    usage(argv[0]);
    return 2;
}
