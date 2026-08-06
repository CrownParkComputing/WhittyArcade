// lan_peers.h - finding the other MANX on the LAN without any setup.
//
// Two machines on one network have to reach each other before either can be
// told about the other, and a new user should not have to type an IP address
// or open a firewall port to get there. Broadcast alone is not enough: a
// desktop firewall with the usual deny-by-default inbound policy drops an
// arriving broadcast, so both cabinets transmit and both stay silent.
//
// The way through is the connection tracker every one of those firewalls
// already runs. A machine that sends a unicast packet to a peer opens the
// return path for that peer's reply, because the reply is then part of an
// established conversation rather than an unsolicited arrival. When both
// machines sweep the subnet, each one's outbound packet opens the door for
// the other's, and the two meet with no rule added anywhere. Broadcast is
// still sent - it finds a permissive peer on the first tick - but discovery
// no longer depends on it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lan {

// One local IPv4 interface. Addresses are network byte order throughout, so
// they drop straight into sockaddr_in::sin_addr without conversion.
struct interface_v4 {
    uint32_t address{};
    uint32_t netmask{};
    uint32_t broadcast{};
};

// Every up, non-loopback IPv4 interface this machine has. Loopback is left
// out because callers add it themselves when they want two instances on one
// computer to see each other.
std::vector<interface_v4> local_interfaces();

// True for any address belonging to this machine, so a cabinet can ignore
// its own transmissions coming back around.
bool is_local_address(uint32_t address);

// Subnets wider than this are advertised to but never swept: a /16 is 65534
// hosts, which is minutes of traffic for a case that is a virtual bridge
// rather than a LAN with a second computer on it.
constexpr uint32_t max_swept_hosts = 1024;

// A rotating list of destinations to send discovery hellos to. Call next()
// once per tick and send the packet to everything it returns.
class peer_sweep {
public:
    peer_sweep();

    // Re-reads the interface list and starts the sweep over. Worth calling
    // when a link drops, since a machine that changed network is exactly the
    // case where the cached interface list is now wrong.
    void restart();

    // Destinations for this tick: every broadcast address each time, plus
    // the next slice of the unicast sweep. unicast_budget bounds the packets
    // per tick so a large subnet costs bandwidth rather than a burst.
    std::vector<uint32_t> next(std::size_t unicast_budget = 48);

    // True once every sweepable address has been tried at least once. A
    // caller that is still unconnected here can reasonably say so out loud.
    bool swept_once() const { return m_laps > 0; }

private:
    std::vector<interface_v4> m_interfaces;
    std::size_t m_interface{};
    uint32_t m_offset{};
    unsigned m_laps{};
};

} // namespace lan
