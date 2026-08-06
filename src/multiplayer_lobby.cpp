#include "multiplayer_lobby.h"

#include "arcade_catalog.h"
#include "lan_peers.h"
#include "manx_log_tap.h"
#include "manx_stun.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <type_traits>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {
constexpr uint16_t lobby_port = 35109;
constexpr std::array<uint8_t, 4> lobby_magic{{'W', 'A', 'L', '1'}};

// Version 2 carries the machine name and the host's roster, which version 1
// had nowhere to put. The version and the length are both checked, so a
// machine running an older build is ignored rather than half-understood.
struct lobby_packet {
    std::array<uint8_t, 4> magic{};
    uint8_t version{};
    uint8_t node{};            // the sender's own player number
    uint8_t invite{};          // multiplayer_invite, the sender's state
    uint8_t assigned_node{};   // what the sender says the RECIPIENT is
    uint64_t nonce{};
    uint64_t games{};
    uint32_t launch_sequence{};
    uint8_t cabinet_count{};   // machines in the session, host authoritative
    uint8_t presence{};        // machine_presence
    uint8_t reserved[2]{};
    char game[32]{};
    char name[24]{};
};
static_assert(std::is_trivially_copyable_v<lobby_packet>);

// The log relay is a second packet type rather than extra fields on the
// hello, so the hello stays small enough to send to every address on a
// subnet several times a second.
constexpr std::array<uint8_t, 4> log_magic{{'W', 'A', 'L', '2'}};
constexpr std::size_t log_payload_bytes = 768;
// What a machine that has just appeared is told about what happened before
// it arrived. Enough to cover a launch; short enough to send in a few ticks.
constexpr uint32_t log_backfill_lines = 60;

struct lobby_log_packet {
    std::array<uint8_t, 4> magic{};
    uint8_t version{};
    uint8_t reserved{};
    uint16_t bytes{};
    uint64_t nonce{};
    uint32_t first_line{};
    uint32_t line_count{};
    char text[log_payload_bytes]{};
};
static_assert(std::is_trivially_copyable_v<lobby_log_packet>);

// In-game traffic. The payload is opaque here: the lobby's job is to get it
// to the other cabinets, not to understand it.
constexpr std::array<uint8_t, 4> session_magic{{'W', 'A', 'L', '3'}};

struct lobby_session_packet {
    std::array<uint8_t, 4> magic{};
    uint8_t version{};
    uint8_t reserved{};
    uint16_t bytes{};
    uint64_t nonce{};
    uint8_t payload[multiplayer_lobby::max_session_payload]{};
};
static_assert(std::is_trivially_copyable_v<lobby_session_packet>);

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

template <typename packet_type>
void send_packet(native_socket socket, const packet_type& packet,
                 const sockaddr_in& endpoint) {
    sendto(socket, reinterpret_cast<const char*>(&packet),
           static_cast<int>(sizeof(packet)), 0,
           reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));
}

// Sleeps until a packet arrives or the timeout expires. A fixed sleep would
// add its own latency to every exchange, which lockstep at 60 Hz cannot
// afford: the whole frame is 16 ms.
void wait_for_traffic(native_socket socket, int milliseconds) {
#if defined(_WIN32)
    WSAPOLLFD entry{};
    entry.fd = socket;
    entry.events = POLLRDNORM;
    WSAPoll(&entry, 1, milliseconds);
#else
    pollfd entry{};
    entry.fd = socket;
    entry.events = POLLIN;
    ::poll(&entry, 1, milliseconds);
#endif
}

std::string address_text(uint32_t ipv4) {
    in_addr value{};
    value.s_addr = ipv4;
    char text[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &value, text, sizeof(text))) return "?";
    return text;
}

std::string this_machine_name() {
    // Overridable so two instances on one computer are tellable apart in
    // the lobby, which is otherwise impossible when they share a hostname.
    if (const char* forced = std::getenv("MANX_MACHINE_NAME"))
        if (*forced) return std::string(forced).substr(0, 23);
    char host[64]{};
#if defined(_WIN32)
    DWORD size = sizeof(host);
    if (GetComputerNameA(host, &size) && host[0]) return host;
#else
    if (gethostname(host, sizeof(host) - 1) == 0 && host[0]) return host;
#endif
    return {};
}

uint64_t random_nonce() {
    const uint64_t ticks = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::random_device random;
    uint64_t value = ticks ^
        (static_cast<uint64_t>(random()) << 32) ^ random();
    return value ? value : 1;
}

// The public-address probe, as a plain object driven by the lobby loop. It
// owns no thread and no lock, and it borrows the lobby's socket rather than
// opening one - which is the entire point. A mapping learned on a different
// socket describes a different hole in the router and is worth nothing to a
// peer trying to reach the one the game will actually use.
struct stun_probe {
    struct server {
        uint32_t ipv4{};
        uint16_t port{};
    };

    std::vector<server> servers;
    std::size_t current{};
    int attempt{};
    bool waiting{};
    manx_stun::transaction_id id{};
    std::chrono::steady_clock::time_point next_send{};

    // The first answer is published immediately, because a player waiting to
    // connect should not also wait for a classification. A second server is
    // then asked the same question: the same port back means one mapping for
    // every destination and punching will work; a different port means a
    // mapping per destination, and no amount of patience will connect this
    // machine to anything.
    bool confirming{};
    manx_stun::mapped_endpoint first{};

