// Offline tests for the STUN codec. No sockets, no network, no libcurl:
// every case here is a buffer of bytes going in and an endpoint or a refusal
// coming out, which is exactly why manx_stun was written as a pure module.

#include "manx_stun.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
}

void push_be16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

void push_be32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>(value & 0xff));
}

manx_stun::transaction_id fixed_transaction() {
    manx_stun::transaction_id id{};
    for (std::size_t index = 0; index < id.bytes.size(); ++index)
        id.bytes[index] = static_cast<uint8_t>(0xa0 + index);
    return id;
}

// One attribute, already padded to a 4-byte boundary the way a real server
// would emit it.
std::vector<uint8_t> attribute(uint16_t type,
                               const std::vector<uint8_t>& value) {
    std::vector<uint8_t> out;
    push_be16(out, type);
    push_be16(out, static_cast<uint16_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
    while (out.size() % 4 != 0) out.push_back(0);
    return out;
}

std::vector<uint8_t> address_value(uint8_t family, uint16_t port,
                                   uint32_t address) {
    std::vector<uint8_t> value;
    value.push_back(0);
    value.push_back(family);
    push_be16(value, port);
    push_be32(value, address);
    return value;
}

std::vector<uint8_t> message(uint16_t type,
                             const manx_stun::transaction_id& id,
                             const std::vector<uint8_t>& attributes) {
    std::vector<uint8_t> out;
    push_be16(out, type);
    push_be16(out, static_cast<uint16_t>(attributes.size()));
    push_be32(out, manx_stun::magic_cookie);
    out.insert(out.end(), id.bytes.begin(), id.bytes.end());
    out.insert(out.end(), attributes.begin(), attributes.end());
    return out;
}

// 203.0.113.7 as it would sit in a sockaddr_in on this machine.
uint32_t expected_ipv4() {
    const uint8_t octets[4] = {203, 0, 113, 7};
    uint32_t packed = 0;
    std::memcpy(&packed, octets, sizeof(packed));
    return packed;
}

constexpr uint16_t binding_success = 0x0101;
constexpr uint16_t binding_error = 0x0111;
constexpr uint32_t dotted_quad = 0xcb007107u;   // 203.0.113.7
constexpr uint16_t mapped_port = 51234;

std::vector<uint8_t> xor_address_attribute(uint16_t type) {
    return attribute(type,
                     address_value(0x01,
                                   static_cast<uint16_t>(
                                       mapped_port ^ (manx_stun::magic_cookie >> 16)),
                                   dotted_quad ^ manx_stun::magic_cookie));
}

void concat(std::vector<uint8_t>& into, const std::vector<uint8_t>& more) {
    into.insert(into.end(), more.begin(), more.end());
}

} // namespace

