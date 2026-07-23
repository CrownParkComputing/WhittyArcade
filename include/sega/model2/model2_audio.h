// Sega Model 2A sound board: MC68000 + Yamaha YMF292-F (SCSP).
#pragma once

#include "arcade_audio_output.h"
#include "sega/model2/model2_rom.h"
#include "sega/model2/model2_scsp_dsp.h"
#include "musashi_memory.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

class model2_audio_system final : public musashi_memory {
public:
    model2_audio_system();
    ~model2_audio_system();

    model2_audio_system(const model2_audio_system&) = delete;
    model2_audio_system& operator=(const model2_audio_system&) = delete;

    bool initialize(const model2_roms& roms);
    void shutdown();
    void start();
    void stop();
    bool enqueue_midi(uint8_t data);
    // SCSP MIDI-out: bytes the 68000 sound program transmits back to the
    // main CPU through the uPD71051C receive channel.
    void set_midi_out_callback(std::function<void(uint8_t)> callback) {
        m_midi_out_callback = std::move(callback);
    }
    // Freeze the emulated sound board without tearing down the stream. The
    // audio thread keeps the OpenAL queue fed with silence while paused.
    void set_paused(bool paused) {
        m_paused.store(paused, std::memory_order_relaxed);
    }
    void set_mix_levels(int master, int music, int effects);

    uint32_t program_counter() const { return m_program_counter.load(); }
    uint64_t executed_cycles() const { return m_executed_cycles.load(); }
    uint64_t midi_bytes() const { return m_midi_bytes.load(); }
    uint64_t scsp_writes() const { return m_scsp_writes.load(); }
    int active_voices() const { return m_active_voice_count.load(); }
    int peak_sample() const { return m_peak_sample.load(); }

    uint8_t read8(uint32_t address) override;
    uint16_t read16(uint32_t address) override;
    uint32_t read32(uint32_t address) override;
    void write8(uint32_t address, uint8_t value) override;
    void write16(uint32_t address, uint16_t value) override;
    void write32(uint32_t address, uint32_t value) override;

private:
    friend class model2_audio_test_peer;
    static constexpr int OUTPUT_RATE = 48000;
    static constexpr int NATIVE_RATE = 44100;
    static constexpr int MAX_BUFFER_FRAMES = 836;
    static constexpr int OPENAL_BUFFER_COUNT = 4;
    static constexpr uint32_t MIDI_CAPACITY = 1024;
    static constexpr std::size_t MAX_BUFFER_SAMPLES =
        static_cast<std::size_t>(MAX_BUFFER_FRAMES) * 2;

    enum class envelope_stage : uint8_t { attack, decay1, decay2, release };
    struct voice_state {
        bool active{};
        bool backwards{};
        uint64_t phase{}; // 32.32 sample position.
        uint64_t step{};
        double envelope{};
        double pitch_lfo_phase{};
        double amplitude_lfo_phase{};
        envelope_stage stage{envelope_stage::release};
        uint32_t noise_lfsr{1};
    };

    void audio_loop();
    int next_frame_samples();
    void fill_buffer(int frames);
    void execute_sound_cpu(int cycles);
    void render_scsp(int16_t* output, int frames);
    void render_scsp_wide(int32_t* output, int frames, int ui_mix);
    void tick_timers(int native_samples);
    void queue_midi_out(uint8_t value);
    void tick_midi_out(int native_samples);
    void update_irq();
    void update_slot(unsigned slot, unsigned byte_offset);
    void update_global(unsigned byte_offset);
    void execute_scsp_dma();
    void update_slot_step(unsigned slot);
    void start_slot(unsigned slot);
    void stop_slot(unsigned slot, bool release);
    uint16_t monitor_status() const;
    uint16_t scsp_word(unsigned offset) const;
    void set_scsp_word(unsigned offset, uint16_t value);
    uint8_t pop_midi();
    void transport_midi();
    uint8_t sample_rom_read(uint32_t address) const;

    std::vector<uint8_t> m_program_rom;
    std::vector<uint8_t> m_sample_rom;
    std::vector<uint8_t> m_sound_ram;
    std::array<uint8_t, 0x1000> m_scsp_registers{};
    std::array<voice_state, 32> m_voices{};
    // Modulation ring values reach twice the 16-bit sample range after the
    // hardware's ring-write gain; keep the headroom instead of clamping.
    std::array<int32_t, 64> m_fm_ring{};
    unsigned m_fm_ring_position{};
    model2_scsp_dsp m_dsp;
    std::array<uint32_t, 3> m_timer_counter{};
    std::array<uint32_t, 3> m_timer_prescale{{1, 1, 1}};
    std::array<uint32_t, 3> m_timer_fraction{};
    double m_native_sample_fraction{};
    uint16_t m_sample_bank_control{0x20};
    uint8_t m_monitor_slot{};
    uint32_t m_dma_memory_address{};
    uint16_t m_dma_register_address{};
    uint16_t m_dma_length{};
    bool m_dma_to_ram{};
    bool m_dma_gate{};

    std::array<uint8_t, MIDI_CAPACITY> m_midi_transport{};
    std::atomic<uint32_t> m_midi_write{};
    std::atomic<uint32_t> m_midi_read{};
    std::array<uint8_t, 32> m_midi_fifo{};
    uint8_t m_midi_fifo_read{};
    uint8_t m_midi_fifo_write{};
    uint8_t m_midi_fifo_count{};
    std::array<uint8_t, 32> m_midi_out_fifo{};
    uint8_t m_midi_out_read{};
    uint8_t m_midi_out_write{};
    uint8_t m_midi_out_count{};
    double m_midi_out_fraction{};

    std::function<void(uint8_t)> m_midi_out_callback;
    void* m_device{};
    void* m_context{};
    uint32_t m_source{};
    std::array<uint32_t, OPENAL_BUFFER_COUNT> m_buffers{};
    std::array<int32_t, MAX_BUFFER_SAMPLES> m_mix_output{};
    std::array<int16_t, MAX_BUFFER_SAMPLES> m_output{};
    arcade_audio_output::loudness_normalizer m_output_normalizer;
    std::FILE* m_wave_capture{};
    uint32_t m_wave_capture_bytes{};

    std::atomic<bool> m_initialized{};
    std::atomic<bool> m_running{};
    std::atomic<bool> m_paused{};
    std::thread m_thread;
    uint64_t m_output_frame_fraction{};
    std::atomic<int> m_master_volume{100};
    std::atomic<int> m_music_volume{100};
    std::atomic<int> m_effects_volume{100};
    std::atomic<uint32_t> m_program_counter{};
    std::atomic<uint64_t> m_executed_cycles{};
    std::atomic<uint64_t> m_midi_bytes{};
    std::atomic<uint64_t> m_scsp_writes{};
    std::atomic<int> m_active_voice_count{};
    std::atomic<int> m_peak_sample{};
    uint64_t m_rendered_buffers{};
};