    static std::chrono::milliseconds gap(int attempt_number) {
        switch (attempt_number) {
        case 1:  return std::chrono::milliseconds(500);
        case 2:  return std::chrono::milliseconds(1000);
        default: return std::chrono::milliseconds(2000);
        }
    }
    // Four attempts is RFC 5389's shape trimmed to something a launcher can
    // wait through: about four seconds before a server is written off.
    static constexpr int attempts_per_server = 4;
    static constexpr auto cycle_gap = std::chrono::seconds(60);

    void begin(std::chrono::steady_clock::time_point now, std::size_t which) {
        if (servers.empty()) return;
        current = which % servers.size();
        attempt = 0;
        waiting = true;
        id = manx_stun::make_transaction_id();
        next_send = now;
    }

    void tick(native_socket socket,
              std::chrono::steady_clock::time_point now) {
        if (servers.empty() || !waiting || now < next_send) return;
        if (attempt >= attempts_per_server) {
            // This server is not answering. Try the next one, and when they
            // have all been tried, leave the address unknown and come back
            // in a minute rather than hammering.
            const std::size_t next = current + 1;
            if (next >= servers.size() || confirming) {
                waiting = false;
                next_send = now + cycle_gap;
                confirming = false;
                return;
            }
            begin(now, next);
        }
        uint8_t request[manx_stun::header_bytes];
        const std::size_t bytes =
            manx_stun::build_binding_request(id, request, sizeof(request));
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = servers[current].ipv4;
        endpoint.sin_port = htons(servers[current].port);
        sendto(socket, reinterpret_cast<const char*>(request),
               static_cast<int>(bytes), 0,
               reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint));
        ++attempt;
        next_send = now + gap(attempt);
    }

    // True when this datagram was an answer to our question and it changed
    // what we know. `mapping` is only meaningful on the first answer of a
    // cycle; `nat` is only meaningful after the confirming one.
    bool absorb(const void* data, std::size_t bytes,
                std::chrono::steady_clock::time_point now,
                manx_stun::mapped_endpoint& mapping, nat_kind& nat,
                bool& mapping_known) {
        if (!waiting) return false;
        const auto answer =
            manx_stun::parse_binding_response(data, bytes, id);
        if (!answer) return false;

        if (!confirming) {
            first = *answer;
            mapping = *answer;
            mapping_known = true;
            if (servers.size() < 2) {
                // Nothing to compare against. Leave the classification
                // unknown rather than guessing from one sample.
                waiting = false;
                next_send = now + cycle_gap;
                return true;
            }
            confirming = true;
            begin(now, current + 1);
            return true;
        }

        nat = (answer->ipv4 == first.ipv4 && answer->port == first.port)
                  ? nat_kind::cone
                  : nat_kind::symmetric;
        mapping = first;
        mapping_known = true;
        waiting = false;
        confirming = false;
        next_send = now + cycle_gap;
        return true;
    }

    // A cycle that finished, or one that gave up, comes round again so a
    // router that rebinds or a WAN address that changes is noticed.
    void restart_if_due(std::chrono::steady_clock::time_point now) {
        if (waiting || servers.empty() || now < next_send) return;
        confirming = false;
        begin(now, 0);
    }
};

// "1.2.3.4:35109,5.6.7.8:40000" - the escape hatch that makes the whole
// seeding path testable with two processes on one machine and no cloud
// account anywhere.
std::vector<lobby_link::remote_peer> peers_from_environment() {
    std::vector<lobby_link::remote_peer> peers;
    const char* list = std::getenv("MANX_REMOTE_PEER");
    if (!list || !*list) return peers;

    std::string text(list);
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::size_t end = comma == std::string::npos ? text.size() : comma;
        const std::string entry = text.substr(start, end - start);
        const std::size_t colon = entry.rfind(':');
        if (colon != std::string::npos) {
            in_addr address{};
            if (inet_pton(AF_INET, entry.substr(0, colon).c_str(),
                          &address) == 1) {
                lobby_link::remote_peer peer;
                peer.ipv4 = address.s_addr;
                peer.port = static_cast<uint16_t>(
                    std::strtoul(entry.c_str() + colon + 1, nullptr, 10));
                if (peer.port != 0) peers.push_back(peer);
            }
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return peers;
}

// Which bit in the hello's mask stands for this game.
//
// It used to be the game's position in supported_rom_sets(), and plugins are
// appended to that list - so two machines with any difference in what is
// installed numbered the same game differently, each concluded the other did
// not have it, and Network Play sat greyed out saying "not on the others"
// with both cabinets holding the game. The name decides the bit now, so two
// machines agree without having to agree on anything else.
//
// Sixty-four bits and a hash means two names can collide, which would claim
// a machine has a game it does not. That is the failure the launcher already
// handles - it says so when the ROM turns out to be missing - and it is much
// the better way round than refusing a game everybody has.
int game_index(std::string_view short_name) {
    if (short_name.empty()) return -1;
    uint64_t mixed = 1469598103934665603ull;
    for (const char character : short_name) {
        mixed ^= static_cast<unsigned char>(character);
        mixed *= 1099511628211ull;
    }
    return static_cast<int>(mixed % 64);
}
} // namespace

multiplayer_lobby::multiplayer_lobby()
        : m_nonce(random_nonce()), m_local_name(this_machine_name()) {
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
    if (m_resolve_thread.joinable()) m_resolve_thread.join();
}

