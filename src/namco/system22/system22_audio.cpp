// system22_audio.cpp - Standalone C352 emulation and OpenAL streaming
#include "namco/system22/system22_audio.h"
#include "arcade_audio_output.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

c352_audio::c352_audio() {
    int value = 0;
    for (int i = 0; i < 128; ++i) {
        m_mulaw_table[i] = static_cast<int16_t>(value << 5);
        if (i < 16) value += 1;
        else if (i < 24) value += 2;
        else if (i < 48) value += 4;
        else if (i < 100) value += 8;
        else value += 16;
    }
    for (int i = 0; i < 128; ++i)
        m_mulaw_table[i + 128] = static_cast<int16_t>((~m_mulaw_table[i]) & 0xffe0);
    reset();
}

void c352_audio::set_sample_rom(const uint8_t* data, std::size_t size) {
    if (!data || size == 0) {
        m_sample_rom.clear();
        return;
    }
    m_sample_rom.assign(data, data + size);
}

void c352_audio::reset() {
    m_voices = {};
    m_random = 0x1234;
    m_control = 0;
}

int16_t c352_audio::decode_sample(uint8_t value, bool mulaw) const {
    if (mulaw) return m_mulaw_table[value];
    const int signed_sample = value < 0x80 ? value : static_cast<int>(value) - 0x100;
    return static_cast<int16_t>(signed_sample * 0x100);
}

void c352_audio::fetch_sample(voice& channel) {
    channel.last_sample = channel.sample;

    if (channel.flags & FLAG_NOISE) {
        m_random = static_cast<uint16_t>((m_random >> 1) ^
            ((-(m_random & 1)) & 0xfff6));
        channel.sample = static_cast<int16_t>(m_random);
        return;
    }

    const uint8_t encoded = channel.position < m_sample_rom.size() ?
        m_sample_rom[channel.position] : 0;
    channel.sample = decode_sample(encoded, channel.flags & FLAG_MULAW);
    const uint16_t position = static_cast<uint16_t>(channel.position);

    if ((channel.flags & FLAG_LOOP) && (channel.flags & FLAG_REVERSE)) {
        if ((channel.flags & FLAG_LOOP_DIRECTION) && position == channel.wave_loop)
            channel.flags &= ~FLAG_LOOP_DIRECTION;
        else if (!(channel.flags & FLAG_LOOP_DIRECTION) && position == channel.wave_end)
            channel.flags |= FLAG_LOOP_DIRECTION;
        channel.position += (channel.flags & FLAG_LOOP_DIRECTION) ? -1u : 1u;
    } else if (position == channel.wave_end) {
        if ((channel.flags & FLAG_LINK) && (channel.flags & FLAG_LOOP)) {
            channel.position = (static_cast<uint32_t>(channel.wave_start) << 16) |
                               channel.wave_loop;
            channel.flags |= FLAG_LOOP_HISTORY;
        } else if (channel.flags & FLAG_LOOP) {
            channel.position = (channel.position & 0xff0000) | channel.wave_loop;
            channel.flags |= FLAG_LOOP_HISTORY;
        } else {
            channel.flags |= FLAG_KEYOFF;
            channel.flags &= ~FLAG_BUSY;
            channel.sample = 0;
        }
    } else {
        channel.position += (channel.flags & FLAG_REVERSE) ? -1u : 1u;
    }
}

void c352_audio::ramp_volume(voice& channel, int output, uint8_t target) {
    const int delta = static_cast<int>(channel.current_volume[output]) - target;
    if (delta != 0)
        channel.current_volume[output] += delta > 0 ? -1 : 1;
}

uint16_t c352_audio::read(uint16_t offset) const {
    if (offset < 0x100) {
        const voice& channel = m_voices[offset / 8];
        switch (offset % 8) {
            case 0: return channel.volume_front;
            case 1: return channel.volume_rear;
            case 2: return channel.frequency;
            case 3: return channel.flags;
            case 4: return channel.wave_bank;
            case 5: return channel.wave_start;
            case 6: return channel.wave_end;
            case 7: return channel.wave_loop;
        }
    }
    return offset == 0x200 ? m_control : 0;
}

