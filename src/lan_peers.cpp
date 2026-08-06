#include "lan_peers.h"

#include <algorithm>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace lan {
namespace {

uint32_t subnet_hosts(uint32_t netmask_network_order) {
    const uint32_t mask = ntohl(netmask_network_order);
    const uint32_t size = ~mask;
    // A /31 or /32 has no host range to sweep; anything else loses the
    // network and broadcast addresses at the ends.
    return size < 2 ? 0 : size - 1;
}

} // namespace

std::vector<interface_v4> local_interfaces() {
    std::vector<interface_v4> found;
#if defined(_WIN32)
    // SIO_GET_INTERFACE_LIST keeps this to ws2_32, which MANX already links,
    // rather than pulling in iphlpapi for one call.
    const SOCKET query = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (query == INVALID_SOCKET) return found;
    INTERFACE_INFO info[64]{};
    DWORD returned = 0;
    if (WSAIoctl(query, SIO_GET_INTERFACE_LIST, nullptr, 0, info,
                 sizeof(info), &returned, nullptr, nullptr) == 0) {
        const std::size_t count = returned / sizeof(INTERFACE_INFO);
        for (std::size_t index = 0; index < count; ++index) {
            const INTERFACE_INFO& entry = info[index];
            if (!(entry.iiFlags & IFF_UP)) continue;
            if (entry.iiFlags & IFF_LOOPBACK) continue;
            if (entry.iiAddress.Address.sa_family != AF_INET) continue;
            interface_v4 item;
            item.address = entry.iiAddress.AddressIn.sin_addr.s_addr;
            item.netmask = entry.iiNetmask.AddressIn.sin_addr.s_addr;
            item.broadcast =
                (item.address & item.netmask) | ~item.netmask;
            if (item.address && item.netmask) found.push_back(item);
        }
    }
    closesocket(query);
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return found;
    for (const ifaddrs* entry = list; entry; entry = entry->ifa_next) {
        if (!entry->ifa_addr || !entry->ifa_netmask) continue;
        if (entry->ifa_addr->sa_family != AF_INET) continue;
        if (!(entry->ifa_flags & IFF_UP)) continue;
        if (entry->ifa_flags & IFF_LOOPBACK) continue;
        interface_v4 item;
        item.address = reinterpret_cast<const sockaddr_in*>(
            entry->ifa_addr)->sin_addr.s_addr;
        item.netmask = reinterpret_cast<const sockaddr_in*>(
            entry->ifa_netmask)->sin_addr.s_addr;
        item.broadcast = (item.address & item.netmask) | ~item.netmask;
        if (item.address && item.netmask) found.push_back(item);
    }
    freeifaddrs(list);
#endif
    return found;
}

bool is_local_address(uint32_t address) {
    if (address == htonl(INADDR_LOOPBACK)) return true;
    for (const interface_v4& item : local_interfaces())
        if (item.address == address) return true;
    return false;
}

peer_sweep::peer_sweep() { restart(); }

void peer_sweep::restart() {
    m_interfaces = local_interfaces();
    m_interface = 0;
    m_offset = 0;
    m_laps = 0;
}

std::vector<uint32_t> peer_sweep::next(std::size_t unicast_budget) {
    std::vector<uint32_t> targets;
    targets.reserve(unicast_budget + m_interfaces.size() + 1);

    // Broadcast every tick. It costs one packet per interface and is the
    // whole of discovery on a network where nothing is filtering.
    targets.push_back(htonl(INADDR_BROADCAST));
    for (const interface_v4& item : m_interfaces)
        targets.push_back(item.broadcast);

    if (m_interfaces.empty()) return targets;

    // Then the unicast sweep, resumed where the last tick stopped so the
    // cost per tick stays flat however large the subnet is.
    std::size_t sent = 0;
    std::size_t examined = 0;
    while (sent < unicast_budget && examined <= m_interfaces.size()) {
        const interface_v4& item = m_interfaces[m_interface];
        const uint32_t hosts = subnet_hosts(item.netmask);
        if (hosts == 0 || hosts > max_swept_hosts) {
            // Nothing to sweep here - a point-to-point link such as a VPN
            // tunnel, or a bridge subnet far too wide to walk.
            m_interface = (m_interface + 1) % m_interfaces.size();
            m_offset = 0;
            ++examined;
            if (m_interface == 0) ++m_laps;
            continue;
        }
        const uint32_t network = ntohl(item.address) & ntohl(item.netmask);
        while (sent < unicast_budget && m_offset < hosts) {
            const uint32_t host = network + 1 + m_offset;
            ++m_offset;
            const uint32_t candidate = htonl(host);
            if (candidate == item.address) continue;  // ourselves
            targets.push_back(candidate);
            ++sent;
        }
        if (m_offset >= hosts) {
            m_interface = (m_interface + 1) % m_interfaces.size();
            m_offset = 0;
            ++examined;
            if (m_interface == 0) ++m_laps;
        }
    }
    return targets;
}

} // namespace lan
