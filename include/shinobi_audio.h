// Shinobi sound synthesis (YM2151 + SegaPCM) and OpenAL streaming worker.
//
// The model follows galaxian_audio.h: a pure-synthesis interface plus an
// OpenAL streaming worker that owns its device/context/source/buffers
// and runs a fill thread. Shinobi's synth drives ymfm's opm_chip
// (YM2151) plus the segapcm chip, mixes their output, and supplies
// int16 frames on demand.
#pragma once

#include "arcade_audio_output.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

// Forward-declare the ymfm/segapcm integration types so we don't pollute
// every includer with the ymfm / segapcm headers.
namespace ymfm { template <typename T> class opm_chip; class opm_callbacks; }
class segapcm;

class shinobi_sound_synth {
public:
    static constexpr int sample_rate = 48'000;
    static constexpr int chip_clock_hz = 4'000'000;

    shinobi_sound_synth();
    ~shinobi_sound_synth();

    shinobi_sound_synth(const shinobi_sound_synth&) = delete;
    shinobi_sound_synth& operator=(const shinobi_sound_synth&) = delete;

    void reset();
    void write_control(unsigned port, uint8_t data);
    // YM2151 status register (busy + timer flags). The sound program polls
    // timer A to pace its sequencer.
    uint8_t read_status();

    // Advance the YM2151's free-running hardware timers in chip-clock
    // units. Timer time belongs to the emulated board, not to OpenAL buffer
    // consumption, so the machine runtime calls this from Z80 execution.
    void advance_timer_clocks(uint32_t clocks);

    // Render interleaved int16 frames (L|R|L|R|...).
    void generate(int16_t* output, int frames);

    // Set the 64 KiB SegaPCM bank 0 ROM (caller owns the buffer).
    void set_pcm_rom(const uint8_t* rom, std::size_t bytes);

    int active_tune() const { return 0; }   // for parity with galaxian_audio

private:
    struct implementation;
    std::unique_ptr<implementation> m_impl;
};

class shinobi_audio_system {
public:
    static constexpr std::size_t kMaxControls = 8;
    static constexpr int buffer_count   = 4;
    static constexpr int buffer_frames  = 800;
    static constexpr std::size_t buffer_samples =
        static_cast<std::size_t>(buffer_frames) * 2;

    explicit shinobi_audio_system(std::unique_ptr<shinobi_sound_synth> synth);
    ~shinobi_audio_system();

    shinobi_audio_system(const shinobi_audio_system&) = delete;
    shinobi_audio_system& operator=(const shinobi_audio_system&) = delete;

    bool initialize();
    void shutdown();
    void start();
    void stop();
    void write_control(unsigned port, uint8_t data);
    uint8_t read_status();
    void advance_timer_clocks(uint32_t clocks);
    void set_mix_levels(int master, int music, int effects);
    void set_paused(bool paused) { m_paused.store(paused); }
    int  peak_sample() const { return m_peak_sample.load(); }

private:
    void fill_buffer(uint32_t buffer);
    void audio_loop();

    std::unique_ptr<shinobi_sound_synth> m_synth;

    // YM register writes/status reads occur on the sound-CPU thread while
    // sample generation runs on the audio worker. A short chip-state lock
    // preserves exact register ordering and, critically, makes timer-status
    // clears visible immediately to the polling Z80.
    std::mutex m_synth_mutex;

    void*   m_device{nullptr};
    void*   m_context{nullptr};
    uint32_t m_source{0};
    std::array<uint32_t, buffer_count> m_buffers{};

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};
    std::atomic<int>  m_master_volume{100};
    std::atomic<int>  m_music_volume{100};
    std::atomic<int>  m_effects_volume{100};
    std::atomic<int>  m_peak_sample{0};

    std::array<int16_t, buffer_samples> m_samples{};   // interleaved stereo
    arcade_audio_output::loudness_normalizer m_output_normalizer;
    std::thread m_thread;
};

std::unique_ptr<shinobi_sound_synth> make_shinobi_sound_synth();