void c352_audio::write(uint16_t offset, uint16_t data) {
    if (offset < 0x100) {
        voice& channel = m_voices[offset / 8];
        switch (offset % 8) {
            case 0: channel.volume_front = data; break;
            case 1: channel.volume_rear = data; break;
            case 2: channel.frequency = data; break;
            case 3: channel.flags = data; break;
            case 4: channel.wave_bank = data; break;
            case 5: channel.wave_start = data; break;
            case 6: channel.wave_end = data; break;
            case 7: channel.wave_loop = data; break;
        }
        return;
    }
    if (offset == 0x200) {
        m_control = data;
        return;
    }
    if (offset != 0x202) return;

    for (std::size_t index = 0; index < m_voices.size(); ++index) {
        voice& channel = m_voices[index];
        if (channel.flags & FLAG_KEYON) {
            if (std::getenv("RRACER_AUDIO_TRACE")) {
                std::printf("C352 keyon voice=%zu flags=%04x vol=%04x/%04x "
                            "freq=%04x bank=%04x start=%04x end=%04x loop=%04x\n",
                            index, channel.flags, channel.volume_front,
                            channel.volume_rear, channel.frequency,
                            channel.wave_bank, channel.wave_start,
                            channel.wave_end, channel.wave_loop);
            }
            channel.position = (static_cast<uint32_t>(channel.wave_bank) << 16) |
                               channel.wave_start;
            channel.sample = 0;
            channel.last_sample = 0;
            channel.counter = 0xffff;
            channel.flags |= FLAG_BUSY;
            channel.flags &= ~(FLAG_KEYON | FLAG_LOOP_HISTORY);
            channel.current_volume = {};
        }
        if (channel.flags & FLAG_KEYOFF) {
            channel.flags &= ~(FLAG_BUSY | FLAG_KEYOFF);
            channel.counter = 0xffff;
        }
    }
}

void c352_audio::generate_samples(int16_t* output, int frames,
                                  int music_volume, int effects_volume) {
    if (!output || frames <= 0) return;
    std::vector<int32_t> wide(static_cast<std::size_t>(frames) * 2);
    generate_samples_wide(wide.data(), frames, music_volume, effects_volume);
    std::transform(wide.begin(), wide.end(), output, [](int32_t sample) {
        return static_cast<int16_t>(std::clamp(sample, -32768, 32767));
    });
}

void c352_audio::generate_samples_wide(int32_t* output, int frames,
                                       int music_volume,
                                       int effects_volume) {
    if (!output || frames <= 0) return;
    music_volume = std::clamp(music_volume, 0, 100);
    effects_volume = std::clamp(effects_volume, 0, 100);

    for (int frame = 0; frame < frames; ++frame) {
        std::array<int32_t, 4> mixed{};
        for (voice& channel : m_voices) {
            int16_t sample = 0;
            if (channel.flags & FLAG_BUSY) {
                const uint32_t next_counter = channel.counter + channel.frequency;
                if (next_counter & 0x10000) fetch_sample(channel);

                if ((next_counter ^ channel.counter) & 0x18000) {
                    ramp_volume(channel, 0, channel.volume_front >> 8);
                    ramp_volume(channel, 1, channel.volume_front & 0xff);
                    ramp_volume(channel, 2, channel.volume_rear >> 8);
                    ramp_volume(channel, 3, channel.volume_rear & 0xff);
                }
                channel.counter = next_counter & 0xffff;
                sample = channel.sample;
                if ((channel.flags & FLAG_FILTER) == 0) {
                    sample = static_cast<int16_t>(channel.last_sample +
                        (channel.counter * (channel.sample - channel.last_sample) >> 16));
                }
            }

            // C352 has no semantic music/effect tag. Looping voices carry the
            // sequenced music beds in these games; one-shot voices carry SFX
            // and speech. Keeping this policy here makes the UI split explicit
            // and deterministic instead of guessing from sample addresses.
            const int category_volume = (channel.flags & FLAG_LOOP) ?
                music_volume : effects_volume;
            auto mix_output = [sample, category_volume](bool invert,
                                                         uint8_t volume) {
                const int phased_sample = invert ? -sample : sample;
                return ((phased_sample * volume) >> 8) * category_volume / 100;
            };
            mixed[0] += mix_output(channel.flags & FLAG_PHASE_FRONT_LEFT,
                                   channel.current_volume[0]);
            mixed[1] += mix_output(channel.flags & FLAG_PHASE_FRONT_RIGHT,
                                   channel.current_volume[1]);
            mixed[2] += mix_output(channel.flags & FLAG_PHASE_REAR_LEFT,
                                   channel.current_volume[2]);
            mixed[3] += mix_output(channel.flags & FLAG_PHASE_FRONT_RIGHT,
                                   channel.current_volume[3]);
        }

        // System 22 boards expose four physical C352 outputs.  The arcade
        // cabinets may wire all four, while the host backend is stereo, so
        // retain music/effects assigned to the rear pair by downmixing them.
        output[frame * 2] = (mixed[0] + mixed[2]) >> 3;
        output[frame * 2 + 1] = (mixed[1] + mixed[3]) >> 3;
    }
}

