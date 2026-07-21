// galaxian_audio.cpp - shared OpenAL streaming worker for the
// Galaxian-family games. The synth is parameterised; the audio device
// lifecycle, source / buffer queue, and per-buffer fill loop are common to
// Phoenix, Galaxian, Moon Cresta and UniWar S.

#include "galaxian_audio.h"
#include "arcade_audio_output.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

galaxian_audio_system::galaxian_audio_system(
    std::unique_ptr<galaxian_sound_synth> synth)
    : m_synth(std::move(synth)) {}

galaxian_audio_system::~galaxian_audio_system() {
    if (m_initialized.load()) shutdown();
}

bool galaxian_audio_system::initialize() {
    if (m_initialized.load()) return true;
    {
        std::lock_guard<std::mutex> lock(m_control_mutex);
        m_pending_controls.clear();
    }
    m_output_normalizer.reset();
    m_device = alcOpenDevice(nullptr);
    if (!m_device) return false;
    m_context = alcCreateContext(static_cast<ALCdevice*>(m_device), nullptr);
    if (!m_context ||
        !alcMakeContextCurrent(static_cast<ALCcontext*>(m_context))) {
        shutdown();
        return false;
    }
    alGenSources(1, &m_source);
    alGenBuffers(buffer_count, m_buffers.data());
    if (alGetError() != AL_NO_ERROR) {
        shutdown();
        return false;
    }
    alcMakeContextCurrent(nullptr);
    if (m_synth) m_synth->reset();
    m_initialized = true;
    std::printf("Galaxian audio: 48 kHz mono via OpenAL streaming worker\n");
    return true;
}

void galaxian_audio_system::shutdown() {
    stop();
    {
        std::lock_guard<std::mutex> lock(m_control_mutex);
        m_pending_controls.clear();
    }
    if (!m_context && !m_device) return;
    if (m_context) alcMakeContextCurrent(static_cast<ALCcontext*>(m_context));
    if (m_source) alDeleteSources(1, &m_source);
    alDeleteBuffers(buffer_count, m_buffers.data());
    alcMakeContextCurrent(nullptr);
    if (m_context) alcDestroyContext(static_cast<ALCcontext*>(m_context));
    if (m_device) alcCloseDevice(static_cast<ALCdevice*>(m_device));
    m_source = 0;
    m_buffers = {};
    m_context = nullptr;
    m_device = nullptr;
    m_initialized = false;
}

void galaxian_audio_system::write_control(unsigned port, uint8_t data) {
    const unsigned idx = port & 0x07;
    if (idx < m_controls.size())
        m_controls[idx].store(data, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_control_mutex);
    m_pending_controls.push_back({port, data});
}

void galaxian_audio_system::set_mix_levels(int master, int music,
                                           int effects) {
    m_master_volume.store(std::clamp(master, 0, 200));
    m_music_volume.store(std::clamp(music, 0, 100));
    m_effects_volume.store(std::clamp(effects, 0, 100));
}

uint8_t galaxian_audio_system::control(unsigned port) const {
    if (port >= m_controls.size()) return 0;
    return m_controls[port].load(std::memory_order_acquire);
}

void galaxian_audio_system::fill_buffer(uint32_t buffer) {
    if (!m_synth) return;
    apply_pending_controls();
    const int music = m_music_volume.load(std::memory_order_relaxed);
    const int effects = m_effects_volume.load(std::memory_order_relaxed);
    m_synth->generate(m_samples.data(), buffer_frames,
                      music, effects);
    const arcade_audio_output::metrics output =
        m_output_normalizer.process_in_place(
            m_samples.data(), m_samples.size(), 1,
            galaxian_sound_synth::sample_rate, m_master_volume.load(),
            (music + effects) / 2);
    m_peak_sample = output.peak;
    alBufferData(buffer, AL_FORMAT_MONO16, m_samples.data(),
                 static_cast<ALsizei>(m_samples.size() * sizeof(int16_t)),
                 galaxian_sound_synth::sample_rate);
}

void galaxian_audio_system::apply_pending_controls() {
    std::vector<control_write> writes;
    {
        std::lock_guard<std::mutex> lock(m_control_mutex);
        writes.swap(m_pending_controls);
    }
    for (const control_write& write : writes)
        m_synth->write_control(write.port, write.data);
}

void galaxian_audio_system::audio_loop() {
    if (!alcMakeContextCurrent(static_cast<ALCcontext*>(m_context))) {
        m_running = false;
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

void galaxian_audio_system::start() {
    if (!m_initialized.load() || m_running.exchange(true)) return;
    m_thread = std::thread(&galaxian_audio_system::audio_loop, this);
}

void galaxian_audio_system::stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}
