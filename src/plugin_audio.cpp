#include "plugin_audio.h"

#include "arcade_audio_output.h"
#include "arcade_feedback.h"

#include <AL/al.h>
#include <AL/alc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

constexpr std::size_t voice_count = 24;
constexpr uint32_t synth_rate = 22050;

uint32_t read_u32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t read_u16(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
}

bool name_mentions(const std::string& name, const char* word) {
    return name.find(word) != std::string::npos;
}

float cue_feedback_strength(const std::string& name) {
    if (name_mentions(name, "death") || name_mentions(name, "crash") ||
        name_mentions(name, "explo") || name_mentions(name, "bomb") ||
        name_mentions(name, "collapse"))
        return 1.0f;
    if (name_mentions(name, "hit") || name_mentions(name, "land") ||
        name_mentions(name, "magic"))
        return 0.72f;
    if (name_mentions(name, "shot") || name_mentions(name, "fire"))
        return 0.38f;
    if (name_mentions(name, "jump") || name_mentions(name, "kick") ||
        name_mentions(name, "punch"))
        return 0.45f;
    return 0.0f;
}

// A tiny deterministic noise source. std::rand would make the synthesised
// sounds differ between runs and between machines, which would make a
// difference in what two players hear look like a bug in the cue wiring.
struct noise {
    uint32_t state;
    float next() noexcept {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state & 0xffffu) / 32768.0f - 1.0f;
    }
};

} // namespace

pcm_sound parse_wav(const uint8_t* data, std::size_t size) {
    pcm_sound sound;
    if (data == nullptr || size < 44) return sound;
    if (std::memcmp(data, "RIFF", 4) != 0 ||
        std::memcmp(data + 8, "WAVE", 4) != 0)
        return sound;

    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t rate = 0;
    const uint8_t* pcm = nullptr;
    uint32_t pcm_size = 0;

    // Chunk walk rather than assuming fmt-then-data at fixed offsets: real
    // files carry LIST/INFO chunks between them, and a fixed-offset reader
    // turns those into noise.
    std::size_t offset = 12;
    while (offset + 8 <= size) {
        const uint32_t chunk_size = read_u32(data + offset + 4);
        const uint8_t* body = data + offset + 8;
        if (offset + 8 + chunk_size > size) break;
        if (std::memcmp(data + offset, "fmt ", 4) == 0 && chunk_size >= 16) {
            const uint16_t format = read_u16(body);
            // 1 = PCM, 0xFFFE = extensible, whose first 16 bytes match PCM.
            if (format != 1 && format != 0xFFFE) return sound;
            channels = read_u16(body + 2);
            rate = read_u32(body + 4);
            bits = read_u16(body + 14);
        } else if (std::memcmp(data + offset, "data", 4) == 0) {
            pcm = body;
            pcm_size = chunk_size;
        }
        offset += 8 + chunk_size + (chunk_size & 1u); // chunks are word-aligned
    }

    if (pcm == nullptr || channels == 0 || rate == 0) return sound;
    if (bits != 8 && bits != 16) return sound;

    const uint32_t bytes_per_frame = channels * (bits / 8u);
    if (bytes_per_frame == 0) return sound;
    const uint32_t frames = pcm_size / bytes_per_frame;
    sound.sample_rate = rate;
    sound.samples.resize(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        int32_t total = 0;
        for (uint32_t c = 0; c < channels; ++c) {
            const uint8_t* s = pcm + i * bytes_per_frame + c * (bits / 8u);
            // 8-bit WAV is unsigned with 128 as silence; treating it as signed
            // produces a loud square wave, which is unmistakable but wrong.
            total += bits == 8
                         ? (static_cast<int32_t>(*s) - 128) * 256
                         : static_cast<int16_t>(read_u16(s));
        }
        sound.samples[i] = static_cast<int16_t>(total / channels);
    }
    return sound;
}

pcm_sound load_wav_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    return parse_wav(bytes.data(), bytes.size());
}

