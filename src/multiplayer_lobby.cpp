#include "multiplayer_lobby.h"

#include "arcade_catalog.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <type_traits>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
constexpr uint16_t lobby_port = 35109;
constexpr std::array<uint8_t, 4> lobby_magic{{'W', 'A', 'L', '1'}};
constexpr auto peer_timeout = std::chrono::milliseconds(1500);

struct lobby_packet {
    std::array<uint8_t, 4> magic{};
    uint8_t version{};
    uint8_t node{};
    uint16_t reserved{};
    uint64_t nonce{};
    uint64_t games{};
    uint32_t launch_sequence{};
    char game[32]{};
};
static_assert(std::is_trivially_copyable_v<lobby_packet>);

#if defined(_WIN32)
using native_socket = SOCKET;
constexpr native_socket invalid_socket = INVALID_SOCKET;
void close_socket(native_socket socket) { closesocket(socket); }
#else
using native_socket = int;
constexpr native_socket invalid_socket = -1;
void close_socket(native_socket socket) { ::close(socket); }
#endif

bool set_nonblocking(native_socket socket) {
#if defined(_WIN32)
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 &&
           fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

uint16_t endpoint_port(const sockaddr_in& endpoint) {
    return ntohs(endpoint.sin_port);
}

void send_packet(native_socket socket, const lobby_packet& packet,
                 const sockaddr_in& endpoint) {
    sendto(socket, reinterpret_cast<const char*>(&packet),
           static_cast<int>(sizeof(packet)), 0,
           reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));
}

uint64_t random_nonce() {
    const uint64_t ticks = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::random_device random;
    uint64_t value = ticks ^
        (static_cast<uint64_t>(random()) << 32) ^ random();
    return value ? value : 1;
}

int game_index(std::string_view short_name) {
    const auto& games = supported_rom_sets();
    for (std::size_t index = 0; index < games.size(); ++index)
        if (short_name == games[index].short_name)
            return static_cast<int>(index);
    return -1;
}
} // namespace

multiplayer_lobby::multiplayer_lobby() : m_nonce(random_nonce()) {
#if defined(_WIN32)
    static const bool winsock_ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!winsock_ready) return;
#endif
    m_thread = std::thread(&multiplayer_lobby::run, this);
}

multiplayer_lobby::~multiplayer_lobby() {
    m_stop.store(true, std::memory_order_release);
    if (m_thread.joinable()) m_thread.join();
}

void multiplayer_lobby::set_installed_games(
        const std::vector<rom_choice>& games) {
    uint64_t mask = 0;
    for (const rom_choice& game : games) {
        const auto identity = identify_arcade_game(game.path);
        if (!identity) continue;
        const int index = game_index(identity->short_name);
        const rom_set_manifest* manifest =
            find_supported_rom_set(identity->short_name);
        if (index >= 0 && index < 64 && manifest &&
            manifest->working &&
            manifest->multiplayer != arcade_multiplayer_mode::none)
            mask |= uint64_t{1} << index;
    }
    m_local_games.store(mask, std::memory_order_release);
}

bool multiplayer_lobby::connected() const {
    return m_connected.load(std::memory_order_acquire);
}

int multiplayer_lobby::node() const {
    return m_node.load(std::memory_order_acquire);
}

bool multiplayer_lobby::peer_has_game(std::string_view short_name) const {
    const int index = game_index(short_name);
    return index >= 0 && index < 64 &&
        (m_peer_games.load(std::memory_order_acquire) &
         (uint64_t{1} << index)) != 0;
}

std::string multiplayer_lobby::status_text() const {
    if (!connected())
        return "Searching automatically for another WhittyArcade...";
    return node() == 1 ?
        "Player 2 connected. This cabinet chooses the game." :
        "Connected as Player 2. Player 1 chooses the game.";
}

void multiplayer_lobby::launch_game(std::string_view short_name) {
    if (node() != 1 || short_name.empty()) return;
    {
        std::lock_guard<std::mutex> lock(m_launch_mutex);
        m_outgoing_game.assign(short_name.substr(0, 31));
    }
    m_launch_sequence.fetch_add(1, std::memory_order_acq_rel);
}

bool multiplayer_lobby::launch_pending() const {
    std::lock_guard<std::mutex> lock(m_launch_mutex);
    return !m_incoming_game.empty();
}

std::optional<std::string> multiplayer_lobby::take_launch() {
    std::lock_guard<std::mutex> lock(m_launch_mutex);
    if (m_incoming_game.empty()) return std::nullopt;
    std::string result = std::move(m_incoming_game);
    m_incoming_game.clear();
    return result;
}

