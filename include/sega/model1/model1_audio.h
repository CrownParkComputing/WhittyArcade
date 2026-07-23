// Sega Model 1 sound board: 68000 + YM3438 + dual MultiPCM on one worker.
#pragma once

#include "arcade_audio_output.h"
#include "sega/model1/model1_rom.h"
#include "musashi_memory.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <mutex>
#include <memory>
#include <vector>

struct multipcm;
class model1_dsb;

class model1_audio_system : public musashi_memory {
public:
    model1_audio_system();
    ~model1_audio_system();

    model1_audio_system(const model1_audio_system&) = delete;
    model1_audio_system& operator=(const model1_audio_system&) = delete;

    bool initialize(const model1_roms& roms);
    void shutdown();
    void start();
    void stop();
    bool enqueue_uart(uint8_t data, uint64_t v60_timestamp);
    // Advance the audio by one video frame. With block=true (Model 1) the
    // caller's CPU thread is throttled to the audio buffer, which is how the
    // Model 1 V60 keeps real time. With block=false (Model 2 / Virtua Cop) the
    // call never waits: if the audio has fallen behind, this frame's advance is
    // dropped so the i960 CPU worker is never frozen on the audio device.
    void signal_frame(bool block = true);
    void set_mix_levels(int master, int music, int effects);
    // Freeze the emulated sound board without tearing down the stream. While
    // paused the audio thread keeps advancing its frame clock (so queued UART
    // timestamps stay aligned) but renders silence instead of running the
    // 68000. Used when a host menu pauses the Model 2 machine.
    void set_paused(bool paused) {
        m_paused.store(paused, std::memory_order_relaxed);
    }

    bool initialized() const { return m_initialized.load(); }
    uint32_t program_counter() const { return m_program_counter.load(); }
    uint64_t executed_cycles() const { return m_executed_cycles.load(); }
    uint64_t uart_bytes() const { return m_uart_bytes.load(); }
    uint64_t dropped_uart_bytes() const { return m_dropped_uart.load(); }
    int peak_sample() const { return m_peak_sample.load(); }
    uint64_t ym_writes() const { return m_ym_writes.load(); }
    uint64_t pcm_writes() const { return m_pcm_writes.load(); }
    int active_pcm_voices() const { return m_active_pcm_voices.load(); }
    bool dsb_active() const;
    uint64_t dsb_triggers() const;

    uint8_t read8(uint32_t address) override;
    uint16_t read16(uint32_t address) override;
    uint32_t read32(uint32_t address) override;
    void write8(uint32_t address, uint8_t value) override;
    void write16(uint32_t address, uint16_t value) override;
    void write32(uint32_t address, uint32_t value) override;

private:
    struct ym3438_state;

    static constexpr uint32_t UART_CAPACITY = 1024;
    static constexpr uint32_t UART_RX_CAPACITY = 16;
    static constexpr int OPENAL_BUFFER_COUNT = 4;
    static constexpr int BUFFER_FRAMES = 800;
    static constexpr int OUTPUT_RATE = 48000;
    static constexpr std::size_t OUTPUT_SAMPLES =
        static_cast<std::size_t>(BUFFER_FRAMES) * 2;

    void audio_loop();
    void fill_buffer(uint32_t buffer);
    int execute_sound_cpu(int requested_cycles);
    void deliver_uart(uint64_t v60_time);
    uint8_t read_uart_data();
    uint8_t read_uart_status();
    void tick_chips(int cycles);
    void push_ym_sample(int32_t left, int32_t right);
    std::vector<int32_t> take_ym_samples(std::size_t count);
    struct resampler_state {
        uint64_t phase{};
        uint64_t step{};
        int32_t last_left{};
        int32_t last_right{};
        int32_t next_left{};
        int32_t next_right{};
        bool primed{};
    };
    static std::size_t source_frames_needed(const resampler_state& state,
                                            int output_frames);
    static void resample_add(resampler_state& state,
                             const std::vector<int32_t>& source,
                             int32_t* output, int output_frames,
                             int gain_percent);

    std::vector<uint8_t> m_rom;
    std::vector<uint8_t> m_ram;
    std::vector<uint8_t> m_samples_1;
    std::vector<uint8_t> m_samples_2;
    multipcm* m_pcm_1{};
    multipcm* m_pcm_2{};
    std::unique_ptr<model1_dsb> m_dsb;
    std::unique_ptr<ym3438_state> m_ym;

    std::array<uint8_t, UART_CAPACITY> m_uart_transport{};
    std::array<uint64_t, UART_CAPACITY> m_uart_timestamp{};
    std::atomic<uint32_t> m_uart_write{};
    std::atomic<uint32_t> m_uart_read{};
    std::array<uint8_t, UART_RX_CAPACITY> m_uart_rx{};
    uint32_t m_uart_rx_write{};
    uint32_t m_uart_rx_read{};
    uint32_t m_uart_rx_count{};
    bool m_uart_overrun{};

    std::array<int32_t, std::size_t{8192} * 2> m_ym_ring{};
    uint32_t m_ym_ring_write{};
    uint32_t m_ym_ring_read{};
    uint64_t m_ym_clock_fraction{};
    uint64_t m_sound_frame_base{};
    resampler_state m_ym_resampler{};
    resampler_state m_pcm_1_resampler{};
    resampler_state m_pcm_2_resampler{};

    void* m_device{};
    void* m_context{};
    uint32_t m_source{};
    std::array<uint32_t, OPENAL_BUFFER_COUNT> m_buffers{};
    std::array<int16_t, OUTPUT_SAMPLES> m_output{};
    arcade_audio_output::loudness_normalizer m_output_normalizer;

    std::atomic<bool> m_running{};
    std::atomic<bool> m_initialized{};
    std::atomic<bool> m_paused{};
    std::thread m_thread;
    std::mutex m_frame_mutex;
    std::condition_variable m_frame_ready;
    unsigned m_pending_frames{};
    std::atomic<int> m_master_volume{100};
    std::atomic<int> m_music_volume{100};
    std::atomic<int> m_effects_volume{100};
    std::atomic<int> m_peak_sample{};
    std::atomic<uint32_t> m_program_counter{};
    std::atomic<uint64_t> m_executed_cycles{};
    std::atomic<uint64_t> m_uart_bytes{};
    std::atomic<uint64_t> m_dropped_uart{};
    std::atomic<uint64_t> m_ym_writes{};
    std::atomic<uint64_t> m_pcm_writes{};
    std::atomic<int> m_active_pcm_voices{};
};
