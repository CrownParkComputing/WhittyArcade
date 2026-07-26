// arcade_input.cpp - shared, configurable SDL arcade input implementation.
#include "arcade_input.h"
#include "arcade_sdl_guard.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <type_traits>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
constexpr float axis_deadzone = 0.15f;
constexpr float cabinet_press_threshold = 0.5f;
constexpr std::size_t sdl_guid_text_size = 33;
constexpr std::array<uint8_t, 4> input_link_magic{{'W', 'A', 'I', '2'}};

std::atomic_bool network_peer_seen{false};
std::atomic_uint32_t network_peer_ipv4{0};
std::atomic_uint8_t network_authoritative_player{0};

struct network_input_packet {
    std::array<uint8_t, 4> magic{};
    uint8_t version{};
    uint8_t node{};
    uint16_t state_size{};
    uint32_t session{};
    uint32_t sequence{};
    uint8_t active_player{};
    // Netplay: the frame this input belongs to. Streaming play leaves it
    // zero and keeps the old newest-wins behaviour, so the two modes share
    // one packet without either having to know about the other.
    uint8_t netplay{};
    uint8_t reserved[2]{};
    uint32_t frame{};
    // Netplay divergence check: the sender's board hash, and the frame it
    // describes. Zero when the sender has not published one.
    uint32_t hash_frame{};
    uint64_t state_hash{};
    uint64_t input_hash{};
    input_state state{};
};
static_assert(std::is_trivially_copyable_v<input_state>);

#if defined(_WIN32)
using input_native_socket = SOCKET;
constexpr input_native_socket invalid_input_socket = INVALID_SOCKET;
input_native_socket to_input_socket(std::intptr_t value) {
    return static_cast<input_native_socket>(value);
}
#else
using input_native_socket = int;
constexpr input_native_socket invalid_input_socket = -1;
input_native_socket to_input_socket(std::intptr_t value) {
    return static_cast<input_native_socket>(value);
}
#endif

uint16_t input_link_port(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value) return 0;
    const unsigned long parsed = std::strtoul(value, nullptr, 10);
    return parsed > 1024 && parsed <= 65535 ?
        static_cast<uint16_t>(parsed) : 0;
}

constexpr std::array<input_action, 4> cabinet_actions{{
    input_action::coin1,
    input_action::coin2,
    input_action::start1,
    input_action::start2,
}};

float approach(float current, float target, float amount) {
    if (current < target) return std::min(current + amount, target);
    if (current > target) return std::max(current - amount, target);
    return current;
}

uint8_t cyber_axis(float normalized) {
    constexpr float center = 0x7f;
    constexpr float half_range = 0x38;
    const int value = static_cast<int>(std::lround(
        center + std::clamp(normalized, -1.0f, 1.0f) * half_range));
    return static_cast<uint8_t>(std::clamp(value, 0x47, 0xb7));
}

int watch_code(const input_binding& binding, input_binding_type type) {
    return binding.type == type ? binding.code : -1;
}

bool second_cabinet_process() {
    const char* node = std::getenv("WHITTY_CABINET_NODE");
    return node && std::strcmp(node, "2") == 0;
}

void route_player_two_keyboard_to_cabinet(
        input_binding_table& bindings) {
    if (!second_cabinet_process()) return;
    // A network cabinet is a separate physical machine, so its local user
    // gets the normal P1 keyboard layout; the link maps that contribution to
    // the authoritative game's P2 inputs. Only the second process of a local
    // two-cabinet setup needs the alternate P2 keys.
    const char* network_link = std::getenv("WHITTY_INPUT_LINK");
    if (network_link && *network_link &&
        std::strcmp(network_link, "0") != 0)
        return;
    const auto copy = [&bindings](input_action destination,
                                  input_action source) {
        bindings[input_action_index(destination)] =
            bindings[input_action_index(source)];
    };
    copy(input_action::coin1, input_action::coin2);
    copy(input_action::start1, input_action::start2);
    copy(input_action::p1_left, input_action::p2_left);
    copy(input_action::p1_right, input_action::p2_right);
    copy(input_action::p1_up, input_action::p2_up);
    copy(input_action::p1_down, input_action::p2_down);
    copy(input_action::p1_action1, input_action::p2_action1);
    copy(input_action::p1_action2, input_action::p2_action2);
    copy(input_action::steer_left, input_action::p2_left);
    copy(input_action::steer_right, input_action::p2_right);
    copy(input_action::gas, input_action::p2_up);
    copy(input_action::brake, input_action::p2_down);
    copy(input_action::shift_down, input_action::p2_action1);
    copy(input_action::shift_up, input_action::p2_action2);
}
} // namespace

bool arcade_input_network_peer_seen() {
    return network_peer_seen.load(std::memory_order_acquire);
}

// Netplay state shared with the host loop. The input object lives on the
// emulation thread while the host loop reads these each frame, so they are
// atomics rather than members reached through a pointer.
std::atomic_bool g_netplay_active{false};
std::atomic_bool g_netplay_stalled{false};
std::atomic_int g_netplay_lead{0};
std::atomic_uint32_t g_netplay_desync_frame{0};
// The local board's state hash, kept per frame so a peer's report about an
// older frame can still be compared against what we ran.
constexpr int netplay_hash_ring = 256;
std::array<std::atomic_uint64_t, netplay_hash_ring> g_netplay_local_hashes{};
// The merged board input per frame, compared alongside the picture so a
// divergence names its own cause.
std::array<std::atomic_uint64_t, netplay_hash_ring> g_netplay_input_hashes{};

bool arcade_input_netplay_stalled() {
    return g_netplay_stalled.load(std::memory_order_acquire);
}

bool arcade_input_netplay_active() {
    return g_netplay_active.load(std::memory_order_acquire);
}

int arcade_input_netplay_lead() {
    return g_netplay_lead.load(std::memory_order_acquire);
}

std::atomic_uint32_t g_netplay_frame{0};

uint32_t arcade_input_netplay_frame() {
    return g_netplay_frame.load(std::memory_order_acquire);
}

void arcade_input_netplay_publish_state(uint32_t frame, uint64_t hash) {
    g_netplay_local_hashes[frame % netplay_hash_ring].store(
        hash, std::memory_order_release);
}

uint32_t arcade_input_netplay_desync_frame() {
    return g_netplay_desync_frame.load(std::memory_order_acquire);
}

std::atomic_bool g_netplay_peer_lost{false};

bool arcade_input_netplay_peer_lost() {
    return g_netplay_peer_lost.load(std::memory_order_acquire);
}

