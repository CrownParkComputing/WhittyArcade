#include "xbox360_audio.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

xbox360_audio_output::~xbox360_audio_output() {
    shutdown();
}

bool xbox360_audio_output::initialize() {
    if (m_running.load()) return true;
    m_device = alcOpenDevice(nullptr);
    if (!m_device) {
        std::fprintf(stderr, "Robotron audio: no OpenAL device available\n");
        return false;
    }
    m_context = alcCreateContext(static_cast<ALCdevice*>(m_device), nullptr);
    if (!m_context ||
        !alcMakeContextCurrent(static_cast<ALCcontext*>(m_context))) {
        std::fprintf(stderr, "Robotron audio: OpenAL context failed\n");
        shutdown();
        return false;
    }
    alGenSources(1, &m_source);
    alGenBuffers(buffer_count, m_buffers.data());
    const bool valid = alGetError() == AL_NO_ERROR;
    alcMakeContextCurrent(nullptr);
    if (!valid) {
        shutdown();
        return false;
    }
    m_normalizer.reset();
    m_running = true;
    m_thread = std::thread(&xbox360_audio_output::thread_main, this);
    return true;
}

void xbox360_audio_output::shutdown() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_samples.clear();
    }
    if (m_context) alcMakeContextCurrent(static_cast<ALCcontext*>(m_context));
    if (m_source) alDeleteSources(1, &m_source);
    if (m_context) alDeleteBuffers(buffer_count, m_buffers.data());
    alcMakeContextCurrent(nullptr);
    if (m_context) alcDestroyContext(static_cast<ALCcontext*>(m_context));
    if (m_device) alcCloseDevice(static_cast<ALCdevice*>(m_device));
    m_source = 0;
    m_buffers = {};
    m_context = nullptr;
    m_device = nullptr;
}

void xbox360_audio_output::enqueue(const std::int16_t* stereo,
                                   std::size_t frames) {
    if (!stereo || frames == 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    // Bound latency to roughly 250 ms. Old audio is less useful than current
    // audio when the host briefly falls behind shader compilation.
    constexpr std::size_t maximum_samples = sample_rate / 4 * 2;
    const std::size_t incoming = frames * 2;
    while (m_samples.size() + incoming > maximum_samples &&
           m_samples.size() >= 2) {
        m_samples.pop_front();
        m_samples.pop_front();
    }
    m_samples.insert(m_samples.end(), stereo, stereo + incoming);
}

void xbox360_audio_output::fill_buffer(std::uint32_t buffer) {
    m_output.assign(target_frames * 2, 0);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::size_t count = std::min(m_output.size(), m_samples.size());
        for (std::size_t index = 0; index < count; ++index) {
            m_output[index] = m_samples.front();
            m_samples.pop_front();
        }
    }
    m_normalizer.process_in_place(m_output.data(), m_output.size(), 2,
                                  sample_rate, m_master_volume.load(), 100);
    alBufferData(buffer, AL_FORMAT_STEREO16, m_output.data(),
                 static_cast<ALsizei>(m_output.size() * sizeof(std::int16_t)),
                 sample_rate);
}

void xbox360_audio_output::thread_main() {
    if (!alcMakeContextCurrent(static_cast<ALCcontext*>(m_context))) {
        m_running = false;
        return;
    }
    for (std::uint32_t buffer : m_buffers) fill_buffer(buffer);
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
