// manx_stun.h - the smallest STUN that answers one question: what address
// does the rest of the internet see this socket as?
//
// Deliberately pure. It opens no socket, resolves no name and starts no
// thread: it turns bytes into bytes. That is what lets the lobby ask the
// question on the socket it already owns - and the mapping is only worth
// anything if it belongs to the socket the game traffic will use, so asking
// on a socket of our own would answer the wrong question.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace manx_stun {

// RFC 5389. The cookie is what tells a STUN response apart from everything
// else arriving on a shared port, and it is checked before anything else.
constexpr uint32_t magic_cookie = 0x2112A442u;
constexpr std::size_t header_bytes = 20;

struct transaction_id {
    std::array<uint8_t, 12> bytes{};
    bool operator==(const transaction_id& other) const {
        return bytes == other.bytes;
    }
};

struct mapped_endpoint {
    uint32_t ipv4{};   // network byte order, ready for sockaddr_in
    uint16_t port{};   // host byte order
    bool operator==(const mapped_endpoint& other) const {
        return ipv4 == other.ipv4 && port == other.port;
    }
};

transaction_id make_transaction_id();

// Writes a 20-byte binding request. Returns the bytes written, or 0 when the
// buffer is too small to hold one.
std::size_t build_binding_request(const transaction_id& id, uint8_t* out,
                                  std::size_t capacity);

// Cheap discriminator, run before the lobby's own packet dispatch. A datagram
// is STUN or it is ours, and the cookie says which without either format
// having to know anything about the other. This matters more than it looks:
// a STUN response carrying a couple of attributes can be exactly the size of
// a lobby hello, so length alone would misread one as the other.
bool looks_like_stun(const void* data, std::size_t bytes);

// A binding SUCCESS response whose transaction id is the one we asked with.
// Anything else - an error response, a stale transaction, a truncated
// attribute, an IPv6 address - is nullopt rather than a guess.
std::optional<mapped_endpoint> parse_binding_response(
    const void* data, std::size_t bytes, const transaction_id& expected);

struct server_address {
    std::string host;
    uint16_t port{};
};

// Two operators, so one of them going away is a slower probe rather than no
// probe at all. Overridable with MANX_STUN_SERVERS ("host:port,host:port")
// so a test can point this at something it controls.
std::vector<server_address> default_servers();

} // namespace manx_stun