// The one input object holding the netplay link. Set when the link opens and
// cleared when it closes, so a stalled host can reach it without every board
// session having to forward the call.
arcade_input* g_netplay_link_owner = nullptr;

void arcade_input_netplay_poll() {
    if (g_netplay_link_owner) g_netplay_link_owner->poll_link();
}

uint32_t arcade_input_network_peer_ipv4() {
    return network_peer_ipv4.load(std::memory_order_acquire);
}

void arcade_input_set_authoritative_player(uint8_t player) {
    network_authoritative_player.store(
        player <= 2 ? player : 0, std::memory_order_release);
}

uint8_t arcade_input_network_authoritative_player() {
    return network_authoritative_player.load(std::memory_order_acquire);
}

arcade_input::~arcade_input() {
    shutdown();
}

bool arcade_input::initialize_network_link() {
    // Netplay is decided by the launch and travels in the environment, the
    // same way the input link's ports do.
    if (const char* mode = std::getenv("WHITTY_NETPLAY"))
        m_netplay = *mode == '1';
    g_netplay_active.store(m_netplay, std::memory_order_release);
    std::fprintf(stderr, "Netplay link: %s (node %s)\n",
                 m_netplay ? "ON" : "off",
                 std::getenv("WHITTY_CABINET_NODE") ?
                     std::getenv("WHITTY_CABINET_NODE") : "?");
    if (m_netplay) {
        // The opening frames need seeding or the session deadlocks: input is
        // published a delay ahead, so the first packet either machine sends
        // describes frame `delay` and nobody ever publishes frames 0..delay-1.
        // Both sides would then wait forever for input that is never coming.
        // Those frames precede any possible player action, so both machines
        // agree they are empty and can start from them.
        m_netplay_frame = 0;
        m_netplay_peer_frame = 0;
        m_netplay_stalled = false;
        m_netplay_local.fill(input_state{});
        m_netplay_peer.fill(input_state{});
        m_netplay_peer_known.fill(false);
        for (int frame = 0; frame < netplay_delay_frames; ++frame)
            m_netplay_peer_known[frame % netplay_ring] = true;
        g_netplay_frame.store(0, std::memory_order_release);
        g_netplay_desync_frame.store(0, std::memory_order_release);
        for (auto& slot : g_netplay_local_hashes)
            slot.store(0, std::memory_order_release);
    }
    shutdown_network_link();
    const char* enabled = std::getenv("WHITTY_INPUT_LINK");
    if (!enabled || !*enabled || std::strcmp(enabled, "0") == 0)
        return true;
    const char* node_text = std::getenv("WHITTY_CABINET_NODE");
    const int node = node_text ? std::atoi(node_text) : 0;
    const uint16_t local_port = input_link_port("WHITTY_INPUT_LOCAL_PORT");
    const uint16_t peer_port = input_link_port("WHITTY_INPUT_PEER_PORT");
    if ((node != 1 && node != 2) || !local_port || !peer_port)
        return false;

#if defined(_WIN32)
    static const bool winsock_ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!winsock_ready) return false;
#endif
    const input_native_socket socket_handle =
        ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == invalid_input_socket) return false;
    int enabled_option = 1;
    setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&enabled_option),
               sizeof(enabled_option));
    setsockopt(socket_handle, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char*>(&enabled_option),
               sizeof(enabled_option));
    // Input is live state, not an event recording. A small receive buffer
    // prevents a stalled frame from accumulating seconds of old commands.
    const int receive_buffer = 32768;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&receive_buffer),
               sizeof(receive_buffer));
#if defined(_WIN32)
    u_long nonblocking = 1;
    if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) != 0) {
        closesocket(socket_handle);
        return false;
    }
#else
    const int flags = fcntl(socket_handle, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(socket_handle, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(socket_handle);
        return false;
    }
#endif
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(local_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(socket_handle, reinterpret_cast<const sockaddr*>(&local),
               sizeof(local)) != 0) {
#if defined(_WIN32)
        closesocket(socket_handle);
#else
        ::close(socket_handle);
#endif
        return false;
    }

    m_network_socket = static_cast<std::intptr_t>(socket_handle);
    // Claimed here, not earlier: initialize_network_link calls
    // shutdown_network_link on its way in, which clears the owner - so an
    // assignment made before that point is thrown away, and a waiting
    // cabinet is then never polled and never announces itself.
    if (m_netplay) g_netplay_link_owner = this;
    m_network_peer_port = peer_port;
    m_network_node = static_cast<uint8_t>(node);
    const uint64_t ticks = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    m_network_session = static_cast<uint32_t>(
        ticks ^ (ticks >> 32) ^ reinterpret_cast<std::uintptr_t>(this));
    if (m_network_session == 0) m_network_session = 1;
    m_network_peer_session = 0;
    m_network_sequence = 0;
    m_network_peer_sequence = 0;
    m_network_peer_age = 0xffff;
    m_network_peer_state = {};
    network_peer_seen.store(false, std::memory_order_release);
    network_peer_ipv4.store(0, std::memory_order_release);
    std::printf("WhittyArcade player %u input link UDP %u -> %u\n",
                m_network_node, local_port, peer_port);
    return true;
}

void arcade_input::shutdown_network_link() {
    if (g_netplay_link_owner == this) g_netplay_link_owner = nullptr;
    if (m_network_socket != -1) {
        const input_native_socket socket_handle =
            to_input_socket(m_network_socket);
#if defined(_WIN32)
        closesocket(socket_handle);
#else
        ::close(socket_handle);
#endif
    }
    m_network_socket = -1;
    m_network_peer_port = 0;
    m_network_node = 0;
    m_network_session = 0;
    m_network_peer_session = 0;
    m_network_peer_age = 0xffff;
    m_network_peer_state = {};
    network_peer_seen.store(false, std::memory_order_release);
    network_peer_ipv4.store(0, std::memory_order_release);
}

