#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

class CGSHandler;

struct system246_video_metrics {
    uint64_t produced_frames{};
    uint64_t captured_frames{};
    uint64_t superseded_frames{};
    uint64_t last_readback_microseconds{};
    uint64_t peak_readback_microseconds{};
};

// Play! renders on its own GS thread. This bridge records completion without
// blocking that thread, then performs at most one synchronous readback from
// Whitty's paced host thread.
class system246_video_bridge {
public:
    void notify_frame();
    bool wait_for_frame(std::chrono::milliseconds timeout);
    bool capture_latest(CGSHandler* handler, std::vector<uint8_t>& rgba,
                        int& width, int& height);
    system246_video_metrics metrics() const;

    static CGSHandler* create_handler();

private:
    std::atomic<uint64_t> m_produced_frames{0};
    std::atomic<uint64_t> m_captured_frames{0};
    std::atomic<uint64_t> m_superseded_frames{0};
    std::atomic<uint64_t> m_last_readback_microseconds{0};
    std::atomic<uint64_t> m_peak_readback_microseconds{0};
    std::mutex m_frame_mutex;
    std::condition_variable m_frame_ready;
    uint64_t m_observed_produced_frames{};
    uint64_t m_consumed_sequence{};
};
