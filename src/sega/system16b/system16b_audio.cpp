// Shinobi sound synthesis implementation: ymfm ym2151 + segapcm +
// OpenAL streaming worker.
//
// The synth combines a ymfm YM2151 (FM) and the standalone segapcm
// chip, mixes their output, and supplies int16 stereo frames on demand.

#include "sega/system16b/system16b_audio.h"
#include "arcade_audio_output.h"

#include "segapcm.h"
#include "ymfm.h"
#include "ymfm_opm.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include <AL/al.h>
#include <AL/alc.h>

namespace {

// ymfm bus shim. ymfm delegates the YM2151's two hardware timers to the
// host: the chip arms them via ymfm_set_timer() with a duration in input
// clocks and expects engine_timer_expired() back once that many clocks
// elapse. Shinobi's sound program paces its entire sequencer off the
// timer A status flag, so these have to run for music to play at all.
class opm_bus final : public ymfm::ymfm_interface {
public:
    void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override {
        if (tnum < 2) m_timer_clocks[tnum] = duration_in_clocks;
    }

    void advance_clocks(uint32_t clocks) {
        for (uint32_t t = 0; t < 2; ++t) {
            if (m_timer_clocks[t] < 0) continue;
            m_timer_clocks[t] -= static_cast<int64_t>(clocks);
            if (m_timer_clocks[t] <= 0) {
                m_timer_clocks[t] = -1; // engine re-arms on expiry
                if (m_engine) m_engine->engine_timer_expired(t);
            }
        }
    }

    void ymfm_external_write(ymfm::access_class /*type*/,
                             uint32_t /*address*/, uint8_t data) override {
        (void)data;
    }

private:
    int64_t m_timer_clocks[2]{-1, -1};
};

}  // namespace

struct system16b_sound_synth::implementation {
    opm_bus         opm_bus_;
    ymfm::ym2151    opm_;
    segapcm         pcm_;
    double          resample_phase{0.0};

    implementation() : opm_(opm_bus_) {
        pcm_.set_clock(4'000'000);
        pcm_.reset();
    }
};

system16b_sound_synth::system16b_sound_synth()
    : m_impl(std::make_unique<implementation>()) {}

system16b_sound_synth::~system16b_sound_synth() = default;

void system16b_sound_synth::reset() {
    m_impl->opm_.reset();
    m_impl->pcm_.reset();
}

void system16b_sound_synth::write_control(unsigned port, uint8_t data) {
    if (port == 0) {
        m_impl->opm_.write_address(data);
    } else if (port == 1) {
        m_impl->opm_.write_data(data);
    } else {
        // SegaPCM forward: ports >= 2 are interpreted as SegaPCM
        // register writes (mirror of MAME's sh_sega_pcm handler).
        m_impl->pcm_.write(port, data);
    }
}

uint8_t system16b_sound_synth::read_status() {
    return m_impl->opm_.read_status();
}

void system16b_sound_synth::advance_timer_clocks(uint32_t clocks) {
    m_impl->opm_bus_.advance_clocks(clocks);
}

void system16b_sound_synth::generate(int16_t* output, int frames) {
    if (!output || frames <= 0) return;

    constexpr int kOut = system16b_sound_synth::sample_rate;
    // The YM2151 renders natively at clock/64 = 55.9 kHz. Generate at the
    // native rate and resample to 48 kHz linearly; previously the native
    // stream was stamped 48 kHz and played ~14% fast.
    constexpr double native_rate =
        static_cast<double>(system16b_sound_synth::chip_clock_hz) / 64.0;
    const int native_needed = static_cast<int>(
        m_impl->resample_phase + frames * (native_rate / kOut)) + 2;
    std::vector<ymfm::ymfm_output<2>> opm_buf(native_needed);
    m_impl->opm_.generate(opm_buf.data(),
                          static_cast<uint32_t>(native_needed));

    std::vector<int16_t> pcm_l(frames, 0);
    std::vector<int16_t> pcm_r(frames, 0);
    m_impl->pcm_.sound_stream_update(pcm_l.data(), pcm_r.data(), frames, kOut);

    double phase = m_impl->resample_phase;
    const double step = native_rate / kOut;
    for (int i = 0; i < frames; ++i) {
        const int index = static_cast<int>(phase);
        const double frac = phase - index;
        auto lerp = [frac](int32_t a, int32_t b) {
            return a + static_cast<int32_t>((b - a) * frac);
        };
        int32_t l = lerp(opm_buf[index].data[0],
                         opm_buf[index + 1].data[0]) + (pcm_l[i] >> 1);
        int32_t r = lerp(opm_buf[index].data[1],
                         opm_buf[index + 1].data[1]) + (pcm_r[i] >> 1);
        phase += step;
        if (l >  32767) l =  32767;
        if (l < -32768) l = -32768;
        if (r >  32767) r =  32767;
        if (r < -32768) r = -32768;
        output[i * 2 + 0] = static_cast<int16_t>(l);
        output[i * 2 + 1] = static_cast<int16_t>(r);
    }
    // Carry the unconsumed native-sample fraction into the next call.
    m_impl->resample_phase = phase - (native_needed - 2);
}

