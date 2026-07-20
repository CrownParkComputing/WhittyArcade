// Galaxian-family audio synthesis + OpenAL streaming worker.
//
// The two supported Galaxian-family games (Phoenix, Moon Cresta) use
// qualitatively different synthesis models (Phoenix: 4-voice square + LFSR
// noise + MM6221 melody; Moon Cresta: scalar LFO + pitch-latched melody +
// HIT/FIRE envelopes) but the OpenAL worker is byte-for-byte identical
// between them. This header declares a single concrete worker that takes
// the synth as a runtime parameter; per-game implementations live in
// galaxian_audio_phoenix.cpp and galaxian_audio_mooncrst.cpp.

#pragma once

#include "arcade_audio_output.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Pure-synthesis interface. No OpenAL, no threading. A concrete synth
// receives write_control events from the Z80 hot loop and renders
// interleaved int16 frames into the worker's scratch buffer on demand.
class galaxian_sound_synth {
public:
    static constexpr int sample_rate = 48'000;

    virtual ~galaxian_sound_synth() = default;

    virtual void reset() = 0;
    virtual void write_control(unsigned port, uint8_t data) = 0;
    virtual void generate(int16_t* output, int frames,
                          int music_volume, int effects_volume) = 0;
    // Index of the currently-playing tune. Phoenix returns 1..3; Moon
    // Cresta returns the position in its 16-entry melody loop. Used by
    // the heartbeat printf only.
    virtual int active_tune() const = 0;
};

// Internal control tags used between the Moon Cresta memory-map adapter and
// its synth.  Keep the three hardware register families disjoint: A800-A807
// are the discrete sound lines, B800 is the pitch latch, and A004-A007 are
// the four LFO/DAC bits.
namespace mooncrst_audio_port {
constexpr unsigned sound_base = 0x80;
constexpr unsigned pitch = 0x90;
constexpr unsigned lfo_base = 0xa0;
}  // namespace mooncrst_audio_port

// OpenAL streaming worker shared by all Galaxian-family games. The
// worker owns its own device / context / source / buffer queue and a
// dedicated std::thread that fills the queued buffers with samples
// produced by the supplied synth. write_control(port, data) is the
// entry point the Z80 hot loop calls. Writes are queued in order and applied
// by the audio thread at the next buffer boundary, so the CPU and synth never
// race over analogue state.
//
// port values >= kMaxControls are stored with the low bits used as the
// latch index; the synth is responsible for interpreting the high bits
// (Moon Cresta uses the tagged families above).
class galaxian_audio_system {
public:
    static constexpr std::size_t kMaxControls = 8;
    static constexpr int buffer_count = 4;
    static constexpr int buffer_frames = 800;

    explicit galaxian_audio_system(
        std::unique_ptr<galaxian_sound_synth> synth);
    ~galaxian_audio_system();

    galaxian_audio_system(const galaxian_audio_system&) = delete;
    galaxian_audio_system& operator=(const galaxian_audio_system&) = delete;

    bool initialize();
    void shutdown();
    void start();
    void stop();
    void write_control(unsigned port, uint8_t data);
    void set_mix_levels(int master, int music, int effects);
    void set_paused(bool paused) { m_paused.store(paused); }
    int peak_sample() const { return m_peak_sample.load(); }
    uint8_t control(unsigned port) const;

private:
    struct control_write {
        unsigned port;
        uint8_t data;
    };

    void fill_buffer(uint32_t buffer);
    void audio_loop();
    void apply_pending_controls();

    std::unique_ptr<galaxian_sound_synth> m_synth;
    std::array<std::atomic<uint8_t>, kMaxControls> m_controls{};
    std::mutex m_control_mutex;
    std::vector<control_write> m_pending_controls;

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

    std::array<int16_t, buffer_frames> m_samples{};
    arcade_audio_output::loudness_normalizer m_output_normalizer;
    std::thread m_thread;
};

// Per-game factory functions. Defined in galaxian_audio_phoenix.cpp and
// galaxian_audio_mooncrst.cpp respectively. The frontend session picks
// the right one based on the galaxian_rom_set result.
std::unique_ptr<galaxian_sound_synth> make_phoenix_sound_synth();
std::unique_ptr<galaxian_sound_synth> make_mooncrst_sound_synth();
