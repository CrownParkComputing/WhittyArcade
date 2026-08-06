#include "manx_stun.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>

namespace manx_stun {
namespace {

constexpr uint16_t binding_request = 0x0001;
constexpr uint16_t binding_success = 0x0101;

constexpr uint16_t attribute_mapped_address = 0x0001;
constexpr uint16_t attribute_xor_mapped_address = 0x0020;
// The pre-RFC number for XOR-MAPPED-ADDRESS. Some deployed servers still
// answer with it, and reading it costs one extra case label.
constexpr uint16_t attribute_xor_mapped_address_legacy = 0x8020;

constexpr uint8_t family_ipv4 = 0x01;

uint16_t read_be16(const uint8_t* at) {
    return static_cast<uint16_t>((static_cast<uint16_t>(at[0]) << 8) | at[1]);
}

uint32_t read_be32(const uint8_t* at) {
    return (static_cast<uint32_t>(at[0]) << 24) |
           (static_cast<uint32_t>(at[1]) << 16) |
           (static_cast<uint32_t>(at[2]) << 8) |
           static_cast<uint32_t>(at[3]);
}

void write_be16(uint8_t* at, uint16_t value) {
    at[0] = static_cast<uint8_t>(value >> 8);
    at[1] = static_cast<uint8_t>(value & 0xff);
}

void write_be32(uint8_t* at, uint32_t value) {
    at[0] = static_cast<uint8_t>(value >> 24);
    at[1] = static_cast<uint8_t>((value >> 16) & 0xff);
    at[2] = static_cast<uint8_t>((value >> 8) & 0xff);
    at[3] = static_cast<uint8_t>(value & 0xff);
}

// a.b.c.d as the four bytes in memory order, which is what sockaddr_in wants
// and what the lobby compares against. Built by hand rather than with htonl
// so this file needs no socket header and stays testable anywhere.
uint32_t pack_ipv4(uint32_t dotted_quad) {
    uint8_t octets[4];
    write_be32(octets, dotted_quad);
    uint32_t packed = 0;
    std::memcpy(&packed, octets, sizeof(packed));
    return packed;
}

// One address attribute, XORed or not. Returns nullopt for anything it does
// not fully understand - a short value, an IPv6 family - so that a caller
// can carry on to the next attribute instead of trusting a half-read one.
std::optional<mapped_endpoint> read_address_attribute(
        const uint8_t* value, std::size_t length, bool xored) {
    if (length < 8) return std::nullopt;
    if (value[1] != family_ipv4) return std::nullopt;

    uint16_t port = read_be16(value + 2);
    uint32_t address = read_be32(value + 4);
    if (xored) {
        port = static_cast<uint16_t>(port ^ (magic_cookie >> 16));
        address ^= magic_cookie;
    }
    if (port == 0 || address == 0) return std::nullopt;

    mapped_endpoint endpoint{};
    endpoint.ipv4 = pack_ipv4(address);
    endpoint.port = port;
    return endpoint;
}

} // namespace

transaction_id make_transaction_id() {
    // Mixed the same way the lobby mixes its nonce: a random_device on its
    // own is not guaranteed to be any good on every platform, and the clock
    // costs nothing.
    const uint64_t ticks = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::random_device random;
    transaction_id id{};
    uint64_t mix = ticks ^ (static_cast<uint64_t>(random()) << 32) ^ random();
    for (std::size_t index = 0; index < id.bytes.size(); ++index) {
        if (index % 8 == 0 && index != 0)
            mix = (static_cast<uint64_t>(random()) << 32) ^ random() ^
                  (mix * 0x9e3779b97f4a7c15ull);
        id.bytes[index] = static_cast<uint8_t>(mix >> ((index % 8) * 8));
    }
    return id;
}

std::size_t build_binding_request(const transaction_id& id, uint8_t* out,
                                  std::size_t capacity) {
    if (!out || capacity < header_bytes) return 0;
    write_be16(out, binding_request);
    write_be16(out + 2, 0);            // no attributes
    write_be32(out + 4, magic_cookie);
    std::memcpy(out + 8, id.bytes.data(), id.bytes.size());
    return header_bytes;
}

bool looks_like_stun(const void* data, std::size_t bytes) {
    if (!data || bytes < header_bytes) return false;
    const uint8_t* first = static_cast<const uint8_t*>(data);
    // The top two bits of a STUN message are always zero, the cookie is
    // fixed, and the declared length must account for exactly the rest of
    // the datagram in whole 4-byte units. Three independent checks, because
    // this decides which parser gets a packet on a shared port.
    if ((first[0] & 0xc0) != 0) return false;
    if (read_be32(first + 4) != magic_cookie) return false;
    const uint16_t length = read_be16(first + 2);
    if ((length & 0x3) != 0) return false;
    return static_cast<std::size_t>(length) + header_bytes == bytes;
}

std::optional<mapped_endpoint> parse_binding_response(
        const void* data, std::size_t bytes, const transaction_id& expected) {
    if (!looks_like_stun(data, bytes)) return std::nullopt;
    const uint8_t* first = static_cast<const uint8_t*>(data);
    if (read_be16(first) != binding_success) return std::nullopt;
    // An answer to somebody else's question, or to one of ours we have
    // already given up on. Either way it says nothing about this mapping.
    if (std::memcmp(first + 8, expected.bytes.data(),
                    expected.bytes.size()) != 0)
        return std::nullopt;

    std::optional<mapped_endpoint> fallback;   // MAPPED-ADDRESS, if that is all there is
    std::size_t offset = header_bytes;
    while (offset + 4 <= bytes) {
        const uint16_t type = read_be16(first + offset);
        const std::size_t length = read_be16(first + offset + 2);
        const std::size_t value_at = offset + 4;
        // A length running past the end of the datagram is a malformed or
        // truncated message; stop rather than read whatever follows.
        if (value_at + length > bytes) break;
        const uint8_t* value = first + value_at;

        switch (type) {
        case attribute_xor_mapped_address:
        case attribute_xor_mapped_address_legacy:
            if (auto found = read_address_attribute(value, length, true))
                return found;
            break;
        case attribute_mapped_address:
            // Taken only if no XOR form turns up. Some middleboxes rewrite
            // addresses they can recognise in flight, which is the whole
            // reason the XOR form exists, so it is preferred wherever both
            // are present.
            if (!fallback)
                fallback = read_address_attribute(value, length, false);
            break;
        default:
            break;
        }
        offset = value_at + ((length + 3) & ~std::size_t{3});
    }
    return fallback;
}

std::vector<server_address> default_servers() {
    std::vector<server_address> servers;

    const auto add = [&servers](const std::string& text) {
        const std::size_t colon = text.rfind(':');
        server_address entry;
        if (colon == std::string::npos) {
            entry.host = text;
            entry.port = 3478;
        } else {
            entry.host = text.substr(0, colon);
            entry.port = static_cast<uint16_t>(
                std::strtoul(text.c_str() + colon + 1, nullptr, 10));
        }
        if (!entry.host.empty() && entry.port != 0)
            servers.push_back(std::move(entry));
    };

    if (const char* forced = std::getenv("MANX_STUN_SERVERS")) {
        std::string list(forced);
        std::size_t start = 0;
        while (start <= list.size()) {
            const std::size_t comma = list.find(',', start);
            const std::size_t end =
                comma == std::string::npos ? list.size() : comma;
            if (end > start) add(list.substr(start, end - start));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (!servers.empty()) return servers;
    }

    add("stun.l.google.com:19302");
    add("stun1.l.google.com:19302");
    add("stun.cloudflare.com:3478");
    add("stun.nextcloud.com:443");
    return servers;
}

} // namespace manx_stun