void system16b_sound_synth::set_pcm_rom(const uint8_t* rom, std::size_t bytes) {
    m_impl->pcm_.set_rom(rom, bytes);
}

std::unique_ptr<system16b_sound_synth> make_system16b_sound_synth() {
    return std::make_unique<system16b_sound_synth>();
}

// =====================================================================
// OpenAL streaming worker -- mirrors the galaxian_audio_system shape.
// =====================================================================

system16b_audio_system::system16b_audio_system(
    std::unique_ptr<system16b_sound_synth> synth)
    : m_synth(std::move(synth)) {}

system16b_audio_system::~system16b_audio_system() { shutdown(); }

bool system16b_audio_system::initialize() {
    if (m_initialized.load()) return true;
    m_output_normalizer.reset();
    if (m_synth) {
        std::lock_guard<std::mutex> lock(m_synth_mutex);
        m_synth->reset();
    }
    m_device = alcOpenDevice(nullptr);
    if (!m_device) return false;
    m_context = alcCreateContext(
        static_cast<ALCdevice*>(m_device), nullptr);
    if (!m_context) {
        alcCloseDevice(static_cast<ALCdevice*>(m_device));
        m_device = nullptr;
        return false;
    }
    if (!alcMakeContextCurrent(
            static_cast<ALCcontext*>(m_context))) {
        alcDestroyContext(static_cast<ALCcontext*>(m_context));
        alcCloseDevice(static_cast<ALCdevice*>(m_device));
        m_context = nullptr;
        m_device = nullptr;
        return false;
    }
    alGenBuffers(static_cast<ALsizei>(buffer_count), m_buffers.data());
    alGenSources(1, &m_source);
    if (alGetError() != AL_NO_ERROR) {
        std::fprintf(stderr, "Shinobi OpenAL: buffer allocation failed\n");
        if (m_source) alDeleteSources(1, &m_source);
        alDeleteBuffers(static_cast<ALsizei>(buffer_count), m_buffers.data());
        alcMakeContextCurrent(nullptr);
        alcDestroyContext(static_cast<ALCcontext*>(m_context));
        alcCloseDevice(static_cast<ALCdevice*>(m_device));
        m_source = 0;
        m_buffers.fill(0);
        m_context = nullptr;
        m_device = nullptr;
        return false;
    }
    alcMakeContextCurrent(nullptr);
    m_initialized.store(true);
    std::printf("Shinobi audio: YM2151 at 48 kHz via OpenAL worker\n");
    return true;
}

void system16b_audio_system::shutdown() {
    stop();
    if (m_initialized.load()) {
        alcMakeContextCurrent(static_cast<ALCcontext*>(m_context));
        alDeleteSources(1, &m_source);
        alDeleteBuffers(static_cast<ALsizei>(buffer_count), m_buffers.data());
        m_source = 0; m_buffers.fill(0);
        alcMakeContextCurrent(nullptr);
        if (m_context) alcDestroyContext(static_cast<ALCcontext*>(m_context));
        if (m_device)  alcCloseDevice(static_cast<ALCdevice*>(m_device));
        m_context = nullptr; m_device = nullptr;
        m_initialized.store(false);
    }
}