// Drains the link until the peer's input for one frame arrives. Bounded, so
// a peer that has genuinely gone away ends the session rather than hanging
// the program; the frame budget is generous next to a 60 Hz frame because a
// brief network hiccup should cost a stutter, not a disconnection.
// Reads every datagram waiting on the link, filing netplay inputs by frame.
// Returns true when at least one packet from the peer was accepted.
bool arcade_input::drain_network_input() {
    if (m_network_socket == -1) return false;
    const input_native_socket socket_handle =
        to_input_socket(m_network_socket);
    bool received_peer = false;
    for (;;) {
        network_input_packet incoming{};
        sockaddr_in sender{};
#if defined(_WIN32)
        int sender_size = sizeof(sender);
#else
        socklen_t sender_size = sizeof(sender);
#endif
        const int received = static_cast<int>(recvfrom(
            socket_handle, reinterpret_cast<char*>(&incoming),
            static_cast<int>(sizeof(incoming)), 0,
            reinterpret_cast<sockaddr*>(&sender), &sender_size));
        if (received != static_cast<int>(sizeof(incoming))) break;
        if (incoming.magic != input_link_magic ||
            incoming.version != 1 ||
            incoming.node == m_network_node ||
            incoming.state_size != sizeof(input_state))
            continue;
        if (incoming.session != m_network_peer_session) {
            m_network_peer_session = incoming.session;
            m_network_peer_sequence = 0;
        }
        // UDP may duplicate and reorder datagrams. Keep only the newest live
        // state; an older button-down packet must never follow a newer
        // button-release and make firing stick.
        if (m_network_peer_sequence != 0 &&
            static_cast<int32_t>(incoming.sequence -
                                 m_network_peer_sequence) <= 0)
            continue;
        if (m_netplay && incoming.netplay) {
            // Filed by frame rather than overwriting one slot: a packet that
            // arrives late still belongs to its own frame, and one that
            // arrives twice simply lands in the same place.
            const uint32_t slot = incoming.frame % netplay_ring;
            m_netplay_peer[slot] = incoming.state;
            m_netplay_peer_known[slot] = true;
            if (incoming.frame > m_netplay_peer_frame)
                m_netplay_peer_frame = incoming.frame;
            // Compare the peer's report with our own run of that frame. A
            // mismatch means the two boards have diverged - the one thing
            // lockstep exists to prevent - so it is reported rather than
            // left to show as two players seeing different games.
            if (incoming.state_hash != 0 &&
                g_netplay_desync_frame.load(std::memory_order_acquire) == 0) {
                const uint64_t ours =
                    g_netplay_local_hashes[incoming.hash_frame %
                                           netplay_hash_ring].load(
                        std::memory_order_acquire);
                // Only frames we have actually run and still hold.
                const bool operator_data = incoming.hash_frame == 0;
                if (ours != 0 &&
                    (operator_data ||
                     (incoming.hash_frame < m_netplay_frame &&
                      m_netplay_frame - incoming.hash_frame <
                          netplay_hash_ring)) &&
                    ours != incoming.state_hash) {
                    const uint64_t our_input =
                        g_netplay_input_hashes[incoming.hash_frame %
                                               netplay_hash_ring].load(
                            std::memory_order_acquire);
                    if (!operator_data) {
                        std::fprintf(stderr,
                                     "Netplay: merged input %s at frame %u "
                                     "(local %016llx peer %016llx) - the "
                                     "%s\n",
                                     our_input == incoming.input_hash ?
                                         "AGREES" : "DIFFERS",
                                     incoming.hash_frame,
                                     static_cast<unsigned long long>(
                                         our_input),
                                     static_cast<unsigned long long>(
                                         incoming.input_hash),
                                     our_input == incoming.input_hash ?
                                         "emulation drifted" :
                                         "link fed the boards differently");
                    }
                    if (operator_data) {
                        std::fprintf(stderr,
                                     "Netplay: the two machines have "
                                     "different operator data for this game. "
                                     "Match them from the EEPROM / NVRAM "
                                     "manager, or the boards will diverge.\n");
                    }
                    g_netplay_desync_frame.store(
                        incoming.hash_frame == 0 ? 1 : incoming.hash_frame,
                        std::memory_order_release);
                    std::fprintf(stderr,
                                 "Netplay desync at frame %u: local %016llx "
                                 "peer %016llx\n",
                                 incoming.hash_frame,
                                 static_cast<unsigned long long>(ours),
                                 static_cast<unsigned long long>(
                                     incoming.state_hash));
                }
            }
        }
        m_network_peer_state = incoming.state;
        if (incoming.node == 1)
            network_authoritative_player.store(
                incoming.active_player <= 2 ?
                    incoming.active_player : 0,
                std::memory_order_release);
        m_network_peer_sequence = incoming.sequence;
        m_network_peer_age = 0;
        network_peer_ipv4.store(sender.sin_addr.s_addr,
                                std::memory_order_release);
        received_peer = true;
    }
    return received_peer;
}

void arcade_input::poll_link() {
    if (!m_netplay || m_network_socket == -1) return;
    // Announce first, then listen: a silent cabinet is invisible to its
    // partner, and two silent cabinets wait for each other for ever.
    send_link_packet(m_state);
    if (drain_network_input()) {
        m_network_peer_age = 0;
        m_netplay_peer_ever_seen = true;
        // Publish it: the host waits on this before running its first frame,
        // and a cabinet that never runs a frame never presents - so its
        // window, created hidden until the first present, never appears at
        // all. Leaving this unset deadlocked both cabinets into invisibility.
        network_peer_seen.store(true, std::memory_order_release);
        return;
    }
    if (m_network_peer_age < 0xfffe) ++m_network_peer_age;
    // Same threshold the merge uses, so a stalled cabinet and a running one
    // agree on when the partner has gone.
    if (m_netplay_peer_ever_seen && m_network_peer_age >= 180) {
        g_netplay_peer_lost.store(true, std::memory_order_release);
        network_peer_seen.store(false, std::memory_order_release);
    }
}

