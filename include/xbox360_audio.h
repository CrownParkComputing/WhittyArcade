#pragma once

#include "arcade_audio_output.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class xbox360_audio_output {
public:
    ~xbox360_audio_output();

    bool initialize();
    void shutdown();
    void enqueue(const std::int16_t* stereo, std::size_t frames);
    void set_master_volume(int value) { m_master_volume.store(value); }
    void set_paused(bool value) { m_paused.store(value); }

private:
    static constexpr int sample_rate = 48000;
    static constexpr int buffer_count = 6;
    static constexpr std::size_t target_frames = 1024;

    void thread_main();
    void fill_buffer(std::uint32_t buffer);

    void* m_device{};
    void* m_context{};
    std::uint32_t m_source{};
    std::array<std::uint32_t, buffer_count> m_buffers{};
    std::mutex m_mutex;
    std::deque<std::int16_t> m_samples;
    std::vector<std::int16_t> m_output;
    arcade_audio_output::loudness_normalizer m_normalizer;
    std::atomic<bool> m_running{};
    std::atomic<bool> m_paused{};
    std::atomic<int> m_master_volume{100};
    std::thread m_thread;
};
