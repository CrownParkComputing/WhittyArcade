#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace system246::rrv_music {

constexpr std::uint32_t sector_size = 0x800;
constexpr std::uint32_t rrv_file_extent = 21;
// RRV feeds a paired left/right SPU2 stream at the hardware's 48 kHz base
// rate. Play! renders at 44.1 kHz, so the runtime player resamples this source
// rate in player::mix().
constexpr std::uint32_t sample_rate = 48000;
constexpr std::uint32_t source_channel_count = 2;
constexpr std::uint32_t channel_interleave = 0x100;
// The disc streams are mastered much hotter than the native SPU2 effects.
// This is the hardware-style nominal contribution at a 100% music setting.
constexpr int nominal_mix_percent = 25;

std::int32_t mix_disc_sample(std::int32_t native_sample,
                             std::int16_t disc_sample,
                             int music_volume_percent);

struct track_descriptor {
    std::uint32_t index{};
    std::uint32_t relative_sector{};
    std::uint32_t sector_count{};
    const char* name{};

    constexpr std::uint32_t absolute_sector() const {
        return rrv_file_extent + relative_sector;
    }
};

const std::vector<track_descriptor>& tracks();
const track_descriptor* find_track(std::uint32_t index);
// RRV stores one-based BGM numbers in its runtime music state.  Zero and -1
// mean that no disc programme should be playing.
const track_descriptor* find_track_for_game_bgm(
    std::int32_t game_bgm_number);
const track_descriptor* find_track_at_absolute_sector(
    std::uint32_t absolute_sector);
const track_descriptor* find_track_overlapping_absolute_sector_range(
    std::uint32_t first_sector, std::uint32_t sector_count);

struct decoded_audio {
    std::uint32_t rate{sample_rate};
    std::vector<std::int16_t> stereo_samples;
    std::array<double, source_channel_count> source_rms{};
    std::array<std::int32_t, source_channel_count> source_peak{};
    std::size_t decoded_blocks{};
    std::size_t invalid_blocks{};
    bool looping{};
    std::size_t loop_start_frame{};
    std::size_t loop_end_frame{};

    std::size_t frame_count() const { return stereo_samples.size() / 2; }
};

// Reads either the logical payload of rrv1-a.chd or its cached raw ISO.
class media_reader {
public:
    media_reader();
    ~media_reader();

    media_reader(const media_reader&) = delete;
    media_reader& operator=(const media_reader&) = delete;

    bool open(const std::string& path, std::string& error);
    bool read(std::uint64_t offset, std::size_t size,
              std::vector<std::uint8_t>& output, std::string& error);
    std::uint64_t logical_size() const;

private:
    struct implementation;
    std::unique_ptr<implementation> m_impl;
};

bool decode_adpcm(const std::uint8_t* source, std::size_t size,
                  decoded_audio& output, std::string& error);

// max_seconds == 0 decodes the complete table entry.
bool decode_track(media_reader& media, const track_descriptor& track,
                  decoded_audio& output, std::string& error,
                  double max_seconds = 0.0);

bool write_wav(const std::string& path, const decoded_audio& audio,
               std::string& error);

// Runtime fallback used by WhittyArcade. Disc requests select a known stream;
// decoding happens off the VM/audio threads and Write() only mixes ready PCM.
class player {
public:
    player();
    ~player();

    player(const player&) = delete;
    player& operator=(const player&) = delete;

    bool open(const std::string& media_path, std::string& error);
    void observe_game_bgm(std::int32_t game_bgm_number);
    void observe_disc_read(std::uint32_t first_sector,
                           std::uint32_t sector_count);
    void mix(std::int32_t* interleaved_stereo, std::size_t sample_count,
             std::uint32_t output_rate, int music_volume_percent = 100);
    void stop();

private:
    struct implementation;
    std::unique_ptr<implementation> m_impl;
};

} // namespace system246::rrv_music
