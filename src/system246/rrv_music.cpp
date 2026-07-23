#include "rrv_music.h"

#include <libchdr/chd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace system246::rrv_music {

std::int32_t mix_disc_sample(std::int32_t native_sample,
                             std::int16_t disc_sample,
                             int music_volume_percent) {
    const int volume = std::clamp(music_volume_percent, 0, 100);
    const std::int32_t contribution =
        static_cast<std::int32_t>(disc_sample) * nominal_mix_percent * volume /
        10000;
    return native_sample + contribution;
}
namespace {

constexpr std::size_t adpcm_block_size = 16;
constexpr std::size_t samples_per_block = 28;
constexpr std::size_t blocks_per_interleave =
    channel_interleave / adpcm_block_size;
constexpr std::size_t samples_per_interleave =
    blocks_per_interleave * samples_per_block;

// These entries are the contiguous three-voice stream table embedded in
// RRV's START executable. relative_sector is relative to the RRV1_A extent.
const std::vector<track_descriptor> track_table = {
    {0,  0x00000, 0x051C, "Game BGM 01"},
    {1,  0x0051C, 0x097E, "Game BGM 02"},
    {2,  0x00E9A, 0x0A78, "Game BGM 03"},
    {3,  0x01912, 0x0E28, "Game BGM 04"},
    {4,  0x0273A, 0x1FEC, "Game BGM 05"},
    {5,  0x04726, 0x02E4, "Game BGM 06"},
    {6,  0x04A0A, 0x02E4, "Game BGM 07"},
    {7,  0x04CEE, 0x1D54, "Game BGM 08"},
    {8,  0x06A42, 0x1EC8, "Game BGM 09"},
    {9,  0x0890A, 0x1796, "Game BGM 10"},
    {10, 0x0A0A0, 0x1D38, "Game BGM 11"},
    {11, 0x0BDD8, 0x1F06, "Game BGM 12"},
    {12, 0x0DCDE, 0x1D80, "Game BGM 13"},
    {13, 0x0FA5E, 0x02E4, "Game BGM 14"},
    {14, 0x0FD42, 0x02E4, "Game BGM 15"},
    {15, 0x10026, 0x02E4, "Game BGM 16"},
    {16, 0x1030A, 0x02E4, "Game BGM 17"},
    {17, 0x105EE, 0x02E4, "Game BGM 18"},
    {18, 0x108D2, 0x1C0E, "Game BGM 19"},
    {19, 0x124E0, 0x11CC, "Game BGM 20"},
    {20, 0x136AC, 0x042E, "Game BGM 21"},
    {21, 0x13ADA, 0x00D6, "Game BGM 22"},
    {22, 0x13BB0, 0x0648, "Game BGM 23"},
    {23, 0x141F8, 0x013A, "Game BGM 24"},
    {24, 0x14332, 0x013E, "Game BGM 25"},
    {25, 0x14470, 0x013E, "Game BGM 26"},
    {26, 0x145AE, 0x013A, "Game BGM 27"},
    {27, 0x146E8, 0x0142, "Game BGM 28"},
    {28, 0x1482A, 0x0138, "Game BGM 29"},
    {29, 0x14962, 0x0140, "Game BGM 30"},
    {30, 0x14AA2, 0x013C, "Game BGM 31"},
    {31, 0x14BDE, 0x013C, "Game BGM 32"},
    {32, 0x14D1A, 0x013E, "Game BGM 33"},
    {33, 0x14E58, 0x013E, "Game BGM 34"},
    {34, 0x14F96, 0x013C, "Game BGM 35"},
    {35, 0x150D2, 0x013C, "Game BGM 36"},
    {36, 0x1520E, 0x013E, "Game BGM 37"},
    {37, 0x1534C, 0x00DE, "Game BGM 38"},
    {38, 0x1542A, 0x0A32, "Game BGM 39"},
    {39, 0x15E5C, 0x11A8, "Game BGM 40"},
    {40, 0x17004, 0x097E, "Game BGM 41"},
};

struct chd_closer {
    void operator()(chd_file* image) const {
        if (image) chd_close(image);
    }
};

std::int16_t clamp_sample(std::int64_t value) {
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(
        value, std::numeric_limits<std::int16_t>::min(),
        std::numeric_limits<std::int16_t>::max()));
}

