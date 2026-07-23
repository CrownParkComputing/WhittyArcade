// multiplayer_lobby.h - automatic launcher-level cabinet discovery.
#pragma once

#include "arcade_types.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class multiplayer_lobby {
public:
    multiplayer_lobby();
    ~multiplayer_lobby();

    multiplayer_lobby(const multiplayer_lobby&) = delete;
    multiplayer_lobby& operator=(const multiplayer_lobby&) = delete;

    void set_installed_games(const std::vector<rom_choice>& games);
    bool connected() const;
    int node() const;
    bool peer_has_game(std::string_view short_name) const;
    std::string status_text() const;

    // Player 1 publishes the selection until Player 2 leaves the launcher.
    void launch_game(std::string_view short_name);
    bool launch_pending() const;
    std::optional<std::string> take_launch();

private:
    void run();

    std::thread m_thread;
    std::atomic_bool m_stop{false};
    std::atomic_bool m_connected{false};
    std::atomic_int m_node{0};
    std::atomic_uint64_t m_local_games{0};
    std::atomic_uint64_t m_peer_games{0};
    std::atomic_uint32_t m_launch_sequence{0};
    uint64_t m_nonce{};

    mutable std::mutex m_launch_mutex;
    std::string m_outgoing_game;
    std::string m_incoming_game;
    uint32_t m_received_launch_sequence{};
};
