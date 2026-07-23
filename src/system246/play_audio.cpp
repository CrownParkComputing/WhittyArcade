#include "play_audio.h"

#include "rrv_music.h"

#include "arcade_audio_output.h"

#include "sound/SH_OpenAL/SH_OpenAL.h"
#include "sound/SoundHandler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

system246_audio_control::system246_audio_control()
    : m_music(std::make_unique<system246::rrv_music::player>()) {}

system246_audio_control::~system246_audio_control() = default;

void system246_audio_control::set_mix_levels(
        int master_percent, int music_percent, int effects_percent) {
    const int master = std::clamp(master_percent, 0, 200);
    const int music = std::clamp(music_percent, 0, 100);
    const int effects = std::clamp(effects_percent, 0, 100);
    m_master_volume.store(master,
                          std::memory_order_release);
    m_music_volume.store(music,
                         std::memory_order_release);
    m_effects_volume.store(effects,
                           std::memory_order_release);
    if (const char* trace = std::getenv("WHITTYARCADE_TRACE_AUDIO");
        trace && *trace && std::strcmp(trace, "0") != 0) {
        std::printf("System 246 mix levels master=%d music=%d effects=%d\n",
                    master, music, effects);
        std::fflush(stdout);
    }
}

int system246_audio_control::master_volume() const {
    return m_master_volume.load(std::memory_order_acquire);
}

int system246_audio_control::music_volume() const {
    return m_music_volume.load(std::memory_order_acquire);
}

int system246_audio_control::effects_volume() const {
    return m_effects_volume.load(std::memory_order_acquire);
}

void system246_audio_control::set_paused(bool paused) {
    m_paused.store(paused, std::memory_order_release);
}

bool system246_audio_control::paused() const {
    return m_paused.load(std::memory_order_acquire);
}

bool system246_audio_control::configure_music(
        const std::string& media_path, std::string& error) {
    return m_music && m_music->open(media_path, error);
}

void system246_audio_control::observe_game_bgm(
        std::int32_t game_bgm_number) {
    if (m_music) m_music->observe_game_bgm(game_bgm_number);
}

void system246_audio_control::observe_disc_read(
        std::uint32_t first_sector, std::uint32_t sector_count) {
    if (m_music) m_music->observe_disc_read(first_sector, sector_count);
}

void system246_audio_control::mix_music(
        std::int32_t* samples, std::size_t sample_count,
        std::uint32_t sample_rate) {
    if (m_music)
        m_music->mix(
            samples, sample_count, sample_rate,
            system246_effective_bus_percent(
                master_volume(), music_volume()));
}

namespace {

class system246_sound_handler final : public CSoundHandler {
public:
    explicit system246_sound_handler(
            std::shared_ptr<system246_audio_control> control)
        : m_control(std::move(control)),
          m_output(std::make_unique<CSH_OpenAL>()) {
        if (const char* path = std::getenv("WHITTYARCADE_AUDIO_DUMP");
            path && *path)
            m_dump = std::fopen(path, "wb");
        if (const char* path = std::getenv("WHITTYARCADE_SPU_DUMP");
            path && *path)
            m_spu_dump = std::fopen(path, "wb");
        m_trace_started = std::chrono::steady_clock::now();
    }

    ~system246_sound_handler() override {
        if (m_dump) std::fclose(m_dump);
        if (m_spu_dump) std::fclose(m_spu_dump);
    }

    void Reset() override {
        m_normalizer.reset();
        m_mix_buffer.clear();
        m_output->Reset();
    }