void system16b_audio_system::start() {
    if (!m_initialized.load() || m_running.load()) return;
    m_running.store(true);
    m_thread = std::thread([this] { audio_loop(); });
}

void system16b_audio_system::stop() {
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
}

void system16b_audio_system::write_control(unsigned port, uint8_t data) {
    if (!m_synth || port > 0xffu) return;
    std::lock_guard<std::mutex> lock(m_synth_mutex);
    m_synth->write_control(port, data);
}

uint8_t system16b_audio_system::read_status() {
    if (!m_synth) return 0;
    std::lock_guard<std::mutex> lock(m_synth_mutex);
    return m_synth->read_status();
}

void system16b_audio_system::advance_timer_clocks(uint32_t clocks) {
    if (!m_synth || clocks == 0) return;
    std::lock_guard<std::mutex> lock(m_synth_mutex);
    m_synth->advance_timer_clocks(clocks);
}

void system16b_audio_system::set_mix_levels(int master, int music, int effects) {
    m_master_volume.store(std::clamp(master, 0, 200));
    m_music_volume.store(std::clamp(music, 0, 100));
    m_effects_volume.store(std::clamp(effects, 0, 100));
}

void system16b_audio_system::fill_buffer(uint32_t buffer) {
    {
        std::lock_guard<std::mutex> lock(m_synth_mutex);
        m_synth->generate(m_samples.data(), buffer_frames);
    }
    // Both chips carry sequences and one-shots, so neither can be assigned to
    // just one UI bus. This matches Model 2's mixed-chip slider policy.
    const int category_mix =
        (m_music_volume.load(std::memory_order_relaxed) +
         m_effects_volume.load(std::memory_order_relaxed)) / 2;
    for (int16_t& sample : m_samples)
        sample = static_cast<int16_t>(
            static_cast<int32_t>(sample) * category_mix / 100);
    const arcade_audio_output::metrics output =
        m_output_normalizer.process_in_place(
            m_samples.data(), m_samples.size(), 2,
            system16b_sound_synth::sample_rate,
            m_master_volume.load(std::memory_order_relaxed), category_mix);
    m_peak_sample.store(output.peak, std::memory_order_relaxed);

    alBufferData(buffer, AL_FORMAT_STEREO16, m_samples.data(),
                 static_cast<ALsizei>(m_samples.size()) *
                     static_cast<ALsizei>(sizeof(int16_t)),
                 system16b_sound_synth::sample_rate);
}

void system16b_audio_system::audio_loop() {
    if (!alcMakeContextCurrent(static_cast<ALCcontext*>(m_context))) {
        std::fprintf(stderr, "Shinobi OpenAL: worker context failed\n");
        m_running.store(false);
        return;
    }
    for (uint32_t buffer : m_buffers) fill_buffer(buffer);
    alSourceQueueBuffers(m_source, buffer_count, m_buffers.data());
    alSourcePlay(m_source);
    bool source_paused = false;
    while (m_running.load(std::memory_order_acquire)) {
        if (m_paused.load(std::memory_order_acquire)) {
            if (!source_paused) alSourcePause(m_source);
            source_paused = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (source_paused) alSourcePlay(m_source);
        source_paused = false;
        ALint processed = 0;
        alGetSourcei(m_source, AL_BUFFERS_PROCESSED, &processed);
        while (processed-- > 0) {
            ALuint buffer = 0;
            alSourceUnqueueBuffers(m_source, 1, &buffer);
            fill_buffer(buffer);
            alSourceQueueBuffers(m_source, 1, &buffer);
        }
        ALint state = AL_STOPPED;
        alGetSourcei(m_source, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING) alSourcePlay(m_source);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    alSourceStop(m_source);
    ALint queued = 0;
    alGetSourcei(m_source, AL_BUFFERS_QUEUED, &queued);
    while (queued-- > 0) {
        ALuint buffer = 0;
        alSourceUnqueueBuffers(m_source, 1, &buffer);
    }
    alcMakeContextCurrent(nullptr);
}
