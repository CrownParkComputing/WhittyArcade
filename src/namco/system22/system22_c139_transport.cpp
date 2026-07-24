// system22_c139_transport.cpp - C139 cabinet-to-cabinet UDP transport.
//
// Extracted from system22_session.cpp so the wire format (encode +
// decode) can be unit-tested without spinning up two emulator
// processes. The on-the-wire layout matches the format documented in
// system22_c139_transport.h.

#include "namco/system22/system22_c139_transport.h"
#include "namco/system22/system22_cpu.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

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

namespace system22_c139 {

std::uint16_t env_port(const char* name, std::uint16_t fallback,
                       const char* (*proxy_get)(const char*)) {
    const char* value = proxy_get ? proxy_get(name) : std::getenv(name);
    if (!value || !*value) return fallback;
    const unsigned long parsed = std::strtoul(value, nullptr, 10);
    return parsed > 1024 && parsed <= 65535
        ? static_cast<std::uint16_t>(parsed) : fallback;
}

void write_u32_le(std::uint8_t* destination, std::uint32_t value) {
    destination[0] = static_cast<std::uint8_t>(value);
    destination[1] = static_cast<std::uint8_t>(value >> 8);
    destination[2] = static_cast<std::uint8_t>(value >> 16);
    destination[3] = static_cast<std::uint8_t>(value >> 24);
}

std::uint32_t read_u32_le(const std::uint8_t* source) {
    return static_cast<std::uint32_t>(source[0]) |
           (static_cast<std::uint32_t>(source[1]) << 8) |
           (static_cast<std::uint32_t>(source[2]) << 16) |
           (static_cast<std::uint32_t>(source[3]) << 24);
}

packet encode(std::uint8_t node, std::uint32_t sequence,
              const std::vector<std::uint16_t>& frame_words) {
    if (frame_words.size() > max_frame_words) {
        return packet{}; // Caller bug — we cannot represent the frame.
    }
    packet bytes(packet_header_bytes + frame_words.size() * 2, 0);
    bytes[0] = packet_magic[0];
    bytes[1] = packet_magic[1];
    bytes[2] = packet_magic[2];
    bytes[3] = packet_magic[3];
    bytes[4] = protocol_version;
    bytes[5] = node;
    bytes[6] = frame_words.empty() ? std::uint8_t{0} : std::uint8_t{1};
    bytes[7] = 0; // reserved
    write_u32_le(bytes.data() + 8, sequence);
    write_u32_le(bytes.data() + 12,
                 static_cast<std::uint32_t>(frame_words.size()));
    for (std::size_t index = 0; index < frame_words.size(); ++index) {
        const std::uint16_t word = frame_words[index];
        bytes[packet_header_bytes + index * 2] =
            static_cast<std::uint8_t>(word >> 8);
        bytes[packet_header_bytes + index * 2 + 1] =
            static_cast<std::uint8_t>(word & 0xff);
    }
    return bytes;
}

bool decode(const std::uint8_t* bytes, std::size_t size,
            std::uint8_t& out_node, std::uint32_t& out_sequence,
            std::vector<std::uint16_t>& out_frame_words) {
    if (!bytes || size < packet_header_bytes) return false;
    if (bytes[0] != packet_magic[0] || bytes[1] != packet_magic[1] ||
        bytes[2] != packet_magic[2] || bytes[3] != packet_magic[3])
        return false;
    if (bytes[4] != protocol_version) return false;
    if (bytes[5] != 1 && bytes[5] != 2) return false;
    if (size != packet_header_bytes +
                read_u32_le(bytes + 12) * 2)
        return false;
    if (read_u32_le(bytes + 12) > max_frame_words) return false;

    out_node = bytes[5];
    out_sequence = read_u32_le(bytes + 8);
    const std::size_t word_count = read_u32_le(bytes + 12);
    out_frame_words.resize(word_count);
    for (std::size_t index = 0; index < word_count; ++index) {
        out_frame_words[index] = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(
                 bytes[packet_header_bytes + index * 2]) << 8) |
            bytes[packet_header_bytes + index * 2 + 1]);
    }
    return true;
}

} // namespace system22_c139

// ---- system22_c139_transport --------------------------------------

namespace {
constexpr int c139_invalid_socket = -1;
}

bool platform_sockets_initialize() {
#if defined(_WIN32)
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
    return true;
#endif
}

void close_socket(int socket) {
    if (socket < 0) return;
#if defined(_WIN32)
    closesocket(socket);
#else
    ::close(socket);
#endif
}

system22_c139_transport::system22_c139_transport() = default;

system22_c139_transport::~system22_c139_transport() {
    if (m_socket >= 0) close_socket(m_socket);
}