void multiplayer_lobby::set_remote_peers(
        std::vector<lobby_link::remote_peer> peers) {
    if (peers.size() > static_cast<std::size_t>(lobby_max_machines))
        peers.resize(lobby_max_machines);
    std::lock_guard<std::mutex> lock(m_remote_mutex);
    m_remote_peers = std::move(peers);
}

std::size_t multiplayer_lobby::remote_peer_count() const {
    std::lock_guard<std::mutex> lock(m_remote_mutex);
    return m_remote_peers.size();
}

void multiplayer_lobby::begin_public_endpoint_probe() {
    // Idempotent: the first caller starts the resolver, everybody else has
    // already got what they asked for.
    if (m_probe_requested.exchange(true, std::memory_order_acq_rel)) return;
    m_resolve_thread =
        std::thread(&multiplayer_lobby::resolve_stun_servers, this);
}

std::optional<lobby_link::remote_peer>
multiplayer_lobby::public_endpoint() const {
    const uint64_t packed = m_public_endpoint.load(std::memory_order_acquire);
    if (packed == 0) return std::nullopt;
    lobby_link::remote_peer endpoint;
    endpoint.ipv4 = static_cast<uint32_t>(packed >> 16);
    endpoint.port = static_cast<uint16_t>(packed & 0xffff);
    return endpoint;
}

nat_kind multiplayer_lobby::nat() const {
    return static_cast<nat_kind>(m_nat.load(std::memory_order_acquire));
}

uint64_t multiplayer_lobby::games_mask() const {
    return m_local_games.load(std::memory_order_acquire);
}

void multiplayer_lobby::resolve_stun_servers() {
    std::vector<uint32_t> addresses;
    std::vector<uint16_t> ports;
    for (const manx_stun::server_address& server : manx_stun::default_servers()) {
        if (m_stop.load(std::memory_order_acquire)) return;
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* results = nullptr;
        if (getaddrinfo(server.host.c_str(), nullptr, &hints, &results) != 0)
            continue;
        for (const addrinfo* at = results; at; at = at->ai_next) {
            if (at->ai_family != AF_INET || !at->ai_addr) continue;
            const auto* address =
                reinterpret_cast<const sockaddr_in*>(at->ai_addr);
            addresses.push_back(address->sin_addr.s_addr);
            ports.push_back(server.port);
            break;   // one address per server is plenty
        }
        freeaddrinfo(results);
    }
    {
        std::lock_guard<std::mutex> lock(m_stun_mutex);
        m_stun_ipv4 = std::move(addresses);
        m_stun_port = std::move(ports);
    }
    m_stun_resolved.store(true, std::memory_order_release);
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

std::vector<lobby_machine> multiplayer_lobby::machines() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<lobby_machine> copy = m_machines;
    for (lobby_machine& machine : copy)
        machine.has_log = m_logs.count(machine.nonce) != 0;
    return copy;
}

bool multiplayer_lobby::connected() const {
    return m_connected.load(std::memory_order_acquire);
}

std::string multiplayer_lobby::local_name() const {
    return m_local_name.empty() ? std::string("this machine") : m_local_name;
}

void multiplayer_lobby::set_presence(machine_presence state) {
    m_presence.store(static_cast<uint8_t>(state), std::memory_order_release);
}

machine_presence multiplayer_lobby::presence() const {
    // A machine in a game is busy whatever it was set to; the game is the
    // fact, the setting is only a preference.
    if (m_in_session.load(std::memory_order_acquire))
        return machine_presence::in_game;
    return static_cast<machine_presence>(
        m_presence.load(std::memory_order_acquire));
}

bool multiplayer_lobby::search_exhausted() const {
    return m_search_exhausted.load(std::memory_order_acquire);
}

void multiplayer_lobby::invite(uint64_t nonce) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (std::find(m_invited.begin(), m_invited.end(), nonce) ==
        m_invited.end())
        m_invited.push_back(nonce);
    m_declined.store(false, std::memory_order_release);
    m_hosting.store(true, std::memory_order_release);
    m_local_invite.store(static_cast<uint8_t>(multiplayer_invite::inviting),
                         std::memory_order_release);
}

void multiplayer_lobby::invite_everyone() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_invited.clear();
    for (const lobby_machine& machine : m_machines)
        m_invited.push_back(machine.nonce);
    if (m_invited.empty()) return;
    m_declined.store(false, std::memory_order_release);
    m_hosting.store(true, std::memory_order_release);
    m_local_invite.store(static_cast<uint8_t>(multiplayer_invite::inviting),
                         std::memory_order_release);
}

void multiplayer_lobby::answer_invite(bool allow) {
    m_local_invite.store(
        static_cast<uint8_t>(allow ? multiplayer_invite::accepted
                                   : multiplayer_invite::declined),
        std::memory_order_release);
    m_hosting.store(false, std::memory_order_release);
}

void multiplayer_lobby::cancel_invite() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_invited.clear();
        m_host_nonce = 0;
    }
    m_hosting.store(false, std::memory_order_release);
    m_local_invite.store(static_cast<uint8_t>(multiplayer_invite::idle),
                         std::memory_order_release);
    m_node.store(0, std::memory_order_release);
    m_agreed.store(0, std::memory_order_release);
    m_host_ipv4.store(0, std::memory_order_release);
    m_declined.store(false, std::memory_order_release);
}

bool multiplayer_lobby::invitation_refused() const {
    return m_declined.load(std::memory_order_acquire);
}

void multiplayer_lobby::clear_refusal() {
    m_declined.store(false, std::memory_order_release);
}