void append_block(const std::uint8_t* block, std::int32_t& history1,
                  std::int32_t& history2,
                  std::vector<std::int16_t>& destination,
                  bool& valid) {
    static constexpr std::int32_t coefficients[5][2] = {
        {0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60},
    };
    const std::uint8_t predictor = block[0] >> 4;
    const std::uint8_t shift = block[0] & 0x0F;
    if (predictor >= 5 || shift > 12) {
        valid = false;
        destination.insert(destination.end(), samples_per_block, 0);
        history1 = 0;
        history2 = 0;
        return;
    }

    valid = true;
    for (std::size_t byte_index = 2; byte_index < adpcm_block_size;
         ++byte_index) {
        const std::uint8_t packed = block[byte_index];
        for (unsigned nibble_index = 0; nibble_index < 2;
             ++nibble_index) {
            std::int16_t unpacked = static_cast<std::int16_t>(
                ((packed >> (nibble_index * 4)) & 0x0F) << 12);
            unpacked >>= shift;

            // Match Play!'s SPU decoder arithmetic, including its 64x
            // internal predictor history and rounding point.
            std::int64_t current = static_cast<std::int32_t>(unpacked) * 64;
            current += (static_cast<std::int64_t>(history1) *
                        coefficients[predictor][0]) / 64;
            current += (static_cast<std::int64_t>(history2) *
                        coefficients[predictor][1]) / 64;
            history2 = history1;
            history1 = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                current, std::numeric_limits<std::int32_t>::min(),
                std::numeric_limits<std::int32_t>::max()));
            destination.push_back(clamp_sample((current + 32) / 64));
        }
    }
}

void write_u16(std::ofstream& output, std::uint16_t value) {
    const std::array<char, 2> bytes = {
        static_cast<char>(value & 0xFF),
        static_cast<char>((value >> 8) & 0xFF),
    };
    output.write(bytes.data(), bytes.size());
}

void write_u32(std::ofstream& output, std::uint32_t value) {
    const std::array<char, 4> bytes = {
        static_cast<char>(value & 0xFF),
        static_cast<char>((value >> 8) & 0xFF),
        static_cast<char>((value >> 16) & 0xFF),
        static_cast<char>((value >> 24) & 0xFF),
    };
    output.write(bytes.data(), bytes.size());
}

} // namespace

const std::vector<track_descriptor>& tracks() { return track_table; }

const track_descriptor* find_track(std::uint32_t index) {
    const auto found = std::find_if(
        track_table.begin(), track_table.end(),
        [index](const auto& track) { return track.index == index; });
    return found == track_table.end() ? nullptr : &*found;
}

const track_descriptor* find_track_for_game_bgm(
        std::int32_t game_bgm_number) {
    if (game_bgm_number <= 0) return nullptr;
    return find_track(static_cast<std::uint32_t>(game_bgm_number - 1));
}

const track_descriptor* find_track_at_absolute_sector(
        std::uint32_t absolute_sector) {
    const auto found = std::find_if(
        track_table.begin(), track_table.end(),
        [absolute_sector](const auto& track) {
            const std::uint64_t begin = track.absolute_sector();
            const std::uint64_t end = begin + track.sector_count;
            return absolute_sector >= begin && absolute_sector < end;
        });
    return found == track_table.end() ? nullptr : &*found;
}

const track_descriptor* find_track_overlapping_absolute_sector_range(
        std::uint32_t first_sector, std::uint32_t sector_count) {
    if (sector_count == 0) return nullptr;
    const std::uint64_t read_begin = first_sector;
    const std::uint64_t read_end = read_begin + sector_count;
    const auto found = std::find_if(
        track_table.begin(), track_table.end(),
        [read_begin, read_end](const auto& track) {
            const std::uint64_t track_begin = track.absolute_sector();
            const std::uint64_t track_end =
                track_begin + track.sector_count;
            return read_begin < track_end && read_end > track_begin;
        });
    return found == track_table.end() ? nullptr : &*found;
}

