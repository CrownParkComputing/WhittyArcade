// system22_audio.h - Namco C352 mixer and dedicated OpenAL streaming thread
#pragma once

#include "arcade_audio_output.h"
#include "namco/system22/system22_config.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

// Standalone adaptation of MAME's BSD-3-Clause C352 implementation by
// R. Belmont and superctr. The chip has 32 sample voices and four physical
// outputs; Ridge Racer uses the front stereo pair.
class c352_audio {
public:
    c352_audio();

    void set_sample_rom(const uint8_t* data, std::size_t size);
    void reset();
    void write(uint16_t offset, uint16_t data);
    uint16_t read(uint16_t offset) const;
    void generate_samples(int16_t* stereo_output, int frames,
                          int music_volume = 100,
                          int effects_volume = 100);
    // Wide host path: retain the C352's multi-voice headroom until the shared
    // output normalizer/limiter performs the only int16 conversion.
    void generate_samples_wide(int32_t* stereo_output, int frames,
                               int music_volume = 100,
                               int effects_volume = 100);
    int busy_voice_count() const;

private:
    enum : uint16_t {
        FLAG_BUSY = 0x8000,
        FLAG_KEYON = 0x4000,
        FLAG_KEYOFF = 0x2000,
        FLAG_LOOP_TRIGGER = 0x1000,
        FLAG_LOOP_HISTORY = 0x0800,
        FLAG_FM = 0x0400,
        FLAG_PHASE_REAR_LEFT = 0x0200,
        FLAG_PHASE_FRONT_LEFT = 0x0100,
        FLAG_PHASE_FRONT_RIGHT = 0x0080,
        FLAG_LOOP_DIRECTION = 0x0040,
        FLAG_LINK = 0x0020,
        FLAG_NOISE = 0x0010,
        FLAG_MULAW = 0x0008,
        FLAG_FILTER = 0x0004,
        FLAG_REVERSE_LOOP = 0x0003,
        FLAG_LOOP = 0x0002,
        FLAG_REVERSE = 0x0001,
    };

    struct voice {
        uint32_t position{0};
        uint32_t counter{0};
        int16_t sample{0};
        int16_t last_sample{0};
        uint16_t volume_front{0};
        uint16_t volume_rear{0};
        std::array<uint8_t, 4> current_volume{};
        uint16_t frequency{0};
        uint16_t flags{0};
        uint16_t wave_bank{0};
        uint16_t wave_start{0};
        uint16_t wave_end{0};
        uint16_t wave_loop{0};
    };

    void fetch_sample(voice& channel);
    static void ramp_volume(voice& channel, int output, uint8_t target);
    int16_t decode_sample(uint8_t value, bool mulaw) const;

    std::array<voice, SYSTEM22_C352_VOICE_COUNT> m_voices{};
    std::array<int16_t, 256> m_mulaw_table{};
    std::vector<uint8_t> m_sample_rom;
    uint16_t m_random{0x1234};
    uint16_t m_control{0};
};

class audio_system {
public:
    audio_system();
    ~audio_system();

    bool initialize();
    void shutdown();
    void set_sample_rom(const uint8_t* data, std::size_t size);

    // Single-producer command path used by the emulated C74 sound MCU.
    bool write_c352(uint16_t offset, uint16_t data);
    uint16_t read_c352(uint16_t offset) const;

    void start();
    void stop();
    bool initialized() const { return m_initialized.load(); }
    uint64_t dropped_commands() const { return m_dropped_commands.load(); }
    int peak_sample() const { return m_peak_sample.load(); }
    int busy_voice_count() const { return m_busy_voice_count.load(); }
    void set_mix_levels(int master, int music, int effects);
    void set_paused(bool paused) { m_paused.store(paused); }
    int master_volume() const { return m_master_volume.load(); }
    int music_volume() const { return m_music_volume.load(); }
    int effects_volume() const { return m_effects_volume.load(); }
    void set_driving_feedback(bool enabled) {
        m_driving_feedback.store(enabled, std::memory_order_release);
    }

private:
    struct command {
        uint16_t offset;
        uint16_t data;
    };

    static constexpr uint32_t COMMAND_CAPACITY = 2048;
    static constexpr int OPENAL_BUFFER_COUNT = 4;

    void audio_loop();
    void process_commands();
    void fill_buffer(uint32_t buffer);

    void* m_device{nullptr};
    void* m_context{nullptr};
    uint32_t m_source{0};
    std::array<uint32_t, OPENAL_BUFFER_COUNT> m_buffers{};

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_paused{false};
    std::thread m_thread;

    c352_audio m_c352;
    static constexpr std::size_t SAMPLE_BUFFER_SIZE =
        static_cast<std::size_t>(SYSTEM22_AUDIO_BUFFER_FRAMES) *
        SYSTEM22_AUDIO_CHANNELS;
    std::array<int16_t, SAMPLE_BUFFER_SIZE> m_sample_buffer{};
    std::array<int32_t, SAMPLE_BUFFER_SIZE> m_mix_buffer{};
    arcade_audio_output::loudness_normalizer m_output_normalizer;

    std::array<command, COMMAND_CAPACITY> m_commands{};
    std::atomic<uint32_t> m_command_write{0};
    std::atomic<uint32_t> m_command_read{0};
    std::atomic<uint64_t> m_dropped_commands{0};
    std::atomic<int> m_peak_sample{0};
    std::atomic<int> m_busy_voice_count{0};
    std::atomic<int> m_master_volume{100};
    std::atomic<int> m_music_volume{100};
    std::atomic<int> m_effects_volume{100};
    std::atomic<bool> m_driving_feedback{false};
    std::array<std::atomic<uint16_t>, 0x800> m_register_snapshot{};
    uint32_t m_trace_command_count{0};
};