int main() {
    const manx_stun::transaction_id id = fixed_transaction();

    // --- the request ---------------------------------------------------
    {
        uint8_t buffer[32];
        std::memset(buffer, 0xee, sizeof(buffer));
        const std::size_t written =
            manx_stun::build_binding_request(id, buffer, sizeof(buffer));
        check(written == 20, "a binding request is 20 bytes");
        check(buffer[0] == 0x00 && buffer[1] == 0x01,
              "the request type is Binding Request");
        check(buffer[2] == 0 && buffer[3] == 0,
              "the request carries no attributes");
        check(buffer[4] == 0x21 && buffer[5] == 0x12 && buffer[6] == 0xa4 &&
                  buffer[7] == 0x42,
              "the magic cookie is written big-endian");
        check(std::memcmp(buffer + 8, id.bytes.data(), id.bytes.size()) == 0,
              "the transaction id is copied verbatim");
        check(manx_stun::build_binding_request(id, buffer, 19) == 0,
              "a buffer too short to hold a request writes nothing");
    }

    // --- the happy path ------------------------------------------------
    {
        std::vector<uint8_t> attributes;
        concat(attributes, xor_address_attribute(0x0020));
        const std::vector<uint8_t> wire =
            message(binding_success, id, attributes);
        check(manx_stun::looks_like_stun(wire.data(), wire.size()),
              "a well-formed response is recognised as STUN");
        const auto mapped = manx_stun::parse_binding_response(
            wire.data(), wire.size(), id);
        check(mapped.has_value(), "XOR-MAPPED-ADDRESS parses");
        if (mapped) {
            check(mapped->port == mapped_port, "the port is un-XORed");
            check(mapped->ipv4 == expected_ipv4(),
                  "the address is un-XORed and packed for sockaddr_in");
        }
    }

    // --- the pre-RFC attribute number some servers still emit -----------
    {
        std::vector<uint8_t> attributes;
        concat(attributes, xor_address_attribute(0x8020));
        const std::vector<uint8_t> wire =
            message(binding_success, id, attributes);
        const auto mapped = manx_stun::parse_binding_response(
            wire.data(), wire.size(), id);
        check(mapped.has_value() && mapped->port == mapped_port,
              "the 0x8020 alias is accepted");
    }

    // --- plain MAPPED-ADDRESS, and the preference between the two -------
    {
        std::vector<uint8_t> attributes;
        concat(attributes,
               attribute(0x0001, address_value(0x01, mapped_port, dotted_quad)));
        const std::vector<uint8_t> wire =
            message(binding_success, id, attributes);
        const auto mapped = manx_stun::parse_binding_response(
            wire.data(), wire.size(), id);
        check(mapped.has_value() && mapped->ipv4 == expected_ipv4() &&
                  mapped->port == mapped_port,
              "MAPPED-ADDRESS is used when it is all that is offered");
    }
    {
        // A middlebox that rewrites addresses it recognises is the reason
        // the XOR form exists, so it must win even when it comes second.
        std::vector<uint8_t> attributes;
        concat(attributes,
               attribute(0x0001, address_value(0x01, 1234, 0x0a000001)));
        concat(attributes, xor_address_attribute(0x0020));
        const std::vector<uint8_t> wire =
            message(binding_success, id, attributes);
        const auto mapped = manx_stun::parse_binding_response(
            wire.data(), wire.size(), id);
        check(mapped.has_value() && mapped->port == mapped_port,
              "XOR-MAPPED-ADDRESS is preferred over MAPPED-ADDRESS");
    }

    // --- attributes before the address, with real padding ---------------
    {
        std::vector<uint8_t> attributes;
        const std::string software = "MANX test server";   // 16, no padding
        concat(attributes,
               attribute(0x8022, std::vector<uint8_t>(software.begin(),
                                                      software.end())));
        const std::string odd = "seven!!";                 // 7, pads to 8
        concat(attributes,
               attribute(0x8022,
                         std::vector<uint8_t>(odd.begin(), odd.end())));
        concat(attributes, xor_address_attribute(0x0020));
        concat(attributes, attribute(0x8028, {1, 2, 3, 4}));   // FINGERPRINT
        const std::vector<uint8_t> wire =
            message(binding_success, id, attributes);
        const auto mapped = manx_stun::parse_binding_response(
            wire.data(), wire.size(), id);
        check(mapped.has_value() && mapped->port == mapped_port,
              "the attribute walk steps over padded attributes correctly");
    }

    // --- refusals -------------------------------------------------------
    {
        std::vector<uint8_t> attributes;
        concat(attributes, xor_address_attribute(0x0020));
        const std::vector<uint8_t> wire =
            message(binding_success, id, attributes);

        manx_stun::transaction_id other = id;
        other.bytes[0] ^= 0xff;
        check(!manx_stun::parse_binding_response(wire.data(), wire.size(),
                                                 other),
              "an answer to somebody else's question is refused");

        const std::vector<uint8_t> error =
            message(binding_error, id, attributes);
        check(!manx_stun::parse_binding_response(error.data(), error.size(),
                                                 id),
              "an error response is refused");

        std::vector<uint8_t> corrupt = wire;
        corrupt[4] ^= 0xff;
        check(!manx_stun::looks_like_stun(corrupt.data(), corrupt.size()),
              "a wrong magic cookie is not STUN");
    }
    {
        // An attribute claiming more bytes than the datagram holds. The
        // walk must stop, not read past the end.
        std::vector<uint8_t> attributes;
        push_be16(attributes, 0x0020);
        push_be16(attributes, 64);              // lies
        for (int index = 0; index < 8; ++index) attributes.push_back(0);
        const std::vector<uint8_t> wire =
            message(binding_success, id, attributes);
        check(!manx_stun::parse_binding_response(wire.data(), wire.size(), id),
              "an attribute length past the end of the message is refused");
    }
    {
        // IPv6 is skipped, not misread as four bytes of an IPv4 address.
        std::vector<uint8_t> value;
        value.push_back(0);
        value.push_back(0x02);                  // family IPv6
        push_be16(value, 0x1234);
        for (int index = 0; index < 16; ++index) value.push_back(0x11);
        std::vector<uint8_t> attributes;
        concat(attributes, attribute(0x0020, value));
        const std::vector<uint8_t> wire =
            message(binding_success, id, attributes);
        check(!manx_stun::parse_binding_response(wire.data(), wire.size(), id),
              "an IPv6 mapped address is skipped rather than misparsed");
    }

    // --- the collision that matters ------------------------------------
    // A lobby hello is 88 bytes and a STUN response carrying a few
    // attributes can be 88 bytes too, so the two formats share a port and
    // cannot be told apart by length. Both directions are checked here.
    {
        std::vector<uint8_t> hello(88, 0);
        hello[0] = 'W'; hello[1] = 'A'; hello[2] = 'L'; hello[3] = '1';
        hello[4] = 2;                            // version
        check(!manx_stun::looks_like_stun(hello.data(), hello.size()),
              "a lobby hello is never mistaken for STUN");

        // 20 header + (4 + 52) SOFTWARE + (4 + 8) XOR-MAPPED-ADDRESS = 88.
        std::vector<uint8_t> attributes;
        const std::string filler(52, 'x');
        concat(attributes,
               attribute(0x8022,
                         std::vector<uint8_t>(filler.begin(), filler.end())));
        concat(attributes, xor_address_attribute(0x0020));
        const std::vector<uint8_t> wire =
            message(binding_success, id, attributes);
        check(wire.size() == 88,
              "the collision case really is 88 bytes (it is " +
                  std::to_string(wire.size()) + ")");
        check(manx_stun::looks_like_stun(wire.data(), wire.size()),
              "an 88-byte STUN response is still recognised as STUN");
        const auto mapped = manx_stun::parse_binding_response(
            wire.data(), wire.size(), id);
        check(mapped.has_value() && mapped->port == mapped_port,
              "the 88-byte response still parses");
    }

    // --- transaction ids are not all the same --------------------------
    {
        const auto first = manx_stun::make_transaction_id();
        const auto second = manx_stun::make_transaction_id();
        check(!(first == second), "transaction ids differ between requests");
        bool all_zero = true;
        for (const uint8_t byte : first.bytes)
            if (byte != 0) all_zero = false;
        check(!all_zero, "a transaction id is not all zeroes");
    }

    // --- the server list ------------------------------------------------
    {
        const auto servers = manx_stun::default_servers();
        check(servers.size() >= 2,
              "more than one operator, so one going away is a slower probe");
        for (const auto& server : servers)
            check(!server.host.empty() && server.port != 0,
                  "every default server has a host and a port");
    }

    if (failures == 0) std::printf("manx_stun_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