struct media_reader::implementation {
    std::ifstream raw;
    std::unique_ptr<chd_file, chd_closer> chd;
    std::vector<std::uint8_t> hunk;
    std::uint64_t size{};
    std::uint32_t hunk_bytes{};
    std::uint32_t cached_hunk{std::numeric_limits<std::uint32_t>::max()};
};

media_reader::media_reader() : m_impl(std::make_unique<implementation>()) {}
media_reader::~media_reader() = default;

bool media_reader::open(const std::string& path, std::string& error) {
    error.clear();
    m_impl = std::make_unique<implementation>();

    std::ifstream probe(path, std::ios::binary);
    std::array<char, 8> magic{};
    probe.read(magic.data(), magic.size());
    const bool is_chd = probe.gcount() ==
                            static_cast<std::streamsize>(magic.size()) &&
                        std::memcmp(magic.data(), "MComprHD", 8) == 0;
    probe.close();

    if (is_chd) {
        chd_file* raw_image = nullptr;
        const chd_error result = chd_open(
            path.c_str(), CHD_OPEN_READ, nullptr, &raw_image);
        m_impl->chd.reset(raw_image);
        if (result != CHDERR_NONE || !m_impl->chd) {
            error = std::string("Could not open RRV CHD: ") +
                    chd_error_string(result);
            return false;
        }
        const chd_header* header = chd_get_header(m_impl->chd.get());
        if (!header || header->hunkbytes == 0) {
            error = "RRV CHD has invalid logical geometry.";
            return false;
        }
        m_impl->size = header->logicalbytes;
        m_impl->hunk_bytes = header->hunkbytes;
        m_impl->hunk.resize(header->hunkbytes);
        return true;
    }

    m_impl->raw.open(path, std::ios::binary);
    if (!m_impl->raw) {
        error = "Could not open RRV ISO or CHD: " + path;
        return false;
    }
    m_impl->raw.seekg(0, std::ios::end);
    const std::streamoff size = m_impl->raw.tellg();
    if (size <= 0) {
        error = "RRV media image is empty.";
        return false;
    }
    m_impl->size = static_cast<std::uint64_t>(size);
    m_impl->raw.seekg(0, std::ios::beg);
    return true;
}

bool media_reader::read(std::uint64_t offset, std::size_t size,
                        std::vector<std::uint8_t>& output,
                        std::string& error) {
    error.clear();
    output.clear();
    if (offset > m_impl->size ||
        static_cast<std::uint64_t>(size) > m_impl->size - offset) {
        error = "RRV media read exceeds the logical image.";
        return false;
    }
    output.resize(size);
    if (size == 0) return true;

    if (m_impl->raw.is_open()) {
        m_impl->raw.clear();
        m_impl->raw.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        m_impl->raw.read(reinterpret_cast<char*>(output.data()),
                         static_cast<std::streamsize>(size));
        if (m_impl->raw.gcount() != static_cast<std::streamsize>(size)) {
            output.clear();
            error = "RRV ISO read was truncated.";
            return false;
        }
        return true;
    }

    if (!m_impl->chd || m_impl->hunk_bytes == 0) {
        error = "RRV media reader is not open.";
        output.clear();
        return false;
    }
    std::size_t destination_offset = 0;
    while (destination_offset < size) {
        const std::uint64_t logical = offset + destination_offset;
        const std::uint64_t hunk_index64 = logical / m_impl->hunk_bytes;
        if (hunk_index64 > std::numeric_limits<std::uint32_t>::max()) {
            error = "RRV CHD hunk index is out of range.";
            output.clear();
            return false;
        }
        const auto hunk_index = static_cast<std::uint32_t>(hunk_index64);
        if (hunk_index != m_impl->cached_hunk) {
            const chd_error result = chd_read(
                m_impl->chd.get(), hunk_index, m_impl->hunk.data());
            if (result != CHDERR_NONE) {
                error = std::string("RRV CHD read failed: ") +
                        chd_error_string(result);
                output.clear();
                return false;
            }
            m_impl->cached_hunk = hunk_index;
        }
        const std::size_t within_hunk = static_cast<std::size_t>(
            logical % m_impl->hunk_bytes);
        const std::size_t count = std::min(
            size - destination_offset,
            static_cast<std::size_t>(m_impl->hunk_bytes) - within_hunk);
        std::copy_n(m_impl->hunk.begin() + within_hunk, count,
                    output.begin() + destination_offset);
        destination_offset += count;
    }
    return true;
}