void arcade_input::wait_for_peer_frame(uint32_t frame) {
    const uint32_t slot = frame % netplay_ring;
    // Long enough that no ordinary hiccup reaches it. A timeout here means
    // running a frame the peer has not run, which is precisely the drift
    // that desynced the first sessions - so the bar for giving up is a peer
    // that has genuinely gone, not one that is merely slow.
    constexpr int max_wait_ms = 3000;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(max_wait_ms);
    const auto give_up = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(1000);
    while (!m_netplay_peer_known[slot]) {
        if (drain_network_input()) continue;
        // A partner that has gone quiet for a second is gone: declaring it
        // early is what keeps the surviving cabinet from sitting frozen on
        // its last frame for the full timeout.
        if (m_netplay_peer_ever_seen &&
            std::chrono::steady_clock::now() >= give_up) {
            g_netplay_peer_lost.store(true, std::memory_order_release);
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            // Waiting here is the only place a vanished partner shows up:
            // a stalled cabinet stops running frames, so nothing else polls
            // the link and the loss would otherwise go unnoticed for ever.
            std::fprintf(stderr,
                         "Netplay: no input for frame %u after waiting "
                         "(peer seen before: %s)\n", frame,
                         m_netplay_peer_ever_seen ? "yes" : "no");
            if (m_netplay_peer_ever_seen)
                g_netplay_peer_lost.store(true, std::memory_order_release);
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    }
}

// Publishes one local input packet. Split out because a cabinet waiting
// for its partner must still announce itself: sending only happened while
// running a frame, and neither cabinet runs a frame until it has seen the
// other - so each waited for a packet the other would never send.
void arcade_input::send_link_packet(const input_state& local_state) {
    if (m_network_socket == -1 || m_network_node == 0) return;
    const input_native_socket socket_handle =
        to_input_socket(m_network_socket);
    network_input_packet outgoing{};
    outgoing.magic = input_link_magic;
    outgoing.version = 1;
    outgoing.node = m_network_node;
    outgoing.state_size = static_cast<uint16_t>(sizeof(input_state));
    outgoing.session = m_network_session;
    outgoing.sequence = ++m_network_sequence;
    outgoing.active_player =
        network_authoritative_player.load(std::memory_order_acquire);
    outgoing.netplay = m_netplay ? 1 : 0;
    // Published for a frame in the future: by the time either machine
    // simulates it, the packet has had the whole delay window to arrive.
    const uint32_t publish_frame = m_netplay_frame + netplay_delay_frames;
    outgoing.frame = publish_frame;
    outgoing.state = local_state;
    if (m_netplay) {
        m_netplay_local[publish_frame % netplay_ring] = local_state;
        // Report the most recently completed frame's hash, which the peer
        // compares against its own run of that frame.
        if (m_netplay_frame > 0) {
            const uint32_t reported = m_netplay_frame - 1;
            outgoing.hash_frame = reported;
            outgoing.state_hash =
                g_netplay_local_hashes[reported % netplay_hash_ring].load(
                    std::memory_order_acquire);
            outgoing.input_hash =
                g_netplay_input_hashes[reported % netplay_hash_ring].load(
                    std::memory_order_acquire);
        }
    }

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(m_network_peer_port);
    peer.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(socket_handle, reinterpret_cast<const char*>(&outgoing),
           static_cast<int>(sizeof(outgoing)), 0,
           reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
    // Also loop the LAN packet onto this host so the exact two-computer mode
    // can be tested with two local WhittyArcade instances.
    peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(socket_handle, reinterpret_cast<const char*>(&outgoing),
           static_cast<int>(sizeof(outgoing)), 0,
           reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
}

void arcade_input::exchange_network_input(const input_state& local_state) {
    if (m_network_socket == -1 || m_network_node == 0) return;
    send_link_packet(local_state);
    const bool received_peer = drain_network_input();
    if (!received_peer && m_network_peer_age < 0xfffe)
        ++m_network_peer_age;
    const bool connected = m_network_peer_age < 180;
    if (m_netplay) {
        if (connected) m_netplay_peer_ever_seen = true;
        // Only after a real connection: a cabinet still waiting for its
        // partner has not lost anything yet.
        if (m_netplay_peer_ever_seen && !connected)
            g_netplay_peer_lost.store(true, std::memory_order_release);
    }
    const bool input_fresh = m_network_peer_age < 6;
    network_peer_seen.store(connected, std::memory_order_release);

    // Both emulators see the same logical cabinet: computer one supplies P1,
    // while computer two's normal controls feed the original board's P2 lines.
    if (!connected) {
        if (m_network_node == 2) m_state = input_state{};
        // A lost peer ends the lockstep rather than freezing the board: the
        // session is over, and the host loop reports the disconnection.
        m_netplay_stalled = false;
        return;
    }
    const input_state neutral{};
    // Netplay: both machines simulate the same frame from the same pair of
    // inputs, so the frame being played is the one both sides published a
    // delay ago - not whatever arrived most recently.
    input_state netplay_local{};
    input_state netplay_remote{};
    if (m_netplay) {
        const uint32_t slot = m_netplay_frame % netplay_ring;
        // Lockstep means waiting, not skipping. The caller runs the board
        // immediately after this returns, so returning without the peer's
        // input would advance one machine and not the other - which is
        // exactly how the two boards diverged on the first live test. Wait
        // here, draining the socket, until the input arrives.
        if (!m_netplay_peer_known[slot])
            wait_for_peer_frame(m_netplay_frame);
        m_netplay_stalled = !m_netplay_peer_known[slot];
        g_netplay_stalled.store(m_netplay_stalled,
                                std::memory_order_release);
        g_netplay_lead.store(netplay_lead_frames(),
                             std::memory_order_release);
        if (m_netplay_stalled) {
            // The peer's input for this frame has not arrived. The board
            // must not advance - simulating it now with a guess is exactly
            // the divergence lockstep exists to prevent.
            m_state = m_netplay_local[slot];
            return;
        }
        netplay_local = m_netplay_local[slot];
        netplay_remote = m_netplay_peer[slot];
        // Consumed: the ring slot is reused sixty-four frames from now.
        m_netplay_peer_known[slot] = false;
        ++m_netplay_frame;
        g_netplay_frame.store(m_netplay_frame, std::memory_order_release);
    }
    const input_state& remote = m_netplay ? netplay_remote :
        (input_fresh ? m_network_peer_state : neutral);
    const input_state& effective_local =
        m_netplay ? netplay_local : local_state;
    const input_state& player1 =
        m_network_node == 1 ? effective_local : remote;
    const input_state& player2 =
        m_network_node == 2 ? effective_local : remote;
    input_state combined = player1;
    // Both machines must merge the same pair of inputs into the same board
    // state. Hashing that here separates the two things a divergence can
    // mean: if these agree but the pictures differ, the emulation drifted;
    // if these differ, the link handed the two boards different inputs.
    const auto publish_input_hash = [&](const input_state& state,
                                        uint32_t frame) {
        uint64_t hash = 1469598103934665603ull;
        const auto* bytes = reinterpret_cast<const unsigned char*>(&state);
        for (std::size_t at = 0; at < sizeof(input_state); ++at) {
            hash ^= bytes[at];
            hash *= 1099511628211ull;
        }
        g_netplay_input_hashes[frame % netplay_hash_ring].store(
            hash ? hash : 1, std::memory_order_release);
    };
    combined.coin2 = combined.coin2 || player2.coin1 || player2.coin2;
    combined.p2_start =
        combined.p2_start || player2.start || player2.p2_start;
    // A netplay session has two players by definition, so either cabinet's
    // start button starts the two-player game. Without this a player has to
    // know that the board's second-player start lives on another key - and
    // pressing the obvious one silently begins a single-player game that
    // the other cabinet can only watch.
    if (m_netplay && combined.start) {
        combined.p2_start = true;
        combined.start = false;
    }
    combined.p2_stick_x = player2.left_stick_x;
    combined.p2_stick_y = player2.left_stick_y;
    for (std::size_t button = 0; button < std::size(combined.p2_buttons);
         ++button) {
        combined.p2_buttons[button] =
            combined.p2_buttons[button] || player2.buttons[button] ||
            player2.p2_buttons[button];
    }
    if (m_netplay && m_netplay_frame > 0)
        publish_input_hash(combined, m_netplay_frame - 1);
    m_state = combined;
}

bool arcade_input::watch_cabinet_button_events(void* userdata,
                                               SDL_Event* event) {
    auto* input = static_cast<arcade_input*>(userdata);
    if (!input || !event) return true;

    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat) {
        const int code = static_cast<int>(event->key.scancode);
        for (std::size_t slot = 0; slot < cabinet_actions.size(); ++slot) {
            if (input->m_watch_keyboard_codes[slot].load(
                    std::memory_order_relaxed) == code) {
                input->m_cabinet_events[slot].store(
                    true, std::memory_order_relaxed);
            }
        }
    } else if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
               event->gbutton.which == input->m_controller_instance.load(
                                            std::memory_order_relaxed)) {
        const int code = static_cast<int>(event->gbutton.button);
        for (std::size_t slot = 0; slot < cabinet_actions.size(); ++slot) {
            if (input->m_watch_controller_buttons[slot].load(
                    std::memory_order_relaxed) == code) {
                input->m_cabinet_events[slot].store(
                    true, std::memory_order_relaxed);
            }
        }
    } else if (input->m_time_crisis_mouse &&
               (event->type == SDL_EVENT_MOUSE_MOTION ||
                event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                event->type == SDL_EVENT_MOUSE_BUTTON_UP)) {
        input->m_mouse_activity.store(true, std::memory_order_relaxed);
    }
    return 0;
}

bool arcade_input::is_lightgun_game(std::string_view game_short_name) {
    // The System 246/256 gun games arrive from the PCSX2 collection, where a
    // game's short name is its .acgame folder name rather than a MAME set, so
    // they are listed by the names those folders carry.
    static constexpr std::string_view lightgun_games[] = {
        "vcop", "vcop2",                                       // Model 2
        "timecrisis3", "timecrisis4", "vampirenight", "cobra", // System 246/256
    };
    for (std::string_view game : lightgun_games)
        if (game_short_name == game) return true;
    return false;
}

bool arcade_input::initialize(std::string_view game_short_name) {
    std::lock_guard<std::mutex> sdl_lock(arcade_sdl_mutex());
    if (m_initialized) return true;
    // The persistent video worker owns SDL_INIT_EVENTS and is the only thread
    // that pumps the event queue. This adapter owns controller/joystick
    // references for the lifetime of one emulated board.
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) {
        std::fprintf(stderr, "SDL controller initialization failed: %s\n",
                     SDL_GetError());
        return false;
    }

    m_game_short_name = game_short_name;
    // Time Crisis (System 22) and the Model 2 light-gun games (Virtua Cop and
    // Virtua Cop 2) all drive the crosshair from the mouse; only their ADC
    // calibration differs. The Model 2 guns use the full 0..255 axis.
    m_lightgun_full_range = is_lightgun_game(m_game_short_name);
    // Time Crisis cabinets have a foot pedal; the other gun games do not.
    m_lightgun_pedal = m_game_short_name.rfind("timecrisis", 0) == 0;
    m_time_crisis_mouse = m_game_short_name == "timecris" ||
                          m_lightgun_full_range;
    m_lightgun_mouse_active = false;
    m_mouse_activity.store(false, std::memory_order_relaxed);
    m_mappings = load_input_mappings();
    m_keyboard_bindings = keyboard_bindings_for(m_mappings,
                                                 m_game_short_name);
    route_player_two_keyboard_to_cabinet(m_keyboard_bindings);
    m_controller_bindings = default_controller_bindings();
    for (std::size_t slot = 0; slot < cabinet_actions.size(); ++slot) {
        const input_binding& binding = m_keyboard_bindings[
            input_action_index(cabinet_actions[slot])];
        m_watch_keyboard_codes[slot].store(
            watch_code(binding, input_binding_type::keyboard),
            std::memory_order_relaxed);
        m_watch_controller_buttons[slot].store(-1,
                                               std::memory_order_relaxed);
        m_cabinet_events[slot].store(false, std::memory_order_relaxed);
    }

    SDL_SetGamepadEventsEnabled(true);
    SDL_AddEventWatch(&arcade_input::watch_cabinet_button_events, this);
    m_event_watch_installed = true;
    m_initialized = true;
    scan_for_controller();
    if (!m_controller)
        std::printf("Input: keyboard active; waiting for an SDL controller\n");
    if (m_lightgun_full_range)
        std::printf("Light gun: move to aim, left click fires, shoot off the "
                    "screen to reload%s\n",
                    m_lightgun_pedal ? ", hold Space for the pedal"
                                     : " (right click also reloads)");
    else if (m_time_crisis_mouse)
        std::printf("Time Crisis mouse gun: move to aim, left click fires, "
                    "hold Space to stand, release Space or right click to "
                    "take cover/reload\n");
    if (!initialize_network_link())
        std::fprintf(stderr,
                     "Could not open WhittyArcade's network player link\n");
    return true;
}