pcm_sound synthesise_cue(const std::string& name) {
    // Seeded from the name so every cue gets its own character and the same
    // name always sounds the same.
    uint32_t seed = 2166136261u;
    for (const char c : name) {
        seed ^= static_cast<uint8_t>(c);
        seed *= 16777619u;
    }
    noise rng{seed | 1u};

    float duration = 0.12f;
    float start_hz = 440.0f;
    float end_hz = 220.0f;
    float noise_mix = 0.0f;
    float square = 0.0f;

    if (name_mentions(name, "shot") || name_mentions(name, "fire")) {
        duration = 0.07f;
        start_hz = 900.0f;
        end_hz = 260.0f;
        square = 0.5f;
    } else if (name_mentions(name, "player_death") ||
               name_mentions(name, "bomb") || name_mentions(name, "explo")) {
        duration = 0.60f;
        start_hz = 320.0f;
        end_hz = 40.0f;
        noise_mix = 0.75f;
    } else if (name_mentions(name, "death") || name_mentions(name, "hit") ||
               name_mentions(name, "collapse")) {
        duration = 0.18f;
        start_hz = 620.0f;
        end_hz = 90.0f;
        noise_mix = 0.45f;
    } else if (name_mentions(name, "spawn")) {
        duration = 0.14f;
        start_hz = 180.0f;
        end_hz = 700.0f; // rising: something arriving, not leaving
        noise_mix = 0.15f;
    } else if (name_mentions(name, "multiplier") ||
               name_mentions(name, "pickup") || name_mentions(name, "up")) {
        duration = 0.16f;
        start_hz = 660.0f;
        end_hz = 1320.0f;
        square = 0.35f;
    }

    pcm_sound sound;
    sound.sample_rate = synth_rate;
    const std::size_t count =
        static_cast<std::size_t>(duration * static_cast<float>(synth_rate));
    sound.samples.resize(count);
    float phase = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count);
        const float hz = start_hz + (end_hz - start_hz) * t;
        phase += hz / static_cast<float>(synth_rate);
        if (phase >= 1.0f) phase -= 1.0f;
        const float sine = std::sin(phase * 6.28318530718f);
        const float squared = phase < 0.5f ? 1.0f : -1.0f;
        float value = sine * (1.0f - square) + squared * square;
        value = value * (1.0f - noise_mix) + rng.next() * noise_mix;
        // Exponential decay with a short attack: an instant onset clicks, and
        // a click is the one artefact that survives every later mixing stage.
        const float attack = t < 0.01f ? t / 0.01f : 1.0f;
        const float envelope = attack * std::exp(-4.5f * t);
        sound.samples[i] =
            static_cast<int16_t>(std::clamp(value * envelope * 11000.0f,
                                            -32768.0f, 32767.0f));
    }
    return sound;
}

resolved_cue resolve_cue(const std::string& bundle_path,
                        const std::string& name) {
    resolved_cue resolved;
    const fs::path sfx_dir = fs::path(bundle_path) / "sfx";
    std::error_code ec;
    // .wav first, then .WAV: an extracted bundle may come from a case
    // preserving file system where the case is not ours to choose.
    for (const char* extension : {".wav", ".WAV"}) {
        const fs::path candidate = sfx_dir / (name + extension);
        if (!fs::is_regular_file(candidate, ec)) continue;
        resolved.sound = load_wav_file(candidate.string());
        if (resolved.sound.valid()) {
            resolved.source = candidate.string();
            resolved.from_bundle = true;
            return resolved;
        }
        // A file that is present but unreadable is worth saying out loud: it
        // looks installed, and silently synthesising over it would leave
        // someone convinced their asset pipeline worked.
        std::fprintf(stderr, "plugin audio: %s is not readable PCM WAV\n",
                     candidate.string().c_str());
    }
    resolved.sound = synthesise_cue(name);
    resolved.source = "synthesised";
    return resolved;
}

plugin_audio::~plugin_audio() {
    if (m_context == nullptr) return;
    for (const uint32_t source : m_sources) {
        alSourceStop(source);
        alDeleteSources(1, &source);
    }
    for (const cue_sound& cue : m_cues)
        if (cue.buffer != 0) alDeleteBuffers(1, &cue.buffer);
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(static_cast<ALCcontext*>(m_context));
    if (m_device != nullptr) alcCloseDevice(static_cast<ALCdevice*>(m_device));
}