void multiplayer_lobby::run() {
    const native_socket socket =
        ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == invalid_socket) return;
    int enabled = 1;
    setsockopt(socket, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    if (!set_nonblocking(socket)) {
        close_socket(socket);
        return;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(lobby_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    bool owns_lobby_port =
        ::bind(socket, reinterpret_cast<const sockaddr*>(&local),
               sizeof(local)) == 0;
    if (!owns_lobby_port) {
        local.sin_port = 0;
        if (::bind(socket, reinterpret_cast<const sockaddr*>(&local),
                   sizeof(local)) != 0) {
            close_socket(socket);
            return;
        }
    }

    sockaddr_in broadcast{};
    broadcast.sin_family = AF_INET;
    broadcast.sin_port = htons(lobby_port);
    broadcast.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sockaddr_in loopback = broadcast;
    loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sockaddr_in peer{};
    bool have_peer = false;
    auto last_seen = std::chrono::steady_clock::time_point{};
    auto next_hello = std::chrono::steady_clock::now();

    std::printf("Multiplayer lobby discovery on UDP %u (%s)\n", lobby_port,
                owns_lobby_port ? "host candidate" : "join candidate");
    while (!m_stop.load(std::memory_order_acquire)) {
        for (;;) {
            lobby_packet incoming{};
            sockaddr_in sender{};
#if defined(_WIN32)
            int sender_size = sizeof(sender);
#else
            socklen_t sender_size = sizeof(sender);
#endif
            const int received = static_cast<int>(recvfrom(
                socket, reinterpret_cast<char*>(&incoming),
                static_cast<int>(sizeof(incoming)), 0,
                reinterpret_cast<sockaddr*>(&sender), &sender_size));
            if (received != static_cast<int>(sizeof(incoming))) break;
            if (incoming.magic != lobby_magic || incoming.version != 1 ||
                incoming.nonce == 0 || incoming.nonce == m_nonce)
                continue;

            peer = sender;
            have_peer = true;
            last_seen = std::chrono::steady_clock::now();
            m_peer_games.store(incoming.games, std::memory_order_release);
            int role = 0;
            if (owns_lobby_port && endpoint_port(sender) != lobby_port)
                role = 1;
            else if (!owns_lobby_port && endpoint_port(sender) == lobby_port)
                role = 2;
            else
                role = m_nonce < incoming.nonce ? 1 : 2;
            m_node.store(role, std::memory_order_release);
            m_connected.store(true, std::memory_order_release);

            if (role == 2 && incoming.node == 1 &&
                incoming.launch_sequence != 0 &&
                incoming.launch_sequence != m_received_launch_sequence &&
                incoming.game[0] != '\0') {
                m_received_launch_sequence = incoming.launch_sequence;
                std::lock_guard<std::mutex> lock(m_launch_mutex);
                m_incoming_game.assign(
                    incoming.game,
                    strnlen(incoming.game, sizeof(incoming.game)));
            }

            // A join candidate sends from an ephemeral port. Reply directly
            // so two instances on one PC work exactly like two LAN machines.
            next_hello = std::chrono::steady_clock::time_point{};
        }

        const auto now = std::chrono::steady_clock::now();
        if (have_peer && now - last_seen > peer_timeout) {
            have_peer = false;
            m_connected.store(false, std::memory_order_release);
            m_node.store(0, std::memory_order_release);
            m_peer_games.store(0, std::memory_order_release);
        }
        if (next_hello.time_since_epoch().count() == 0 ||
            now >= next_hello) {
            lobby_packet outgoing{};
            outgoing.magic = lobby_magic;
            outgoing.version = 1;
            outgoing.node = static_cast<uint8_t>(
                std::clamp(m_node.load(std::memory_order_acquire), 0, 2));
            outgoing.nonce = m_nonce;
            outgoing.games = m_local_games.load(std::memory_order_acquire);
            outgoing.launch_sequence =
                m_launch_sequence.load(std::memory_order_acquire);
            {
                std::lock_guard<std::mutex> lock(m_launch_mutex);
                std::memcpy(outgoing.game, m_outgoing_game.c_str(),
                            std::min(m_outgoing_game.size(),
                                     sizeof(outgoing.game) - 1));
            }
            send_packet(socket, outgoing, broadcast);
            send_packet(socket, outgoing, loopback);
            if (have_peer) send_packet(socket, outgoing, peer);
            next_hello = now + std::chrono::milliseconds(200);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    close_socket(socket);
}
