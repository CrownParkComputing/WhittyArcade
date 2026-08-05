// Taito Z sound board: a YM2610 (4-op FM + 3-channel SSG + ADPCM-A and
// ADPCM-B/Delta-T sample playback) driven by a Z80, with the sample ROMs
// supplied through ymfm's external-read hook.
//
// The Z80 and the TC0140SYT mailbox live in the machine, next to the
// 68000s they talk to; this header is the chip plus the OpenAL streaming
// worker, following the same split as system16b_audio.h.
#pragma once

#include "arcade_audio_output.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

class taitoz_sound_synth {
public:
    static constexpr int sample_rate = 48'000;
    // 16 MHz / 2, per the board's YM2610 wiring.
    static constexpr int chip_clock_hz = 8'000'000;

    taitoz_sound_synth();
    ~taitoz_sound_synth();
    taitoz_sound_synth(const taitoz_sound_synth&) = delete;
    taitoz_sound_synth& operator=(const taitoz_sound_synth&) = delete;

    void reset();

    // The Z80 sees the chip as four byte ports at 0xe000-0xe003.
    void write_port(unsigned port, uint8_t data);
    uint8_t read_port(unsigned port);

    // Sample ROMs; the caller owns them and must outlive the synth.
    void set_adpcm_a(const uint8_t* rom, std::size_t bytes);
    void set_adpcm_b(const uint8_t* rom, std::size_t bytes);

    // Advances the chip's timers in chip-clock units. Timer time belongs
    // to the emulated board rather than to audio buffer consumption, and
    // the YM2610's timer IRQ is what paces the sound driver, so the
    // machine drives this from Z80 execution.
    void advance_timer_clocks(uint32_t clocks);
    bool irq_asserted() const;
    // Diagnostics: how many times the driver armed a timer, how many
    // expiries fired, and how many IRQ assertions reached the Z80.
    void debug_counters(unsigned& scheduled, unsigned& expired,
                        unsigned& irqs) const;
    // Diagnostics: ADPCM sample-ROM fetches ymfm made.
    void adpcm_counters(unsigned& reads, unsigned& a_reads,
                        unsigned& a_oob) const;

    // Render interleaved int16 stereo frames.
    void generate(int16_t* output, int frames);

private:
    struct implementation;
    std::unique_ptr<implementation> m_impl;
};

class taitoz_audio_system {
public:
    static constexpr int buffer_count  = 4;
    static constexpr int buffer_frames = 800;
    static constexpr std::size_t buffer_samples =
        static_cast<std::size_t>(buffer_frames) * 2;

    explicit taitoz_audio_system(std::unique_ptr<taitoz_sound_synth> synth);
    ~taitoz_audio_system();
    taitoz_audio_system(const taitoz_audio_system&) = delete;
    taitoz_audio_system& operator=(const taitoz_audio_system&) = delete;

    bool initialize();
    void shutdown();
    void start();
    void stop();

    // Chip access from the sound-CPU thread. These take the same short
    // chip lock the audio worker uses, so register ordering is preserved
    // and a timer-status clear is visible to the polling Z80 at once.
    void write_port(unsigned port, uint8_t data);
    uint8_t read_port(unsigned port);
    void advance_timer_clocks(uint32_t clocks);
    bool irq_asserted();

    void set_mix_levels(int master, int music, int effects);
    void set_paused(bool paused) { m_paused.store(paused); }
    int peak_sample() const { return m_peak_sample.load(); }

private:
    void fill_buffer(uint32_t buffer);
    void audio_loop();

    std::unique_ptr<taitoz_sound_synth> m_synth;
    std::mutex m_synth_mutex;

    void* m_device{nullptr};
    void* m_context{nullptr};
    uint32_t m_source{0};
    std::array<uint32_t, buffer_count> m_buffers{};

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};
    std::atomic<int> m_master_volume{100};
    std::atomic<int> m_music_volume{100};
    std::atomic<int> m_effects_volume{100};
    std::atomic<int> m_peak_sample{0};

    std::array<int16_t, buffer_samples> m_samples{};
    arcade_audio_output::loudness_normalizer m_output_normalizer;
    std::thread m_thread;
};

std::unique_ptr<taitoz_sound_synth> make_taitoz_sound_synth();