int c352_audio::busy_voice_count() const {
    return static_cast<int>(std::count_if(
        m_voices.begin(), m_voices.end(), [](const voice& channel) {
            return (channel.flags & FLAG_BUSY) != 0;
        }));
}

audio_system::audio_system() = default;

audio_system::~audio_system() {
    shutdown();
}

bool audio_system::initialize() {
    if (m_initialized.load()) return true;
    m_output_normalizer.reset();
    for (auto& reg : m_register_snapshot)
        reg.store(0, std::memory_order_relaxed);
    m_device = alcOpenDevice(nullptr);
    if (!m_device) {
        std::fprintf(stderr, "OpenAL: no output device available\n");
        return false;
    }

    m_context = alcCreateContext(static_cast<ALCdevice*>(m_device), nullptr);
    if (!m_context || !alcMakeContextCurrent(static_cast<ALCcontext*>(m_context))) {
        std::fprintf(stderr, "OpenAL: context creation failed\n");
        if (m_context) alcDestroyContext(static_cast<ALCcontext*>(m_context));
        alcCloseDevice(static_cast<ALCdevice*>(m_device));
        m_context = nullptr;
        m_device = nullptr;
        return false;
    }

    alGenSources(1, &m_source);
    alGenBuffers(OPENAL_BUFFER_COUNT, m_buffers.data());
    if (alGetError() != AL_NO_ERROR) {
        std::fprintf(stderr, "OpenAL: source/buffer allocation failed\n");
        shutdown();
        return false;
    }
    alcMakeContextCurrent(nullptr);
    m_initialized = true;
    std::printf("Audio initialized: C352 %d voices at %d Hz, "
                "shared target %.0f RMS\n",
                SYSTEM22_C352_VOICE_COUNT, SYSTEM22_AUDIO_SAMPLE_RATE,
                arcade_audio_output::loudness_normalizer::target_rms);
    return true;
}

void audio_system::shutdown() {
    stop();
    if (!m_context && !m_device) return;

    if (m_context) alcMakeContextCurrent(static_cast<ALCcontext*>(m_context));
    if (m_source) alDeleteSources(1, &m_source);
    alDeleteBuffers(OPENAL_BUFFER_COUNT, m_buffers.data());
    alcMakeContextCurrent(nullptr);
    if (m_context) alcDestroyContext(static_cast<ALCcontext*>(m_context));
    if (m_device) alcCloseDevice(static_cast<ALCdevice*>(m_device));

    m_source = 0;
    m_buffers = {};
    m_context = nullptr;
    m_device = nullptr;
    m_initialized = false;
}

void audio_system::set_sample_rom(const uint8_t* data, std::size_t size) {
    if (m_running.load()) {
        std::fprintf(stderr, "C352 sample ROM cannot be changed while audio is running\n");
        return;
    }
    m_c352.set_sample_rom(data, size);
}

void audio_system::set_mix_levels(int master, int music, int effects) {
    m_master_volume.store(std::clamp(master, 0, 200),
                          std::memory_order_relaxed);
    m_music_volume.store(std::clamp(music, 0, 100),
                         std::memory_order_relaxed);
    m_effects_volume.store(std::clamp(effects, 0, 100),
                           std::memory_order_relaxed);
}