bool plugin_audio::initialize(const std::vector<std::string>& cue_names,
                              const std::string& bundle_path,
                              int master_volume, int effects_volume) {
    if (cue_names.empty()) return true; // a silent game is not a failure

    ALCdevice* device = alcOpenDevice(nullptr);
    if (device == nullptr) {
        std::fprintf(stderr, "plugin audio: no output device; running silent\n");
        return false;
    }
    ALCcontext* context = alcCreateContext(device, nullptr);
    if (context == nullptr || alcMakeContextCurrent(context) == ALC_FALSE) {
        if (context != nullptr) alcDestroyContext(context);
        alcCloseDevice(device);
        std::fprintf(stderr, "plugin audio: no context; running silent\n");
        return false;
    }
    m_device = device;
    m_context = context;
    set_volume(master_volume, effects_volume);

    m_cues.reserve(cue_names.size());
    for (const std::string& name : cue_names) {
        cue_sound cue;
        cue.name = name;
        const resolved_cue resolved = resolve_cue(bundle_path, name);
        const pcm_sound& sound = resolved.sound;
        cue.source = resolved.source;
        cue.feedback_strength = cue_feedback_strength(name);
        if (resolved.from_bundle) ++m_sampled;
        ALuint buffer = 0;
        alGenBuffers(1, &buffer);
        alBufferData(buffer, AL_FORMAT_MONO16, sound.samples.data(),
                     static_cast<ALsizei>(sound.samples.size() *
                                          sizeof(int16_t)),
                     static_cast<ALsizei>(sound.sample_rate));
        cue.buffer = buffer;
        m_cues.push_back(std::move(cue));
    }

    // A fixed pool of voices. Sounds are short and several can land on one
    // frame, so one source per cue would cut a sound off every time the same
    // event happened twice - which in this game is most of the time.
    m_sources.resize(voice_count);
    alGenSources(static_cast<ALsizei>(m_sources.size()), m_sources.data());
    for (const uint32_t source : m_sources) {
        alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSourcef(source, AL_ROLLOFF_FACTOR, 0.0f);
    }
    std::printf("plugin audio: %zu cue(s), %zu from bundle samples\n",
                m_cues.size(), m_sampled);
    return true;
}

void plugin_audio::set_volume(int master_volume, int effects_volume) {
    const float master = static_cast<float>(std::clamp(master_volume, 0, 200));
    const float effects = static_cast<float>(std::clamp(effects_volume, 0, 200));
    m_volume = (master / 100.0f) * (effects / 100.0f);
}

void plugin_audio::set_paused(bool paused) {
    m_paused = paused;
    if (!paused || m_context == nullptr) return;
    // Stop rather than pause: these are one-shots, and resuming the tail of a
    // sound whose cause was minutes ago is just confusing.
    for (const uint32_t source : m_sources) alSourceStop(source);
}

void plugin_audio::play(uint32_t cue, float gain, float pan) {
    if (m_context == nullptr || m_paused || m_sources.empty()) return;
    if (cue >= m_cues.size()) return;
    if (arcade_audio_output::output_muted()) return;

    // Round-robin, skipping voices still sounding. Full pool means the frame
    // asked for more than 24 simultaneous sounds; dropping is correct.
    uint32_t chosen = 0;
    bool found = false;
    for (std::size_t i = 0; i < m_sources.size(); ++i) {
        const uint32_t source =
            m_sources[(m_next_source + i) % m_sources.size()];
        ALint state = 0;
        alGetSourcei(source, AL_SOURCE_STATE, &state);
        if (state == AL_PLAYING) continue;
        chosen = source;
        m_next_source = (m_next_source + i + 1) % m_sources.size();
        found = true;
        break;
    }
    if (!found) return;

    const float level = std::clamp(gain, 0.0f, 1.0f) * m_volume;
    arcade_feedback::publish_impact(
        m_cues[cue].feedback_strength * std::clamp(gain, 0.0f, 1.0f));
    const float position = std::clamp(pan, -1.0f, 1.0f);
    alSourceStop(chosen);
    alSourcei(chosen, AL_BUFFER, static_cast<ALint>(m_cues[cue].buffer));
    alSourcef(chosen, AL_GAIN, level);
    // Placed on a unit half-circle rather than straight along x: a source at
    // x = -1, y = z = 0 sits inside the listener's head, where OpenAL's
    // distance model gives it an arbitrary gain instead of a clean pan.
    alSource3f(chosen, AL_POSITION, position, 0.0f,
               -std::sqrt(std::max(0.0f, 1.0f - position * position)));
    alSourcePlay(chosen);
}

const std::string& plugin_audio::source_of(uint32_t cue) const {
    static const std::string none;
    return cue < m_cues.size() ? m_cues[cue].source : none;
}
