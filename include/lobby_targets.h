// lobby_targets.h - who the lobby sends its next hello to.
//
// Pulled out of multiplayer_lobby::run() so it can be tested without a
// socket, because this is precisely where a regression would land: the LAN
// discovery that works today and the internet seeding that is being added
// share one send pass, and getting the merge wrong would silently blind a
// machine to the cabinet sitting next to it.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace lobby_link {

// A machine that cannot be found by looking. Sending hellos to its public
// address opens this NAT's return path, so the two 200 ms hello streams the
// lobby already runs *are* the hole punch, and the cadence that keeps the
// lobby alive doubles as the NAT keepalive.
struct remote_peer {
    uint32_t ipv4{};   // network byte order
    uint16_t port{};   // host byte order
    bool operator==(const remote_peer& other) const {
        return ipv4 == other.ipv4 && port == other.port;
    }
};

struct hello_target {
    uint32_t ipv4{};
    uint16_t port{};
    // Only machines that have answered get the console log relay. A seed has
    // no nonce yet, so there is nothing personal to say to it, and a log is
    // not worth pushing down a metered link to a machine that may not even
    // be there.
    bool relay_logs{};
};

// The LAN peer that has gone quiet for this long is gone. Seven missed
// hellos: on a wire, that is a machine that has been switched off.
constexpr auto lan_peer_timeout = std::chrono::milliseconds(1500);
// Thirty missed hellos. The same 1.5 s across the internet would dissolve a
// live game on one bursty-loss second or a phone handing over between masts,
// and a deliberate exit is already covered immediately by the session
// teardown rather than by this timeout.
constexpr auto remote_peer_timeout = std::chrono::milliseconds(6000);

inline std::chrono::milliseconds timeout_for(bool seeded_peer) {
    return seeded_peer ? remote_peer_timeout : lan_peer_timeout;
}

// The subnet sweep runs only while nothing has answered - unchanged from
// before seeding existed. Seeds must not suppress it, or joining an online
// lobby would stop a machine ever finding the one next to it.
inline bool should_sweep(bool any_answering) { return !any_answering; }

// "Searching found nothing" is a claim about this network, and the launcher
// turns it into advice about firewalls. While a punch to somewhere far away
// is still in flight, that advice would be wrong and unhelpful.
inline bool search_is_exhausted(bool swept_once, bool any_seeded) {
    return swept_once && !any_seeded;
}

// Everything answering, plus every seed that is not already answering and is
// not this machine's own public address.
inline std::vector<hello_target> merge_hello_targets(
        const std::vector<hello_target>& answering,
        const std::vector<remote_peer>& seeded,
        uint32_t self_public_ipv4, uint16_t self_public_port) {
    std::vector<hello_target> targets = answering;
    targets.reserve(answering.size() + seeded.size());

    for (const remote_peer& seed : seeded) {
        if (seed.ipv4 == 0 || seed.port == 0) continue;
        // Our own row in the cloud member list. Punching at ourselves would
        // be harmless but it would also be a peer that never answers, which
        // is exactly what "still connecting" looks like to a player.
        if (self_public_ipv4 != 0 && seed.ipv4 == self_public_ipv4 &&
            seed.port == self_public_port)
            continue;
        // Already answering. The seed's whole job was to get the first
        // packet through; once the peer replies it is an ordinary machine
        // known by its nonce, and sending twice would only double the rate.
        const bool known = std::any_of(
            targets.begin(), targets.end(), [&seed](const hello_target& at) {
                return at.ipv4 == seed.ipv4 && at.port == seed.port;
            });
        if (known) continue;
        targets.push_back(hello_target{seed.ipv4, seed.port, false});
    }
    return targets;
}

} // namespace lobby_link