bool audio_system::write_c352(uint16_t offset, uint16_t data) {
    if (offset < m_register_snapshot.size())
        m_register_snapshot[offset].store(data, std::memory_order_release);
    if (offset == 0x202) {
        // Key execution changes the flag registers immediately from the
        // sound CPU's point of view.  Publish that transition before the
        // asynchronous audio thread consumes the command.
        for (int voice = 0; voice < SYSTEM22_C352_VOICE_COUNT; ++voice) {
            auto& flag_register = m_register_snapshot[voice * 8 + 3];
            uint16_t flags = flag_register.load(std::memory_order_acquire);
            if (flags & 0x4000)
                flags = static_cast<uint16_t>((flags | 0x8000) &
                                               ~(0x4000 | 0x0800));
            if (flags & 0x2000)
                flags = static_cast<uint16_t>(flags & ~(0x8000 | 0x2000));
            flag_register.store(flags, std::memory_order_release);
        }
    }

    const uint32_t write = m_command_write.load(std::memory_order_relaxed);
    const uint32_t next = (write + 1) % COMMAND_CAPACITY;
    if (next == m_command_read.load(std::memory_order_acquire)) {
        m_dropped_commands.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    m_commands[write] = {offset, data};
    m_command_write.store(next, std::memory_order_release);
    return true;
}

uint16_t audio_system::read_c352(uint16_t offset) const {
    return offset < m_register_snapshot.size() ?
        m_register_snapshot[offset].load(std::memory_order_acquire) : 0;
}

void audio_system::process_commands() {
    uint32_t read = m_command_read.load(std::memory_order_relaxed);
    const uint32_t write = m_command_write.load(std::memory_order_acquire);
    while (read != write) {
        const command& item = m_commands[read];
        if (std::getenv("RRACER_AUDIO_TRACE") && m_trace_command_count < 512) {
            std::printf("C352 write[%u]: offset=%03x data=%04x\n",
                        m_trace_command_count, item.offset, item.data);
            ++m_trace_command_count;
        }
        m_c352.write(item.offset, item.data);
        if (item.offset < m_register_snapshot.size())
            m_register_snapshot[item.offset].store(
                m_c352.read(item.offset), std::memory_order_release);
        if (item.offset == 0x202) {
            for (int voice = 0; voice < SYSTEM22_C352_VOICE_COUNT; ++voice) {
                const uint16_t offset = static_cast<uint16_t>(voice * 8 + 3);
                m_register_snapshot[offset].store(
                    m_c352.read(offset), std::memory_order_release);
            }
        }
        read = (read + 1) % COMMAND_CAPACITY;
    }
    m_command_read.store(read, std::memory_order_release);
}

void audio_system::fill_buffer(uint32_t buffer) {
    process_commands();
    const int music = m_music_volume.load(std::memory_order_relaxed);
    const int effects = m_effects_volume.load(std::memory_order_relaxed);
    m_c352.generate_samples_wide(m_mix_buffer.data(),
                                 SYSTEM22_AUDIO_BUFFER_FRAMES,
                                 music, effects);
    // Keep the 32-voice sum wide until this single, shared board-output stage.
    // It supplies the same programme target as every other board and replaces
    // the old System 22-only fixed boost that caused Victory Lap to crackle.
    const arcade_audio_output::metrics output =
        m_output_normalizer.process_from_int32(
            m_mix_buffer.data(), m_sample_buffer.data(),
            m_sample_buffer.size(), SYSTEM22_AUDIO_CHANNELS,
            SYSTEM22_AUDIO_SAMPLE_RATE,
            m_master_volume.load(std::memory_order_relaxed),
            (music + effects) / 2);
    m_peak_sample.store(output.peak, std::memory_order_relaxed);
    m_busy_voice_count.store(m_c352.busy_voice_count(),
                             std::memory_order_relaxed);
    // BUSY, KEYOFF and LOOPHIST also change as sample playback advances.
    for (int voice = 0; voice < SYSTEM22_C352_VOICE_COUNT; ++voice) {
        const uint16_t offset = static_cast<uint16_t>(voice * 8 + 3);
        m_register_snapshot[offset].store(
            m_c352.read(offset), std::memory_order_release);
    }
    alBufferData(buffer, AL_FORMAT_STEREO16, m_sample_buffer.data(),
                 static_cast<ALsizei>(m_sample_buffer.size() * sizeof(int16_t)),
                 SYSTEM22_AUDIO_SAMPLE_RATE);
}

void audio_system::audio_loop() {
    if (!alcMakeContextCurrent(static_cast<ALCcontext*>(m_context))) {
        m_running = false;
        return;
    }

    for (uint32_t buffer : m_buffers) fill_buffer(buffer);
    alSourceQueueBuffers(m_source, OPENAL_BUFFER_COUNT, m_buffers.data());
    alSourcePlay(m_source);

    bool source_paused = false;
    while (m_running.load(std::memory_order_acquire)) {
        const bool paused = m_paused.load(std::memory_order_acquire);
        if (paused) {
            if (!source_paused) {
                alSourcePause(m_source);
                source_paused = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (source_paused) {
            alSourcePlay(m_source);
            source_paused = false;
        }
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

void audio_system::start() {
    if (!m_initialized.load() || m_running.exchange(true)) return;
    m_thread = std::thread(&audio_system::audio_loop, this);
}

void audio_system::stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
}