bool multiplayer_lobby::hosting() const {
    return m_hosting.load(std::memory_order_acquire);
}

std::optional<lobby_machine> multiplayer_lobby::pending_invitation() const {
    if (static_cast<multiplayer_invite>(
            m_local_invite.load(std::memory_order_acquire)) !=
        multiplayer_invite::idle)
        return std::nullopt;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const lobby_machine& machine : m_machines)
        if (machine.invite == multiplayer_invite::inviting)
            return machine;
    return std::nullopt;
}

int multiplayer_lobby::node() const {
    return m_node.load(std::memory_order_acquire);
}

int multiplayer_lobby::agreed_count() const {
    return m_agreed.load(std::memory_order_acquire);
}

uint32_t multiplayer_lobby::host_ipv4() const {
    return m_host_ipv4.load(std::memory_order_acquire);
}

bool multiplayer_lobby::everyone_has_game(std::string_view short_name,
                                          int places) const {
    const int index = game_index(short_name);
    if (index < 0 || index >= 64) return false;
    const uint64_t bit = uint64_t{1} << index;
    std::lock_guard<std::mutex> lock(m_mutex);
    int counted = 1;   // this machine, which is offering the game
    for (const lobby_machine& machine : m_machines) {
        if (machine.invite != multiplayer_invite::accepted) continue;
        if (counted >= places) break;
        if ((machine.games & bit) == 0) return false;
        ++counted;
    }
    return counted >= 2;
}

void multiplayer_lobby::launch_game(std::string_view short_name,
                                    int places) {
    if (!hosting() || node() != 1 || short_name.empty()) return;
    {
        std::lock_guard<std::mutex> lock(m_launch_mutex);
        m_outgoing_game.assign(short_name.substr(0, 31));
        m_launch_places = std::clamp(places, 2, lobby_max_machines);
    }
    m_launch_sequence.fetch_add(1, std::memory_order_acq_rel);
    m_launched_count.store(agreed_count(), std::memory_order_release);
    m_in_session.store(true, std::memory_order_release);
}

void multiplayer_lobby::leave_session() {
    // Clear the published game first: a machine that comes back to the
    // lobby and is invited again must not be handed the last game as
    // though it were a fresh choice.
    {
        std::lock_guard<std::mutex> lock(m_launch_mutex);
        m_outgoing_game.clear();
        m_incoming_game.clear();
    }
    m_launch_sequence.store(0, std::memory_order_release);
    m_launched_count.store(0, std::memory_order_release);
    m_in_session.store(false, std::memory_order_release);
    cancel_invite();
}

bool multiplayer_lobby::session_ended() const {
    if (!m_in_session.load(std::memory_order_acquire)) return false;
    if (hosting()) {
        // A cabinet has dropped out. The whole session goes with it: a game
        // half of whose machines have quit is not a game anyone is playing.
        const int started = m_launched_count.load(std::memory_order_acquire);
        return started >= 2 && agreed_count() < started;
    }
    // Not the host: the session exists only while the host is still there
    // and still asking for this machine.
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const lobby_machine& machine : m_machines)
        if (machine.nonce == m_host_nonce)
            return machine.invite != multiplayer_invite::inviting;
    return true;
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

void multiplayer_lobby::send_to_session(const void* data,
                                       std::size_t bytes) {
    if (!data || bytes == 0 || bytes > max_session_payload) return;
    const std::intptr_t handle = m_socket.load(std::memory_order_acquire);
    if (handle == -1) return;
    lobby_session_packet packet{};
    packet.magic = session_magic;
    packet.version = 1;
    packet.bytes = static_cast<uint16_t>(bytes);
    packet.nonce = m_nonce;
    std::memcpy(packet.payload, data, bytes);

    // To everyone in the session and nobody else. A machine with a player
    // number is in it - the host sees the cabinets it placed, and each
    // cabinet sees the host - and the addresses are the ones the lobby has
    // been talking to all along.
    std::vector<sockaddr_in> targets;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        targets.reserve(m_machines.size());
        for (const lobby_machine& machine : m_machines) {
            if (machine.node == 0) continue;
            sockaddr_in endpoint{};
            endpoint.sin_family = AF_INET;
            endpoint.sin_port =
                htons(machine.port ? machine.port : lobby_port);
            endpoint.sin_addr.s_addr = machine.ipv4;
            targets.push_back(endpoint);
        }
    }
    for (const sockaddr_in& endpoint : targets)
        send_packet(static_cast<native_socket>(handle), packet, endpoint);
}

void multiplayer_lobby::set_session_receiver(
        std::function<void(const void*, std::size_t)> receiver) {
    std::lock_guard<std::mutex> lock(m_session_mutex);
    m_session_receiver = std::move(receiver);
}

std::vector<std::string> multiplayer_lobby::peer_log(uint64_t nonce) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_logs.find(nonce);
    if (found == m_logs.end()) return {};
    return {found->second.begin(), found->second.end()};
}

bool multiplayer_lobby::has_any_log() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_logs.empty();
}