bool system22_c139_transport::initialize(system22_bus& bus,
                                       const env_view* env,
                                       std::uint8_t forced_node) {
    const char* (*proxy_get)(const char*) = env ? env->get : nullptr;
    const char* node_text = proxy_get ? proxy_get("SYSTEM22_C139_NODE")
                                      : std::getenv("SYSTEM22_C139_NODE");
    if (forced_node) {
        m_node = forced_node;
    } else {
        const int node = node_text ? std::atoi(node_text) : 0;
        if (node != 1 && node != 2) {
            bus.set_c139_link(false);
            return true;
        }
        m_node = static_cast<std::uint8_t>(node);
    }
    if (m_node != 1 && m_node != 2) {
        bus.set_c139_link(false);
        return true;
    }
    if (!platform_sockets_initialize()) return false;
    m_network = (proxy_get ? proxy_get("SYSTEM22_C139_NETWORK")
                           : std::getenv("SYSTEM22_C139_NETWORK")) != nullptr;
    const std::uint16_t default_local = m_node == 2 ? 16113 : 16112;
    const std::uint16_t default_peer = m_node == 2 ? 16112 : 16113;
    m_local_port = system22_c139::env_port("SYSTEM22_C139_LOCAL_PORT", default_local,
                                            proxy_get);
    m_peer_port = system22_c139::env_port("SYSTEM22_C139_PEER_PORT", default_peer,
                                           proxy_get);

    m_socket = static_cast<int>(
        ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (m_socket == c139_invalid_socket) return false;
    int reuse = 1;
    setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    if (m_network) {
        setsockopt(m_socket, SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    }
#if defined(_WIN32)
    u_long nonblocking = 1;
    if (ioctlsocket(m_socket, FIONBIO, &nonblocking) != 0) return false;
#else
    const int flags = fcntl(m_socket, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(m_socket, F_SETFL, flags | O_NONBLOCK) < 0)
        return false;
#endif
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(m_local_port);
    local.sin_addr.s_addr = htonl(m_network ? INADDR_ANY : INADDR_LOOPBACK);
    if (::bind(m_socket, reinterpret_cast<const sockaddr*>(&local),
               sizeof(local)) != 0)
        return false;

    bus.set_c139_link(true, m_node);
    std::printf("System 22 C139 cabinet %u: UDP %u -> %u\n",
                m_node, m_local_port, m_peer_port);
    return true;
}

void system22_c139_transport::exchange(system22_bus& bus) {
    if (m_socket < 0 || m_node == 0) return;
    ++m_tick;

    std::vector<std::uint16_t> frame;
    const bool have_frame = bus.take_c139_transmit_frame(frame);
    if (frame.size() > system22_c139::max_frame_words)
        frame.resize(system22_c139::max_frame_words);
    if (have_frame && !m_payload_sent) {
        m_payload_sent = true;
        std::printf("System 22 C139 cabinet %u: transmitting %zu-word RR2 frames\n",
                    m_node, frame.size());
    }
    const system22_c139::packet packet = system22_c139::encode(
        m_node, ++m_sequence, frame);

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(m_peer_port);
    peer.sin_addr.s_addr = htonl(m_network ? INADDR_BROADCAST
                                            : INADDR_LOOPBACK);
    const int packet_size = static_cast<int>(packet.size());
    sendto(m_socket, reinterpret_cast<const char*>(packet.data()),
           packet_size, 0,
           reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
    if (m_network) {
        peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(m_socket, reinterpret_cast<const char*>(packet.data()),
               packet_size, 0,
               reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
    }

    for (;;) {
        std::vector<std::uint8_t> incoming(
            system22_c139::packet_header_bytes +
            system22_c139::max_frame_words * 2);
        sockaddr_in sender{};
#if defined(_WIN32)
        int sender_size = sizeof(sender);
#else
        socklen_t sender_size = sizeof(sender);
#endif
        const int received = static_cast<int>(recvfrom(
            m_socket, reinterpret_cast<char*>(incoming.data()),
            static_cast<int>(incoming.size()), 0,
            reinterpret_cast<sockaddr*>(&sender), &sender_size));
        if (received < static_cast<int>(
                system22_c139::packet_header_bytes)) break;

        std::uint8_t peer_node = 0;
        std::uint32_t peer_sequence = 0;
        std::vector<std::uint16_t> peer_frame;
        if (!system22_c139::decode(incoming.data(),
                                   static_cast<std::size_t>(received),
                                   peer_node, peer_sequence, peer_frame))
            continue;
        if (peer_node == m_node) continue;
        if (m_last_peer_sequence != 0 &&
            static_cast<std::int32_t>(peer_sequence - m_last_peer_sequence) <= 0)
            continue;
        m_last_peer_sequence = peer_sequence;
        m_last_peer_tick = m_tick;
        if (!m_linked) {
            m_linked = true;
            std::printf("System 22 C139 cabinet %u: linked to cabinet %u\n",
                        m_node, peer_node);
        }
        // Has-TX flag = bit 0 of byte 6.
        if ((incoming[6] & 1) == 0 || peer_frame.empty())
            continue;
        bus.receive_c139_frame(peer_frame.data(), peer_frame.size());
        if (!m_payload_received) {
            m_payload_received = true;
            std::printf("System 22 C139 cabinet %u: receiving %zu-word RR2 frames\n",
                        m_node, peer_frame.size());
        }
    }

    if (m_linked && m_tick - m_last_peer_tick > 180) {
        m_linked = false;
        m_last_peer_sequence = 0;
    }
}
