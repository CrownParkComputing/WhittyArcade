#pragma once

#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

struct network_video_frame {
    std::vector<uint8_t> rgba;
    int width{};
    int height{};
    int display_width{};
    int display_height{};
    bool flip_y{};
    bool apply_board_color{};
    uint32_t sequence{};
};

// Board-neutral authoritative video transport. Player 1 sends the final game
// texture; Player 2 presents that texture and contributes only P2 controls.
// Native multi-cabinet games do not enable this path.
class network_video_link {
public:
    network_video_link();
    ~network_video_link();

    network_video_link(const network_video_link&) = delete;
    network_video_link& operator=(const network_video_link&) = delete;

    bool sender() const noexcept { return m_role == 1; }
    bool receiver() const noexcept { return m_role == 2; }
    bool enabled() const noexcept { return m_role != 0; }

    void submit(network_video_frame frame);
    bool take_received(network_video_frame& frame);

private:
    void run();
    void run_sender();
    void run_receiver();

    int m_role{};
    uint16_t m_port{};
    std::atomic_bool m_stop{false};
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_ready;
    network_video_frame m_pending_send;
    network_video_frame m_received;
    bool m_send_pending{};
    bool m_receive_pending{};
};