std::uint64_t media_reader::logical_size() const { return m_impl->size; }

bool decode_adpcm(const std::uint8_t* source, std::size_t size,
                  decoded_audio& output, std::string& error) {
    output = {};
    error.clear();
    if (!source || size < channel_interleave * source_channel_count) {
        error = "RRV ADPCM stream is too short.";
        return false;
    }
    const std::size_t page_count = size / channel_interleave;
    std::array<std::vector<std::int16_t>, source_channel_count> channels;
    std::array<std::int32_t, source_channel_count> history1{};
    std::array<std::int32_t, source_channel_count> history2{};
    std::array<std::optional<std::size_t>, source_channel_count> loop_starts;
    std::array<std::optional<std::size_t>, source_channel_count> loop_ends;
    std::array<bool, source_channel_count> loop_end_repeats{};
    for (auto& channel : channels) {
        channel.reserve(((page_count + source_channel_count - 1) /
                         source_channel_count) * samples_per_interleave);
    }

    for (std::size_t page_index = 0; page_index < page_count; ++page_index) {
        const std::size_t channel_index = page_index % source_channel_count;
        const std::uint8_t* page =
            source + (page_index * channel_interleave);
        for (std::size_t block_index = 0;
             block_index < blocks_per_interleave; ++block_index) {
            const std::uint8_t* block =
                page + (block_index * adpcm_block_size);
            const std::uint8_t flags = block[1];
            if ((flags & 0x04) && !loop_starts[channel_index])
                loop_starts[channel_index] = channels[channel_index].size();
            bool valid = false;
            append_block(block,
                         history1[channel_index], history2[channel_index],
                         channels[channel_index], valid);
            if (flags & 0x01) {
                loop_ends[channel_index] = channels[channel_index].size();
                loop_end_repeats[channel_index] = (flags == 0x03);
            }
            ++output.decoded_blocks;
            output.invalid_blocks += !valid;
        }
    }

    const std::size_t frames = std::min(
        channels[0].size(), channels[1].size());
    if (frames == 0 || output.invalid_blocks * 20 > output.decoded_blocks) {
        error = "RRV stream does not contain valid stereo PS2 ADPCM.";
        output = {};
        return false;
    }

    std::array<long double, source_channel_count> energy{};
    output.stereo_samples.resize(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t channel = 0; channel < source_channel_count;
             ++channel) {
            const std::int32_t value = channels[channel][frame];
            output.source_peak[channel] = std::max(
                output.source_peak[channel], std::abs(value));
            energy[channel] += static_cast<long double>(value) * value;
        }

        // RRV creates two mode-2 stream handles and configures them as a
        // left/right pair. The separately created mode-1 handle is the output
        // handle, not a third (centre) channel. Pages alternate L, R at the
        // 0x100-byte interleave passed by the game to its stream callbacks.
        output.stereo_samples[(frame * 2) + 0] = channels[0][frame];
        output.stereo_samples[(frame * 2) + 1] = channels[1][frame];
    }
    for (std::size_t channel = 0; channel < source_channel_count; ++channel) {
        output.source_rms[channel] = std::sqrt(
            static_cast<double>(energy[channel] / frames));
    }
    const bool has_common_end = std::all_of(
        loop_ends.begin(), loop_ends.end(),
        [](const auto& marker) { return marker.has_value(); });
    if (has_common_end) {
        output.loop_end_frame = std::min({
            *loop_ends[0], *loop_ends[1], frames});
        output.looping = std::all_of(
            loop_end_repeats.begin(), loop_end_repeats.end(),
            [](bool repeats) { return repeats; });
    } else {
        output.loop_end_frame = frames;
    }
    const bool has_common_start = std::all_of(
        loop_starts.begin(), loop_starts.end(),
        [](const auto& marker) { return marker.has_value(); });
    if (has_common_start) {
        output.loop_start_frame = std::max(
            *loop_starts[0], *loop_starts[1]);
    }
    if (output.loop_start_frame >= output.loop_end_frame)
        output.looping = false;
    return true;
}