    void Write(int16* samples, unsigned int sample_count,
               unsigned int sample_rate) override {
        if (!samples || sample_count == 0) return;
        static const bool trace = [] {
            const char* value = std::getenv("WHITTYARCADE_TRACE_AUDIO");
            return value && *value && std::strcmp(value, "0") != 0;
        }();
        if (trace) {
            for (unsigned int index = 0; index < sample_count; ++index) {
                const int value = samples[index];
                m_trace_spu_peak = std::max(m_trace_spu_peak,
                                            std::abs(value));
                m_trace_spu_energy +=
                    static_cast<long double>(value) * value;
            }
        }
        if (m_spu_dump)
            std::fwrite(samples, sizeof(int16), sample_count, m_spu_dump);
        arcade_audio_output::metrics output_metrics;
        if (!m_control || m_control->paused()) {
            std::fill(samples, samples + sample_count, int16{0});
        } else {
            const int master = m_control->master_volume();
            const int music = m_control->music_volume();
            const int effects = m_control->effects_volume();
            const int effective_music =
                system246_effective_bus_percent(master, music);
            const int effective_effects =
                system246_effective_bus_percent(master, effects);
            m_mix_buffer.resize(sample_count);
            for (unsigned int index = 0; index < sample_count; ++index) {
                m_mix_buffer[index] =
                    system246_effect_bus_sample(
                        samples[index], effective_effects);
                if (trace) {
                    m_trace_effect_peak = std::max(
                        m_trace_effect_peak,
                        static_cast<int>(std::abs(m_mix_buffer[index])));
                }
            }
            if (trace) m_pre_music_buffer = m_mix_buffer;
            m_control->mix_music(
                m_mix_buffer.data(), sample_count, sample_rate);
            if (trace) {
                for (unsigned int index = 0; index < sample_count; ++index) {
                    const std::int64_t contribution =
                        static_cast<std::int64_t>(m_mix_buffer[index]) -
                        m_pre_music_buffer[index];
                    m_trace_music_peak = std::max(
                        m_trace_music_peak,
                        static_cast<int>(std::min<std::int64_t>(
                            std::abs(contribution), 0x7fffffff)));
                }
            }

            // The ordinary master range has already been folded into both
            // source buses. Only the optional >100 cabinet boost remains for
            // the final common normalizer/soft-knee stage.
            output_metrics = m_normalizer.process_from_int32(
                m_mix_buffer.data(), samples, sample_count, 2, sample_rate,
                system246_output_master_percent(master),
                system246_mix_reference_percent(
                    effective_music, effective_effects));
        }
        m_trace_normalization_gain = output_metrics.normalization_gain;
        if (m_dump)
            std::fwrite(samples, sizeof(int16), sample_count, m_dump);

        if (trace) {
            int peak = 0;
            long double energy = 0;
            uint64_t clipped = 0;
            for (unsigned int index = 0; index < sample_count; ++index) {
                const int value = samples[index];
                peak = std::max(peak, std::abs(value));
                energy += static_cast<long double>(value) * value;
                if (value == -32768 || value == 32767) ++clipped;
            }
            m_trace_samples += sample_count;
            m_trace_clipped += clipped;
            m_trace_peak = std::max(m_trace_peak, peak);
            m_trace_energy += energy;
            if ((++m_trace_packets % 50) == 0) {
                const double seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - m_trace_started).count();
                const double stereo_frames = m_trace_samples / 2.0;
                const double rms = m_trace_samples != 0 ?
                    std::sqrt(static_cast<double>(m_trace_energy /
                                                  m_trace_samples)) : 0.0;
                const double spu_rms = m_trace_samples != 0 ?
                    std::sqrt(static_cast<double>(m_trace_spu_energy /
                                                  m_trace_samples)) : 0.0;
                std::printf(
                    "System 246 audio packets=%llu rate=%u delivered=%.1f "
                    "frames/s free=%u spu_peak=%d spu_rms=%.1f "
                    "effects_peak=%d music_peak=%d "
                    "mix_peak=%d mix_rms=%.1f clipped=%llu "
                    "normalizer=%.3fx programme_rms=%.1f "
                    "levels=%d/%d/%d\n",
                    static_cast<unsigned long long>(m_trace_packets),
                    sample_rate, seconds > 0 ? stereo_frames / seconds : 0,
                    m_output->GetFreeBufferCount(), m_trace_spu_peak,
                    spu_rms, m_trace_effect_peak, m_trace_music_peak,
                    m_trace_peak, rms,
                    static_cast<unsigned long long>(m_trace_clipped),
                    m_trace_normalization_gain,
                    m_normalizer.programme_rms(),
                    m_control ? m_control->master_volume() : 0,
                    m_control ? m_control->music_volume() : 0,
                    m_control ? m_control->effects_volume() : 0);
                std::fflush(stdout);
                m_trace_samples = 0;
                m_trace_clipped = 0;
                m_trace_peak = 0;
                m_trace_energy = 0;
                m_trace_spu_peak = 0;
                m_trace_spu_energy = 0;
                m_trace_effect_peak = 0;
                m_trace_music_peak = 0;
                m_trace_started = std::chrono::steady_clock::now();
            }
        }
        m_output->Write(samples, sample_count, sample_rate);
    }

    bool HasFreeBuffers() override { return m_output->HasFreeBuffers(); }
    void RecycleBuffers() override { m_output->RecycleBuffers(); }

private:
    std::shared_ptr<system246_audio_control> m_control;
    std::unique_ptr<CSH_OpenAL> m_output;
    arcade_audio_output::loudness_normalizer m_normalizer;
    std::vector<std::int32_t> m_mix_buffer;
    std::vector<std::int32_t> m_pre_music_buffer;
    std::FILE* m_dump{};
    std::FILE* m_spu_dump{};
    std::chrono::steady_clock::time_point m_trace_started{};
    uint64_t m_trace_packets{};
    uint64_t m_trace_samples{};
    uint64_t m_trace_clipped{};
    int m_trace_peak{};
    long double m_trace_energy{};
    int m_trace_spu_peak{};
    int m_trace_effect_peak{};
    int m_trace_music_peak{};
    long double m_trace_spu_energy{};
    double m_trace_normalization_gain{1.0};
};

} // namespace

CSoundHandler* make_system246_sound_handler(
        std::shared_ptr<system246_audio_control> control) {
    return new system246_sound_handler(std::move(control));
}
