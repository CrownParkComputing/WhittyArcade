#pragma once

#include "arcade_audio_output.h"
#include "gng_rom.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace gng {

// Owns the 3 MHz sound Z80, both 1.5 MHz YM2203s and the OpenAL stream.
class audio_system {
public:
    static constexpr int sample_rate = 48000;
    static constexpr int buffer_count = 4;
    static constexpr int buffer_frames = 800;

    audio_system();
    ~audio_system();
    audio_system(const audio_system&) = delete;
    audio_system& operator=(const audio_system&) = delete;

    bool initialize(const roms& rom_data);
    void start();
    void stop();
    void shutdown();
    void write_command(uint8_t value);
    void set_reset_line(bool asserted);
    void set_mix_levels(int master, int music, int effects);
    void set_paused(bool paused) { m_paused.store(paused); }
    int peak_sample() const { return m_peak.load(); }

private:
    struct synth;
    void audio_loop();
    void fill_buffer(uint32_t buffer);

    std::unique_ptr<synth> m_synth;
    void* m_device{};
    void* m_context{};
    uint32_t m_source{};
    std::array<uint32_t, buffer_count> m_buffers{};
    std::array<int16_t, buffer_frames> m_samples{};
    arcade_audio_output::loudness_normalizer m_normalizer;
    std::thread m_thread;
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_reset{true};
    std::atomic<uint8_t> m_command{0};
    std::atomic<int> m_master{100}, m_music{100}, m_effects{100};
    std::atomic<int> m_peak{0};
};

} // namespace gng