bool decode_track(media_reader& media, const track_descriptor& track,
                  decoded_audio& output, std::string& error,
                  double max_seconds) {
    std::uint32_t sectors = track.sector_count;
    if (max_seconds > 0.0) {
        const double bytes_per_second =
            static_cast<double>(sample_rate) * adpcm_block_size /
            samples_per_block * source_channel_count;
        const auto requested = static_cast<std::uint32_t>(std::ceil(
            max_seconds * bytes_per_second / sector_size));
        sectors = std::min(sectors, std::max<std::uint32_t>(requested, 1));
    }
    std::vector<std::uint8_t> encoded;
    const std::uint64_t offset =
        static_cast<std::uint64_t>(track.absolute_sector()) * sector_size;
    const std::size_t bytes =
        static_cast<std::size_t>(sectors) * sector_size;
    if (!media.read(offset, bytes, encoded, error)) return false;
    return decode_adpcm(encoded.data(), encoded.size(), output, error);
}

bool write_wav(const std::string& path, const decoded_audio& audio,
               std::string& error) {
    error.clear();
    if (audio.stereo_samples.empty()) {
        error = "Cannot write an empty RRV track.";
        return false;
    }
    const std::uint64_t byte_count64 =
        audio.stereo_samples.size() * sizeof(std::int16_t);
    if (byte_count64 > std::numeric_limits<std::uint32_t>::max() - 36) {
        error = "RRV WAV output is too large.";
        return false;
    }
    const auto byte_count = static_cast<std::uint32_t>(byte_count64);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "Could not create WAV file: " + path;
        return false;
    }
    output.write("RIFF", 4);
    write_u32(output, 36 + byte_count);
    output.write("WAVEfmt ", 8);
    write_u32(output, 16);
    write_u16(output, 1);
    write_u16(output, 2);
    write_u32(output, audio.rate);
    write_u32(output, audio.rate * 2 * sizeof(std::int16_t));
    write_u16(output, 2 * sizeof(std::int16_t));
    write_u16(output, 16);
    output.write("data", 4);
    write_u32(output, byte_count);
    output.write(reinterpret_cast<const char*>(audio.stereo_samples.data()),
                 static_cast<std::streamsize>(byte_count));
    if (!output) {
        error = "Writing RRV WAV data failed.";
        return false;
    }
    return true;
}

struct player::implementation {
    std::string media_path;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    bool quitting{};
    bool game_control_observed{};
    std::optional<std::uint32_t> requested_track;
    std::optional<std::uint32_t> loading_track;
    std::optional<std::uint32_t> current_track;
    std::uint64_t request_generation{};
    std::shared_ptr<const decoded_audio> current;
    double current_frame{};

    void clear_locked() {
        requested_track.reset();
        loading_track.reset();
        current_track.reset();
        current.reset();
        current_frame = 0.0;
        ++request_generation;
    }

    bool request_locked(std::uint32_t track_index) {
        if (requested_track == track_index ||
            loading_track == track_index ||
            current_track == track_index)
            return false;
        requested_track = track_index;
        ++request_generation;
        return true;
    }

    void run() {
        media_reader media;
        std::string error;
        if (!media.open(media_path, error)) {
            std::fprintf(stderr, "RRV music: %s\n", error.c_str());
            return;
        }
        for (;;) {
            std::uint32_t track_index = 0;
            std::uint64_t generation = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] {
                    return quitting || requested_track.has_value();
                });
                if (quitting) return;
                track_index = *requested_track;
                generation = request_generation;
                requested_track.reset();
                loading_track = track_index;
            }

            const track_descriptor* descriptor = find_track(track_index);
            auto decoded = std::make_shared<decoded_audio>();
            if (!descriptor ||
                !decode_track(media, *descriptor, *decoded, error)) {
                std::fprintf(stderr, "RRV music track %u: %s\n",
                             track_index, error.c_str());
                std::lock_guard<std::mutex> lock(mutex);
                if (loading_track == track_index) loading_track.reset();
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (quitting) return;
                if (generation != request_generation) {
                    if (loading_track == track_index) loading_track.reset();
                    continue;
                }
                current = std::move(decoded);
                current_track = track_index;
                loading_track.reset();
                current_frame = 0.0;
            }
            std::printf("RRV music: playing track %u (%s) from disc\n",
                        descriptor->index, descriptor->name);
            std::fflush(stdout);
        }
    }
};

