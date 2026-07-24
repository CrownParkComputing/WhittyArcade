// system22_c139_transport.h - Namco System 22 C139 cabinet-to-cabinet
// link cable transport over UDP.
//
// The C139 cable is what wires two Ridge Racer 2 / Cyber Commando /
// Time Crisis / etc cabinets together for multiplayer. Each cabinet
// speaks a small fixed-size UDP frame containing a sequence number,
// a "I have a TX frame" flag, and the up-to-4096-word TX FIFO. The
// wire format is:
//
//   offset  size  content
//   ------  ----  ----------------------------------------------
//   0       4     Magic 'W','A','R','2' ("WAR2")
//   4       1     Protocol version (always 1)
//   5       1     Sender node id (1 or 2)
//   6       1     Has-TX-frame flag (1 if the FIFO has data)
//   7       1     Reserved (0)
//   8       4     Sequence counter (little-endian u32)
//   12      4     TX frame word count (little-endian u32, 0..0x1000)
//   16      N*2  TX frame words (big-endian: high byte, low byte)
//
// On the receiving side, the bit-9 sync mark (`0x0100` OR'd into the
// last word) signals "this is the end of the frame"; the original
// code in system22_cpu.cpp's `receive_c139_frame` strips it before
// placing the word in the RX FIFO. The transport does NOT add the
// mark — that happens on the bus. The transport's job is to deliver
// the word stream intact.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class system22_bus;

namespace system22_c139 {

// Wire-format constants.
inline constexpr std::array<std::uint8_t, 4> packet_magic{{'W', 'A', 'R', '2'}};
inline constexpr std::size_t packet_header_bytes = 16;
inline constexpr std::size_t max_frame_words = 0x1000;
inline constexpr std::uint8_t protocol_version = 1;

// One encoded packet (header + frame payload, in bytes).
using packet = std::vector<std::uint8_t>;

// Build the wire-format packet for a frame. `frame_words` may be empty
// (caller has no TX data); the resulting packet is still a valid
// header-only probe so the peer can sync sequence numbers.
packet encode(std::uint8_t node, std::uint32_t sequence,
              const std::vector<std::uint16_t>& frame_words);

// Inverse of encode(). Returns false if `bytes` is shorter than the
// minimum header, has the wrong magic, the wrong protocol version, or
// the node id is not 1 or 2.
//
// On success fills `out_node`, `out_sequence`, `out_frame_words`. The
// RX-side bit-9 sync mark (0x0100 in the last word) is preserved if
// present — the caller is responsible for stripping it before the
// bus's `receive_c139_frame`.
bool decode(const std::uint8_t* bytes, std::size_t size,
            std::uint8_t& out_node, std::uint32_t& out_sequence,
            std::vector<std::uint16_t>& out_frame_words);

} // namespace system22_c139

// Real UDP transport. Two instances talk to each other over loopback
// (or LAN when SYSTEM22_C139_NETWORK is set). Reads env vars on
// initialize() — pass an explicit env_proxy to inject a custom
// environment for tests.
class system22_c139_transport {
public:
    system22_c139_transport();
    ~system22_c139_transport();

    system22_c139_transport(const system22_c139_transport&) = delete;
    system22_c139_transport& operator=(const system22_c139_transport&) = delete;

    // Bind the UDP socket. When `env_proxy` is non-null, all env-var
    // reads go through it (test seam); otherwise real getenv() is used.
    // `forced_node` overrides SYSTEM22_C139_NODE for tests; pass 0 to
    // honour the env var.
    struct env_view {
        const char* (*get)(const char* name) = nullptr; // nullptr = use getenv
    };
    bool initialize(system22_bus& bus, const env_view* env = nullptr,
                    std::uint8_t forced_node = 0);

    bool enabled() const { return m_node != 0; }
    bool linked() const { return m_linked; }
    std::uint8_t node() const { return m_node; }
    std::uint16_t local_port() const { return m_local_port; }
    std::uint16_t peer_port() const { return m_peer_port; }

    // One round-trip. Pulls a frame from the bus (if any), encodes +
    // sends it, drains any incoming datagrams, and feeds the RX FIFO
    // on the bus.
    void exchange(system22_bus& bus);

private:
    int m_socket{-1};
    std::uint8_t m_node{};
    std::uint16_t m_local_port{};
    std::uint16_t m_peer_port{};
    bool m_network{};
    bool m_linked{};
    bool m_payload_sent{};
    bool m_payload_received{};
    std::uint32_t m_sequence{};
    std::uint32_t m_last_peer_sequence{};
    std::uint64_t m_tick{};
    std::uint64_t m_last_peer_tick{};
};

// Initialize Winsock on Windows once. Returns false on fatal Winsock
// startup failure. No-op on POSIX.
bool platform_sockets_initialize();

// Close a socket handle. Wraps closesocket()/::close() in a single
// portable call.
void close_socket(int socket);