void arcade_input::reload_mappings() {
    std::lock_guard<std::mutex> sdl_lock(arcade_sdl_mutex());
    if (!m_initialized) return;

    m_mappings = load_input_mappings();
    m_keyboard_bindings = keyboard_bindings_for(m_mappings,
                                                 m_game_short_name);
    route_player_two_keyboard_to_cabinet(m_keyboard_bindings);
    for (std::size_t slot = 0; slot < cabinet_actions.size(); ++slot) {
        const input_binding& binding = m_keyboard_bindings[
            input_action_index(cabinet_actions[slot])];
        m_watch_keyboard_codes[slot].store(
            watch_code(binding, input_binding_type::keyboard),
            std::memory_order_relaxed);
    }

    m_controller_bindings = default_controller_bindings();
    if (m_controller) {
        SDL_Joystick* joystick = SDL_GetGamepadJoystick(m_controller);
        std::array<char, sdl_guid_text_size> guid_text{};
        SDL_GUIDToString(SDL_GetJoystickGUID(joystick),
                                  guid_text.data(),
                                  static_cast<int>(guid_text.size()));
        m_controller_bindings = controller_bindings_for(
            m_mappings, guid_text.data(), m_game_short_name);
    }
    set_controller_watch_bindings();

    // Menu navigation must never leak a stale cabinet edge into gameplay.
    m_cabinet_pulse_frames.fill(0);
    m_cabinet_pressed.fill(false);
    for (auto& event : m_cabinet_events)
        event.store(false, std::memory_order_relaxed);
    m_keyboard_steering = 0.0f;
    m_state = input_state{};
}