player::player() : m_impl(std::make_unique<implementation>()) {}
player::~player() { stop(); }

bool player::open(const std::string& media_path, std::string& error) {
    stop();
    media_reader probe;
    if (!probe.open(media_path, error)) return false;
    m_impl = std::make_unique<implementation>();
    m_impl->media_path = media_path;
    m_impl->worker = std::thread([state = m_impl.get()] { state->run(); });
    return true;
}

void player::observe_game_bgm(std::int32_t game_bgm_number) {
    const track_descriptor* descriptor =
        find_track_for_game_bgm(game_bgm_number);
    bool wake_worker = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->game_control_observed = true;
        if (!descriptor || !m_impl->worker.joinable()) {
            m_impl->clear_locked();
        } else {
            wake_worker = m_impl->request_locked(descriptor->index);
        }
    }
    if (wake_worker) m_impl->wake.notify_one();
}

void player::observe_disc_read(std::uint32_t first_sector,
                               std::uint32_t sector_count) {
    if (sector_count == 0) return;
    const track_descriptor* descriptor =
        find_track_overlapping_absolute_sector_range(
            first_sector, sector_count);
    if (!descriptor || !m_impl->worker.joinable()) return;
    bool wake_worker = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        // Direct game-state observation is authoritative. Sector reads remain
        // useful to standalone tests and as a fallback for other revisions,
        // but ordinary boot/file reads must not start music in RRV.
        if (m_impl->game_control_observed) return;
        wake_worker = m_impl->request_locked(descriptor->index);
    }
    if (wake_worker) m_impl->wake.notify_one();
}

void player::mix(std::int32_t* samples, std::size_t sample_count,
                 std::uint32_t output_rate, int music_volume_percent) {
    if (!samples || sample_count < 2 || output_rate == 0) return;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!m_impl->current || m_impl->current->frame_count() == 0) return;

    const decoded_audio& music = *m_impl->current;
    const std::size_t frame_count = sample_count / 2;
    const double step = static_cast<double>(music.rate) / output_rate;
    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        if (m_impl->current_frame >= music.loop_end_frame) {
            if (music.looping) {
                const double loop_length = static_cast<double>(
                    music.loop_end_frame - music.loop_start_frame);
                m_impl->current_frame = music.loop_start_frame + std::fmod(
                    m_impl->current_frame - music.loop_end_frame,
                    loop_length);
            } else {
                m_impl->current.reset();
                m_impl->current_track.reset();
                m_impl->current_frame = 0.0;
                break;
            }
        }
        const std::size_t source_frame = static_cast<std::size_t>(
            m_impl->current_frame);
        const std::size_t next_frame =
            (source_frame + 1) % music.frame_count();
        const double fraction =
            m_impl->current_frame - static_cast<double>(source_frame);
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const std::int32_t first =
                music.stereo_samples[(source_frame * 2) + channel];
            const std::int32_t second =
                music.stereo_samples[(next_frame * 2) + channel];
            const auto interpolated = static_cast<std::int32_t>(std::lround(
                first + ((second - first) * fraction)));
            samples[(frame * 2) + channel] = mix_disc_sample(
                samples[(frame * 2) + channel],
                static_cast<std::int16_t>(interpolated),
                music_volume_percent);
        }
        m_impl->current_frame += step;
    }
}

void player::stop() {
    if (!m_impl) return;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->quitting = true;
        m_impl->requested_track.reset();
        m_impl->current.reset();
        m_impl->loading_track.reset();
        m_impl->current_track.reset();
    }
    m_impl->wake.notify_all();
    if (m_impl->worker.joinable()) m_impl->worker.join();
}

} // namespace system246::rrv_music