void multiplayer_lobby::absorb_peer_log(uint64_t nonce, uint32_t first_line,
                                        const char* text,
                                        std::size_t bytes) {
    if (!text || bytes == 0 || nonce == 0) return;
    std::vector<std::string> arrived;
    std::string current;
    for (std::size_t index = 0; index < bytes; ++index) {
        if (text[index] == '\n') {
            arrived.push_back(std::move(current));
            current.clear();
        } else if (text[index] != '\0') {
            current.push_back(text[index]);
        }
    }
    if (!current.empty()) arrived.push_back(std::move(current));
    if (arrived.empty()) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    std::deque<std::string>& log = m_logs[nonce];
    uint32_t& next = m_log_next[nonce];
    const uint32_t last = first_line + static_cast<uint32_t>(arrived.size());
    std::size_t skip = 0;
    if (!log.empty()) {
        // Already have all of this - a resend, or a duplicated datagram.
        if (static_cast<int32_t>(last - next) <= 0) return;
        if (static_cast<int32_t>(first_line - next) > 0) {
            // A slice went missing. Say so where it happened, so nobody
            // reads two unrelated lines as consecutive.
            log.push_back("... " + std::to_string(first_line - next) +
                          " line(s) lost in transit ...");
        } else {
            skip = next - first_line;
        }
    }
    for (std::size_t index = skip; index < arrived.size(); ++index)
        log.push_back(std::move(arrived[index]));
    next = last;
    while (log.size() > peer_log_capacity) log.pop_front();
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

    sockaddr_in loopback{};
    loopback.sin_family = AF_INET;
    loopback.sin_port = htons(lobby_port);
    loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    lan::peer_sweep sweep;
    // Published so the emulation thread can send on it directly. One socket
    // and one port for everything multiplayer.
    m_socket.store(static_cast<std::intptr_t>(socket),
                   std::memory_order_release);

    std::map<uint64_t, std::chrono::steady_clock::time_point> last_seen;
    std::map<uint64_t, sockaddr_in> endpoints;
    std::map<uint64_t, uint32_t> log_cursor;
    // Which machines were reached by seeding rather than by looking. They
    // are given a far longer silence before being written off, because a
    // second of bursty loss is a normal internet event and dissolving a live
    // game over it would be worse than waiting.
    std::set<uint64_t> remote_nonces;
    stun_probe probe;
    auto next_hello = std::chrono::steady_clock::now();

    // Set by hand, this exercises the whole seed - punch - dedupe - retire
    // path with two processes on one machine, no router and no cloud.
    if (const auto forced = peers_from_environment(); !forced.empty()) {
        set_remote_peers(forced);
        std::printf("Lobby seeded with %zu remote peer(s) from "
                    "MANX_REMOTE_PEER\n", forced.size());
    }

    std::printf("Multiplayer lobby discovery on UDP %u (%s) as \"%s\"\n",
                lobby_port,
                owns_lobby_port ? "host candidate" : "join candidate",
                local_name().c_str());

    while (!m_stop.load(std::memory_order_acquire)) {
        for (;;) {
            // One buffer for both packet types; which one arrived is decided
            // by its magic and its length, so a hello and a log relay share
            // the port without either having to know about the other.
            union {
                lobby_packet hello;
                lobby_log_packet log;
                lobby_session_packet session;
            } wire{};
            sockaddr_in sender{};
#if defined(_WIN32)
            int sender_size = sizeof(sender);
#else
            socklen_t sender_size = sizeof(sender);
#endif
            const int received = static_cast<int>(recvfrom(
                socket, reinterpret_cast<char*>(&wire),
                static_cast<int>(sizeof(wire)), 0,
                reinterpret_cast<sockaddr*>(&sender), &sender_size));
            if (received <= 0) break;

            // Before anything length-based. A STUN response carrying a
            // couple of attributes is exactly the size of a hello, so the
            // magic cookie is the only thing that can tell them apart, and
            // asking it first costs one comparison.
            if (manx_stun::looks_like_stun(&wire,
                                           static_cast<std::size_t>(received))) {
                manx_stun::mapped_endpoint mapped{};
                nat_kind kind = nat();
                bool known = false;
                if (probe.absorb(&wire, static_cast<std::size_t>(received),
                                 std::chrono::steady_clock::now(), mapped,
                                 kind, known)) {
                    if (known) {
                        m_public_endpoint.store(
                            (static_cast<uint64_t>(mapped.ipv4) << 16) |
                                mapped.port,
                            std::memory_order_release);
                        std::printf("Lobby socket is seen from the internet "
                                    "as %s:%u\n",
                                    address_text(mapped.ipv4).c_str(),
                                    mapped.port);
                    }
                    if (kind != nat()) {
                        m_nat.store(static_cast<uint8_t>(kind),
                                    std::memory_order_release);
                        if (kind == nat_kind::symmetric)
                            std::printf("This router gives every destination "
                                        "its own port, so other machines "
                                        "cannot connect directly to this "
                                        "one.\n");
                    }
                }
                continue;
            }

            if (received == static_cast<int>(sizeof(lobby_log_packet)) &&
                wire.log.magic == log_magic && wire.log.version == 1 &&
                wire.log.nonce != m_nonce) {
                absorb_peer_log(wire.log.nonce, wire.log.first_line,
                                wire.log.text,
                                std::min<std::size_t>(wire.log.bytes,
                                                      log_payload_bytes));
                continue;
            }
            if (received == static_cast<int>(sizeof(lobby_session_packet)) &&
                wire.session.magic == session_magic &&
                wire.session.version == 1 &&
                wire.session.nonce != m_nonce) {
                std::function<void(const void*, std::size_t)> receiver;
                {
                    std::lock_guard<std::mutex> lock(m_session_mutex);
                    receiver = m_session_receiver;
                }
                if (receiver)
                    receiver(wire.session.payload,
                             std::min<std::size_t>(wire.session.bytes,
                                                   max_session_payload));
                continue;
            }
            if (received != static_cast<int>(sizeof(lobby_packet))) continue;
            const lobby_packet& incoming = wire.hello;
            if (incoming.magic != lobby_magic || incoming.version != 2 ||
                incoming.nonce == 0 || incoming.nonce == m_nonce)
                continue;

            const auto arrival = std::chrono::steady_clock::now();
            const bool first_sighting = last_seen.count(incoming.nonce) == 0;
            last_seen[incoming.nonce] = arrival;
            endpoints[incoming.nonce] = sender;
            {
                // A hello arriving from an address we were seeded with is
                // the punch succeeding. Remember which machines those are:
                // from here they are ordinary peers in every respect except
                // how long they are allowed to go quiet.
                std::lock_guard<std::mutex> lock(m_remote_mutex);
                for (const lobby_link::remote_peer& seed : m_remote_peers)
                    if (seed.ipv4 == sender.sin_addr.s_addr &&
                        seed.port == ntohs(sender.sin_port)) {
                        remote_nonces.insert(incoming.nonce);
                        break;
                    }
            }
            if (first_sighting) {
                // A machine that has just arrived gets the recent history of
                // this one, not only what happens from here.
                const uint32_t total = manx_log_tap::next_sequence();
                log_cursor[incoming.nonce] =
                    total > log_backfill_lines ? total - log_backfill_lines
                                               : 0;
            }

            uint64_t host_nonce = 0;
            bool all_refused = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto found = std::find_if(
                    m_machines.begin(), m_machines.end(),
                    [&](const lobby_machine& machine) {
                        return machine.nonce == incoming.nonce;
                    });
                if (found == m_machines.end() &&
                    m_machines.size() <
                        static_cast<std::size_t>(lobby_max_machines)) {
                    m_machines.push_back(lobby_machine{});
                    found = std::prev(m_machines.end());
                    found->nonce = incoming.nonce;
                }
                if (found != m_machines.end()) {
                    found->ipv4 = sender.sin_addr.s_addr;
                    found->port = ntohs(sender.sin_port);
                    found->address = address_text(found->ipv4);
                    found->name.assign(
                        incoming.name,
                        strnlen(incoming.name, sizeof(incoming.name)));
                    found->games = incoming.games;
                    found->presence = incoming.presence <= 2 ?
                        static_cast<machine_presence>(incoming.presence) :
                        machine_presence::available;
                    found->invite =
                        static_cast<multiplayer_invite>(incoming.invite);
                    found->node = incoming.node;
                    // The machine inviting this one is its host, and its
                    // word on player numbers is the one that counts.
                    if (found->invite == multiplayer_invite::inviting &&
                        !hosting()) {
                        if (presence() == machine_presence::unavailable) {
                            // Told not to accept anything: answer for the
                            // player rather than interrupting them.
                            m_local_invite.store(
                                static_cast<uint8_t>(
                                    multiplayer_invite::declined),
                                std::memory_order_release);
                        } else {
                            m_host_nonce = found->nonce;
                        }
                    }

                    // A refusal has to be taken for an answer. Leaving the
                    // invitation standing deadlocks the pair: the host keeps
                    // asking, so the machine that said no is never free to be
                    // asked anything else, so it keeps saying no.
                    if (found->invite == multiplayer_invite::declined &&
                        hosting())
                        m_invited.erase(
                            std::remove(m_invited.begin(), m_invited.end(),
                                        found->nonce),
                            m_invited.end());
                }
                host_nonce = m_host_nonce;
                // Everyone asked has said no. Judged here, holding the same
                // lock that the invited list is changed under, so a machine
                // that is mid-invitation is never mistaken for one that has
                // been refused.
                bool anyone_refused = false;
                for (const lobby_machine& machine : m_machines)
                    if (machine.invite == multiplayer_invite::declined)
                        anyone_refused = true;
                all_refused = hosting() && anyone_refused && m_invited.empty();

                // Nobody is asking this machine any more, so a "no" it gave
                // earlier has been heard and is spent. Back to idle, ready to
                // be asked again - otherwise one refusal would bar this
                // machine from every future invitation.
                if (!hosting() &&
                    static_cast<multiplayer_invite>(
                        m_local_invite.load(std::memory_order_acquire)) ==
                        multiplayer_invite::declined) {
                    bool anyone_asking = false;
                    for (const lobby_machine& machine : m_machines)
                        if (machine.invite == multiplayer_invite::inviting)
                            anyone_asking = true;
                    if (!anyone_asking) {
                        m_local_invite.store(0, std::memory_order_release);
                        m_host_nonce = 0;
                        host_nonce = 0;
                    }
                }
            }
            m_connected.store(true, std::memory_order_release);
            m_search_exhausted.store(false, std::memory_order_release);
            if (all_refused) {
                // Every machine asked has refused. Stop hosting so the lobby
                // shows the refusal and offers to ask again, rather than
                // sitting on a request nobody is going to answer.
                m_hosting.store(false, std::memory_order_release);
                m_local_invite.store(0, std::memory_order_release);
                m_node.store(0, std::memory_order_release);
                m_agreed.store(0, std::memory_order_release);
                m_declined.store(true, std::memory_order_release);
            }

            // Not hosting: take the player number and the game from the
            // machine that invited this one.
            if (!hosting() && incoming.nonce == host_nonce) {
                m_host_ipv4.store(sender.sin_addr.s_addr,
                                  std::memory_order_release);
                const auto mine = static_cast<multiplayer_invite>(
                    m_local_invite.load(std::memory_order_acquire));
                if (mine == multiplayer_invite::accepted &&
                    incoming.assigned_node >= 2) {
                    m_node.store(incoming.assigned_node,
                                 std::memory_order_release);
                    m_agreed.store(std::max<int>(incoming.cabinet_count, 2),
                                   std::memory_order_release);
                    if (incoming.launch_sequence != 0 &&
                        incoming.launch_sequence !=
                            m_received_launch_sequence &&
                        incoming.game[0] != '\0') {
                        m_received_launch_sequence = incoming.launch_sequence;
                        m_launched_count.store(
                            std::max<int>(incoming.cabinet_count, 2),
                            std::memory_order_release);
                        m_in_session.store(true, std::memory_order_release);
                        std::lock_guard<std::mutex> lock(m_launch_mutex);
                        m_incoming_game.assign(
                            incoming.game,
                            strnlen(incoming.game, sizeof(incoming.game)));
                    }
                } else if (mine != multiplayer_invite::accepted) {
                    m_node.store(0, std::memory_order_release);
                }
            }

            // Answer immediately rather than on the next tick: a machine
            // that has just spoken is one worth replying to now.
            next_hello = std::chrono::steady_clock::time_point{};
        }

        const auto now = std::chrono::steady_clock::now();

        // The public-address probe, once the resolver has finished and only
        // if anybody asked for one. LAN play never does, and never pays for
        // it: without a request this whole block is one atomic load.
        if (probe.servers.empty() &&
            m_stun_resolved.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(m_stun_mutex);
            for (std::size_t index = 0; index < m_stun_ipv4.size(); ++index)
                probe.servers.push_back(
                    stun_probe::server{m_stun_ipv4[index], m_stun_port[index]});
            if (!probe.servers.empty()) probe.begin(now, 0);
        }
        if (!probe.servers.empty()) {
            probe.restart_if_due(now);
            probe.tick(socket, now);
        }

        // Retire machines that have stopped answering.
        {
            std::vector<uint64_t> gone;
            for (const auto& entry : last_seen)
                if (now - entry.second >
                    lobby_link::timeout_for(
                        remote_nonces.count(entry.first) != 0))
                    gone.push_back(entry.first);
            if (!gone.empty()) {
                bool empty_now = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    for (const uint64_t nonce : gone) {
                        last_seen.erase(nonce);
                        endpoints.erase(nonce);
                        log_cursor.erase(nonce);
                        remote_nonces.erase(nonce);
                        m_machines.erase(
                            std::remove_if(
                                m_machines.begin(), m_machines.end(),
                                [nonce](const lobby_machine& machine) {
                                    return machine.nonce == nonce;
                                }),
                            m_machines.end());
                        m_invited.erase(
                            std::remove(m_invited.begin(), m_invited.end(),
                                        nonce),
                            m_invited.end());
                        if (m_host_nonce == nonce) {
                            // The machine driving this one has gone. Drop
                            // the session rather than wait for a game that
                            // is never going to be chosen.
                            m_host_nonce = 0;
                            m_local_invite.store(0, std::memory_order_release);
                            m_node.store(0, std::memory_order_release);
                            m_agreed.store(0, std::memory_order_release);
                        }
                    }
                    empty_now = m_machines.empty();
                }
                m_connected.store(!empty_now, std::memory_order_release);
                if (empty_now) {
                    m_hosting.store(false, std::memory_order_release);
                    m_search_exhausted.store(false,
                                             std::memory_order_release);
                    sweep.restart();
                }
            }
        }

        // Work out the roster. The host is Player 1 and the machines that
        // accepted take 2..N in nonce order, which both ends compute from
        // the same information and so always agree on.
        std::map<uint64_t, int> assigned;
        int session_size = 0;
        if (hosting()) {
            std::vector<uint64_t> accepted;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (const lobby_machine& machine : m_machines)
                    if (machine.invite == multiplayer_invite::accepted &&
                        std::find(m_invited.begin(), m_invited.end(),
                                  machine.nonce) != m_invited.end())
                        accepted.push_back(machine.nonce);
            }
            std::sort(accepted.begin(), accepted.end());
            int places;
            {
                std::lock_guard<std::mutex> lock(m_launch_mutex);
                places = m_launch_places;
            }
            int next_node = 2;
            for (const uint64_t nonce : accepted) {
                if (next_node > std::min(places, lobby_max_machines)) break;
                assigned[nonce] = next_node++;
            }
            session_size = next_node - 1;
            m_node.store(session_size >= 2 ? 1 : 0,
                         std::memory_order_release);
            m_agreed.store(session_size >= 2 ? session_size : 0,
                           std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (lobby_machine& machine : m_machines) {
                    const auto found = assigned.find(machine.nonce);
                    machine.node =
                        found == assigned.end() ? 0 : found->second;
                }
            }
        }

        // A running game wants the socket read the instant something lands;
        // a launcher sitting idle does not need to be woken 1000 times a
        // second to find nothing.
        const bool in_session = m_in_session.load(std::memory_order_acquire);

        if (next_hello.time_since_epoch().count() == 0 || now >= next_hello) {
            lobby_packet outgoing{};
            outgoing.magic = lobby_magic;
            outgoing.version = 2;
            outgoing.node = static_cast<uint8_t>(
                std::clamp(m_node.load(std::memory_order_acquire), 0,
                           lobby_max_machines));
            outgoing.nonce = m_nonce;
            outgoing.games = m_local_games.load(std::memory_order_acquire);
            outgoing.launch_sequence =
                m_launch_sequence.load(std::memory_order_acquire);
            outgoing.presence = static_cast<uint8_t>(presence());
            outgoing.cabinet_count = static_cast<uint8_t>(
                std::clamp(session_size, 0, lobby_max_machines));
            std::memcpy(outgoing.name, m_local_name.c_str(),
                        std::min(m_local_name.size(),
                                 sizeof(outgoing.name) - 1));
            {
                std::lock_guard<std::mutex> lock(m_launch_mutex);
                std::memcpy(outgoing.game, m_outgoing_game.c_str(),
                            std::min(m_outgoing_game.size(),
                                     sizeof(outgoing.game) - 1));
            }

            const auto local_state = static_cast<multiplayer_invite>(
                m_local_invite.load(std::memory_order_acquire));
            std::vector<uint64_t> invited;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                invited = m_invited;
            }

            outgoing.invite = static_cast<uint8_t>(local_state);
            send_packet(socket, outgoing, loopback);

            // Each machine that has answered is told its own player number,
            // and only the machines actually asked are told they are being
            // asked.
            for (const auto& entry : endpoints) {
                lobby_packet personal = outgoing;
                if (hosting()) {
                    const bool asked =
                        std::find(invited.begin(), invited.end(),
                                  entry.first) != invited.end();
                    personal.invite = static_cast<uint8_t>(
                        asked ? multiplayer_invite::inviting
                              : multiplayer_invite::idle);
                }
                const auto place = assigned.find(entry.first);
                personal.assigned_node = static_cast<uint8_t>(
                    place == assigned.end() ? 0 : place->second);
                send_packet(socket, personal, entry.second);

                uint32_t& cursor = log_cursor[entry.first];
                uint32_t first = cursor;
                const std::vector<std::string> lines =
                    manx_log_tap::lines_from(cursor, 32, &first);
                if (lines.empty()) {
                    cursor = manx_log_tap::next_sequence();
                    continue;
                }
                lobby_log_packet relay{};
                relay.magic = log_magic;
                relay.version = 1;
                relay.nonce = m_nonce;
                relay.first_line = first;
                std::size_t used = 0;
                uint32_t count = 0;
                for (const std::string& line : lines) {
                    if (used + line.size() + 1 > log_payload_bytes) break;
                    std::memcpy(relay.text + used, line.data(), line.size());
                    used += line.size();
                    relay.text[used++] = '\n';
                    ++count;
                }
                if (count == 0) {
                    // One line longer than the whole payload: send what
                    // fits rather than wedging the cursor here for ever.
                    used = log_payload_bytes - 1;
                    std::memcpy(relay.text, lines.front().data(), used);
                    relay.text[used++] = '\n';
                    count = 1;
                }
                relay.bytes = static_cast<uint16_t>(used);
                relay.line_count = count;
                send_packet(socket, relay, entry.second);
                cursor = first + count;
            }

            std::vector<lobby_link::remote_peer> seeds;
            {
                std::lock_guard<std::mutex> lock(m_remote_mutex);
                seeds = m_remote_peers;
            }
            if (!seeds.empty()) {
                // A seed gets the plain hello and nothing else: it has no
                // nonce yet, so there is nothing personal to say to it, and
                // a console log is not worth pushing down a metered link to
                // a machine that may not even be there. What this datagram
                // is really for is opening this router's return path so the
                // other machine's hello can arrive.
                std::vector<lobby_link::hello_target> answering;
                answering.reserve(endpoints.size());
                for (const auto& entry : endpoints)
                    answering.push_back(lobby_link::hello_target{
                        entry.second.sin_addr.s_addr,
                        ntohs(entry.second.sin_port), true});
                const auto self = public_endpoint();
                const std::vector<lobby_link::hello_target> targets =
                    lobby_link::merge_hello_targets(
                        answering, seeds, self ? self->ipv4 : 0,
                        self ? self->port : 0);
                sockaddr_in target{};
                target.sin_family = AF_INET;
                for (const lobby_link::hello_target& at : targets) {
                    if (at.relay_logs) continue;   // answered already, sent above
                    target.sin_addr.s_addr = at.ipv4;
                    target.sin_port = htons(at.port);
                    send_packet(socket, outgoing, target);
                }
            }

            if (lobby_link::should_sweep(!endpoints.empty())) {
                // Nobody found yet. Broadcast plus a paced sweep of the
                // subnet - see lan_peers.h for why the sweep is what carries
                // this through a deny-by-default firewall. Seeds deliberately
                // do not suppress this: a machine punching at somebody in
                // another country must still find the one next to it.
                sockaddr_in target{};
                target.sin_family = AF_INET;
                target.sin_port = htons(lobby_port);
                for (const uint32_t address : sweep.next()) {
                    target.sin_addr.s_addr = address;
                    send_packet(socket, outgoing, target);
                }
                // "Nothing on this network" becomes advice about firewalls
                // in the launcher, and that advice would be wrong while a
                // punch to somewhere far away is still in flight.
                if (lobby_link::search_is_exhausted(sweep.swept_once(),
                                                    !seeds.empty()))
                    m_search_exhausted.store(true, std::memory_order_release);
            }
            next_hello = now + std::chrono::milliseconds(200);
        }
        wait_for_traffic(socket, in_session ? 1 : 20);
    }
    m_socket.store(-1, std::memory_order_release);
    close_socket(socket);
}