void arcade_input::shutdown() {
    std::lock_guard<std::mutex> sdl_lock(arcade_sdl_mutex());
    shutdown_network_link();
    if (m_event_watch_installed) {
        SDL_RemoveEventWatch(&arcade_input::watch_cabinet_button_events, this);
        m_event_watch_installed = false;
    }
    m_controller_instance.store(-1, std::memory_order_relaxed);
    if (m_controller) {
        SDL_CloseGamepad(m_controller);
        m_controller = nullptr;
    }
    if (m_initialized) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
        m_initialized = false;
    }
    m_time_crisis_mouse = false;
    m_lightgun_mouse_active = false;
    m_mouse_activity.store(false, std::memory_order_relaxed);
}

void arcade_input::set_controller_watch_bindings() {
    for (std::size_t slot = 0; slot < cabinet_actions.size(); ++slot) {
        const input_binding& binding = m_controller_bindings[
            input_action_index(cabinet_actions[slot])];
        m_watch_controller_buttons[slot].store(
            watch_code(binding, input_binding_type::controller_button),
            std::memory_order_relaxed);
    }
}

bool arcade_input::open_controller(SDL_JoystickID joystick_id) {
    if (!SDL_IsGamepad(joystick_id)) return false;
    SDL_Gamepad* controller = SDL_OpenGamepad(joystick_id);
    if (!controller) return false;

    if (m_controller) SDL_CloseGamepad(m_controller);
    m_controller = controller;
    SDL_Joystick* joystick = SDL_GetGamepadJoystick(m_controller);
    m_controller_instance.store(SDL_GetJoystickID(joystick),
                                std::memory_order_relaxed);

    std::array<char, sdl_guid_text_size> guid_text{};
    SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid_text.data(),
                              static_cast<int>(guid_text.size()));
    m_controller_bindings = controller_bindings_for(
        m_mappings, guid_text.data(), m_game_short_name);
    set_controller_watch_bindings();
    for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
        m_axis_centers[static_cast<std::size_t>(axis)] =
            SDL_GetGamepadAxis(
                m_controller, static_cast<SDL_GamepadAxis>(axis));
    }

    std::printf("Input controller: %s [%s]\n",
                SDL_GetGamepadName(m_controller) ?
                    SDL_GetGamepadName(m_controller) : "unknown",
                guid_text.data());
    return true;
}

void arcade_input::scan_for_controller() {
    if (m_controller) return;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    if (!ids) return;
    if (const char* preferred = std::getenv("RRACER_CONTROLLER")) {
        const int index = static_cast<int>(std::strtol(preferred, nullptr, 0));
        if (index >= 0 && index < count) open_controller(ids[index]);
        SDL_free(ids);
        return;
    }
    for (int index = 0; index < count; ++index)
        if (open_controller(ids[index])) {
            SDL_free(ids);
            return;
        }
    SDL_free(ids);
}

float arcade_input::keyboard_value(input_action action,
                                   const bool* keys) const {
    if (!keys) return 0.0f;
    const input_binding& binding =
        m_keyboard_bindings[input_action_index(action)];
    if (binding.type != input_binding_type::keyboard || binding.code < 0 ||
        binding.code >= SDL_SCANCODE_COUNT)
        return 0.0f;
    return keys[binding.code] ? 1.0f : 0.0f;
}

float arcade_input::controller_axis_value(int axis) const {
    if (!m_controller || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT)
        return 0.0f;
    const int center = m_axis_centers[static_cast<std::size_t>(axis)];
    const int raw = SDL_GetGamepadAxis(
        m_controller, static_cast<SDL_GamepadAxis>(axis));
    const int delta = raw - center;
    const int available = delta < 0 ? center + 32768 : 32767 - center;
    if (available <= 0) return 0.0f;
    float normalized = static_cast<float>(delta) /
                       static_cast<float>(available);
    normalized = std::clamp(normalized, -1.0f, 1.0f);
    if (std::fabs(normalized) <= axis_deadzone) return 0.0f;
    return std::copysign((std::fabs(normalized) - axis_deadzone) /
                         (1.0f - axis_deadzone), normalized);
}

float arcade_input::controller_value(input_action action) const {
    if (!m_controller) return 0.0f;
    const auto binding_value = [this](const input_binding& binding) {
      if (binding.type == input_binding_type::controller_button) {
        if (binding.code < 0 || binding.code >= SDL_GAMEPAD_BUTTON_COUNT)
            return 0.0f;
        return SDL_GetGamepadButton(
                   m_controller,
                   static_cast<SDL_GamepadButton>(binding.code)) ?
            1.0f : 0.0f;
      }
      if (binding.type == input_binding_type::controller_axis) {
        const float value = controller_axis_value(binding.code);
        return std::clamp(binding.direction < 0 ? -value : value,
                          0.0f, 1.0f);
      }
      return 0.0f;
    };
    const input_binding& primary =
        m_controller_bindings[input_action_index(action)];
    return std::max(binding_value(primary),
                    binding_value(default_controller_alias(action, primary)));
}

float arcade_input::action_value(input_action action,
                                 const bool* keys) const {
    return std::max(keyboard_value(action, keys), controller_value(action));
}

void arcade_input::update() {
    std::lock_guard<std::mutex> sdl_lock(arcade_sdl_mutex());
    if (!m_initialized) return;
    // SDL_PollEvent on the video worker has already pumped host events. Never
    // pump here: doing so concurrently with the renderer corrupted SDL/driver
    // state after several live ROM changes.
    if (m_controller && !SDL_GamepadConnected(m_controller)) {
        std::printf("Input controller disconnected\n");
        m_controller_instance.store(-1, std::memory_order_relaxed);
        SDL_CloseGamepad(m_controller);
        m_controller = nullptr;
        for (auto& code : m_watch_controller_buttons)
            code.store(-1, std::memory_order_relaxed);
    }
    if (!m_controller && (m_update_count % 60) == 0) scan_for_controller();
    ++m_update_count;

    const bool* keys = SDL_GetKeyboardState(nullptr);
    // Cabinet lines are edge-latched briefly. Real cabinets sample dedicated
    // I/O continuously, whereas the adapter transfers host state once per
    // frame; the latch prevents a quick tap being lost between board scans.
    for (std::size_t slot = 0; slot < cabinet_actions.size(); ++slot) {
        const bool pressed = action_value(cabinet_actions[slot], keys) >=
                             cabinet_press_threshold;
        const bool event = m_cabinet_events[slot].exchange(
            false, std::memory_order_relaxed);
        if (!m_suppressed &&
            (event || (pressed && !m_cabinet_pressed[slot]))) {
            m_cabinet_pulse_frames[slot] = slot < 2 ? 2 : 4;
        }
        m_cabinet_pressed[slot] = pressed;
    }

    if (m_suppressed) {
        m_keyboard_steering = 0.0f;
        m_cabinet_pulse_frames.fill(0);
        m_lightgun_mouse_active = false;
        m_mouse_activity.store(false, std::memory_order_relaxed);
        m_state = input_state{};
        return;
    }

    const float key_left = keyboard_value(input_action::steer_left, keys);
    const float key_right = keyboard_value(input_action::steer_right, keys);
    const float keyboard_target = std::clamp(key_right - key_left,
                                             -1.0f, 1.0f);
    m_keyboard_steering = approach(
        m_keyboard_steering, keyboard_target,
        keyboard_target == 0.0f ? 0.12f : 0.08f);
    float steering = std::clamp(
        controller_value(input_action::steer_right) -
            controller_value(input_action::steer_left),
        -1.0f, 1.0f);
    if (key_left > 0.0f || key_right > 0.0f)
        steering = m_keyboard_steering;
    const int steering_adc = static_cast<int>(std::lround(
        0x800 + steering * 0x580));
    m_state.steering = static_cast<uint16_t>(
        std::clamp(steering_adc, 0x280, 0xd80));

    const float gas = action_value(input_action::gas, keys);
    const float brake = action_value(input_action::brake, keys);
    m_state.gas = static_cast<uint16_t>(std::lround(gas * 0x610));
    m_state.brake = static_cast<uint16_t>(std::lround(brake * 0x610));

    // Cyber Commando and 2D boards consume the same logical P1 directions.
    const float left_x = std::clamp(
        action_value(input_action::p1_right, keys) -
            action_value(input_action::p1_left, keys), -1.0f, 1.0f);
    const float left_y = std::clamp(
        action_value(input_action::p1_down, keys) -
            action_value(input_action::p1_up, keys), -1.0f, 1.0f);
    const float right_x = std::clamp(
        action_value(input_action::right_right, keys) -
            action_value(input_action::right_left, keys), -1.0f, 1.0f);
    const float right_y = std::clamp(
        action_value(input_action::right_down, keys) -
            action_value(input_action::right_up, keys), -1.0f, 1.0f);
    const float p2_x = std::clamp(
        action_value(input_action::p2_right, keys) -
            action_value(input_action::p2_left, keys), -1.0f, 1.0f);
    const float p2_y = std::clamp(
        action_value(input_action::p2_down, keys) -
            action_value(input_action::p2_up, keys), -1.0f, 1.0f);
    m_state.left_stick_x = cyber_axis(left_x);
    m_state.left_stick_y = cyber_axis(left_y);
    m_state.right_stick_x = cyber_axis(right_x);
    m_state.right_stick_y = cyber_axis(right_y);
    m_state.p2_stick_x = cyber_axis(p2_x);
    m_state.p2_stick_y = cyber_axis(p2_y);

    // Coin/start lines are momentary switches rather than held gameplay
    // buttons. Only the edge-latched pulse reaches the emulated I/O board.
    m_state.coin1 = m_cabinet_pulse_frames[0] != 0;
    m_state.coin2 = m_cabinet_pulse_frames[1] != 0;
    m_state.start = m_cabinet_pulse_frames[2] != 0;
    m_state.p2_start = m_cabinet_pulse_frames[3] != 0;
    m_state.service = action_value(input_action::service, keys) >=
                      cabinet_press_threshold;
                      
    const bool is_test_pressed = action_value(input_action::test, keys) >= cabinet_press_threshold;
    if (is_test_pressed && !m_test_key_was_pressed) {
        m_test_toggle_active = !m_test_toggle_active;
    }
    m_test_key_was_pressed = is_test_pressed;
    
    m_state.test = m_test_input_enabled.load(std::memory_order_acquire) && m_test_toggle_active;

    m_state.shift_down = action_value(input_action::shift_down, keys) >=
                         cabinet_press_threshold;
    m_state.shift_up = action_value(input_action::shift_up, keys) >=
                       cabinet_press_threshold;
    m_state.view = action_value(input_action::view1, keys) >=
                   cabinet_press_threshold;
    m_state.view2 = action_value(input_action::view2, keys) >=
                    cabinet_press_threshold;
    m_state.view3 = action_value(input_action::view3, keys) >=
                    cabinet_press_threshold;
    m_state.view4 = action_value(input_action::view4, keys) >=
                    cabinet_press_threshold;
    const float action1 = action_value(input_action::p1_action1, keys);
    const float action2 = action_value(input_action::p1_action2, keys);
    m_state.buttons[0] = action1 >= cabinet_press_threshold;
    m_state.buttons[1] = action2 >= cabinet_press_threshold;
    m_state.buttons[2] = action_value(input_action::p1_action3, keys) >=
                         cabinet_press_threshold;
    if (m_lightgun_pedal) {
        // A Time Crisis cabinet has a foot pedal, and it must work whether or
        // not the player is currently moving the mouse, so it is mapped here
        // rather than alongside the pointer. The gun's off-screen line is the
        // pointer's business alone, so no key asserts it.
        m_state.buttons[1] = false;
        m_state.buttons[2] = action2 >= cabinet_press_threshold;
    }
    if (m_lightgun_full_range) {
        // A cabinet button for menus, kept clear of the trigger. Driving one
        // from the trigger's own key closed two switches per shot, and the gun
        // calibration reads the second as "exit", so a shot cancelled the
        // screen it was aiming at.
        m_state.buttons[3] = action_value(input_action::p1_action3, keys) >=
                             cabinet_press_threshold;
    }
    m_state.p2_buttons[0] = action_value(input_action::p2_action1, keys) >=
                            cabinet_press_threshold;
    m_state.p2_buttons[1] = action_value(input_action::p2_action2, keys) >=
                            cabinet_press_threshold;

    if (m_time_crisis_mouse) {
        const bool mouse_activity = m_mouse_activity.exchange(
            false, std::memory_order_relaxed);
        // Only a deliberate push on the alternate aim stick takes the gun away
        // from the mouse. Testing for any non-zero deflection at all meant a
        // controller resting slightly off centre stole the aim on every frame
        // the mouse was not moving - and a mouse that is being held steady on
        // a target sends no motion events, so the gun kept snapping back to
        // the stick exactly when the player was trying to hold it still.
        constexpr float alternate_gun_deadzone = 0.35f;
        const bool alternate_gun_activity =
            std::fabs(left_x) > alternate_gun_deadzone ||
            std::fabs(left_y) > alternate_gun_deadzone;
        if (mouse_activity)
            m_lightgun_mouse_active = true;
        else if (alternate_gun_activity)
            m_lightgun_mouse_active = false;

        SDL_Window* mouse_window = SDL_GetMouseFocus();
        if (m_lightgun_mouse_active && mouse_window) {
            float mouse_fx = 0.0f;
            float mouse_fy = 0.0f;
            const uint32_t mouse_buttons = SDL_GetMouseState(
                &mouse_fx, &mouse_fy);
            const int mouse_x = static_cast<int>(mouse_fx);
            const int mouse_y = static_cast<int>(mouse_fy);
            int window_width = 0;
            int window_height = 0;
            SDL_GetWindowSize(mouse_window, &window_width, &window_height);
            if (window_width > 1 && window_height > 1) {
                // Host presentation preserves the board's 4:3 image. Remove
                // any pillarbox/letterbox area before mapping the cursor to
                // the calibrated gun ADC range.
                int content_x = 0;
                int content_y = 0;
                int content_width = window_width;
                int content_height = window_height;
                float pointer_px = static_cast<float>(mouse_x);
                float pointer_py = static_cast<float>(mouse_y);
                if (m_picture_width > 0 && m_picture_height > 0) {
                    // The renderer said exactly where it drew the picture, in
                    // drawable pixels. Those differ from window coordinates
                    // under display scaling, so move the pointer into the same
                    // space before measuring against the rectangle.
                    content_x = m_picture_x;
                    content_y = m_picture_y;
                    content_width = m_picture_width;
                    content_height = m_picture_height;
                    pointer_px *= static_cast<float>(m_picture_drawable_width) /
                                  static_cast<float>(std::max(window_width, 1));
                    pointer_py *= static_cast<float>(m_picture_drawable_height) /
                                  static_cast<float>(std::max(window_height, 1));
                } else if (window_width * 3 > window_height * 4) {
                    content_width = window_height * 4 / 3;
                    content_x = (window_width - content_width) / 2;
                } else if (window_height * 4 > window_width * 3) {
                    content_height = window_width * 3 / 4;
                    content_y = (window_height - content_height) / 2;
                }
                const float pointer_x =
                    (pointer_px - static_cast<float>(content_x)) /
                    static_cast<float>(std::max(content_width - 1, 1));
                const float pointer_y =
                    (pointer_py - static_cast<float>(content_y)) /
                    static_cast<float>(std::max(content_height - 1, 1));
                // A real light gun sees no screen once it is aimed past the
                // edge of the tube, and the board reports that as off-screen.
                // Anything outside the 4:3 image - the pillarbox or letterbox
                // margins the host presentation leaves around it - is off the
                // screen for the same reason.
                const bool pointer_off_screen =
                    pointer_x < 0.0f || pointer_x > 1.0f ||
                    pointer_y < 0.0f || pointer_y > 1.0f;
                const float normalized_x = std::clamp(pointer_x, 0.0f, 1.0f);
                const float normalized_y = std::clamp(pointer_y, 0.0f, 1.0f);
                if (m_lightgun_full_range) {
                    m_state.left_stick_x = static_cast<uint8_t>(
                        std::lround(normalized_x * 255.0f));
                    m_state.left_stick_y = static_cast<uint8_t>(
                        std::lround(normalized_y * 255.0f));
                } else {
                    m_state.left_stick_x =
                        cyber_axis(normalized_x * 2.0f - 1.0f);
                    m_state.left_stick_y =
                        cyber_axis(normalized_y * 2.0f - 1.0f);
                }
                // WHITTY_GUN_TRACE=1 reports where the pointer is, what the
                // picture's rectangle inside the window is, and the fraction
                // of the picture the gun is told to aim at. Comparing that
                // fraction with where the shot lands says whether the aim is
                // being measured wrongly or converted wrongly further down.
                static const bool trace_gun =
                    std::getenv("WHITTY_GUN_TRACE") != nullptr;
                if (trace_gun && (m_update_count % 30) == 0) {
                    std::printf("GUN pointer=%d,%d window=%dx%d "
                                "picture=%d,%d %dx%d aim=%.3f,%.3f "
                                "offscreen=%d mouse_source=%d\n",
                                mouse_x, mouse_y, window_width, window_height,
                                content_x, content_y, content_width,
                                content_height, normalized_x, normalized_y,
                                pointer_off_screen ? 1 : 0,
                                m_lightgun_mouse_active ? 1 : 0);
                    std::fflush(stdout);
                }
                // A cabinet reloads when the trigger is pulled while the gun is
                // off the screen, so the right button has to produce a shot as
                // well as the off-screen line. Asserting off-screen on its own
                // left the gun pointing away with the trigger never pulled,
                // which no game reads as a reload.
                const bool reload_click =
                    m_lightgun_full_range &&
                    (mouse_buttons & SDL_BUTTON_RMASK) != 0;
                m_state.buttons[0] =
                    (mouse_buttons & SDL_BUTTON_LMASK) != 0 ||
                    reload_click ||
                    action1 >= cabinet_press_threshold;
                if (m_lightgun_full_range) {
                    // Reload the way the cabinet does: aim off the screen and
                    // pull the trigger. Button 1 is the gun's off-screen line,
                    // so it follows the pointer, with the right mouse button
                    // as a shortcut for windows with no margin to aim into.
                    m_state.buttons[1] = pointer_off_screen ||
                        (mouse_buttons & SDL_BUTTON_RMASK) != 0 ||
                        (!m_lightgun_pedal &&
                         action2 >= cabinet_press_threshold);
                    // The pedal is mapped with the other cabinet switches
                    // above, so it keeps working while the mouse is still.
                } else {
                    // Time Crisis instead holds a pedal while Richard is
                    // exposed. Releasing it ducks into cover and reloads, so
                    // right mouse reverses the held Space/controller pedal.
                    m_state.buttons[1] =
                        action2 >= cabinet_press_threshold &&
                        (mouse_buttons & SDL_BUTTON_RMASK) == 0;
                }
            }
        }
    }

    exchange_network_input(m_state);
    for (uint8_t& frames : m_cabinet_pulse_frames)
        if (frames != 0) --frames;
}
