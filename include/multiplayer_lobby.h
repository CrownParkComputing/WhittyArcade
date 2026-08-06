// multiplayer_lobby.h - automatic launcher-level cabinet discovery.
#pragma once

#include "arcade_types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Where a machine is in agreeing to a game. Nothing launches until both
// sides have said yes: the machine that asks becomes Player 1 and chooses
// the game, and the machines that accept follow it in.
enum class multiplayer_invite : uint8_t {
    idle = 0,
    inviting = 1,
    accepted = 2,
    declined = 3,
};

// What a machine is willing to be asked. Published with every hello, so the
// lobby can show at a glance who is worth asking rather than letting someone
// send an invitation into a machine that is mid-game or has said not now.
enum class machine_presence : uint8_t {
    available = 0,   // in the launcher, happy to be asked
    in_game = 1,     // playing something; an invitation would interrupt it
    unavailable = 2, // told not to accept anything
};

// The most cabinets one session can hold. The comm ring the arcade boards
// themselves implement tops out here, so nothing is gained by going wider.
constexpr int lobby_max_machines = 8;

// One other MANX on the network, as the lobby screen needs to draw it.
struct lobby_machine {
    uint64_t nonce{};
    uint32_t ipv4{};
    uint16_t port{};          // where its lobby is answering from
    std::string name;         // that machine's own hostname
    std::string address;      // dotted quad, for when two share a name
    uint64_t games{};         // which supported games it has installed
    multiplayer_invite invite{multiplayer_invite::idle};
    machine_presence presence{machine_presence::available};
    int node{};               // player number, 0 until a host assigns one
    bool has_log{};

    // What to call it on screen: the name if it gave one, the address if
    // not, so a machine is always identifiable as something.
    std::string label() const { return name.empty() ? address : name; }
};

class multiplayer_lobby {
public:
    multiplayer_lobby();
    ~multiplayer_lobby();

    multiplayer_lobby(const multiplayer_lobby&) = delete;
    multiplayer_lobby& operator=(const multiplayer_lobby&) = delete;

    void set_installed_games(const std::vector<rom_choice>& games);

    // Every machine currently answering, in discovery order. A machine that
    // goes quiet drops out after a second and a half.
    std::vector<lobby_machine> machines() const;
    // True while at least one other machine is answering.
    bool connected() const;
    // This machine's own name, as the others see it.
    std::string local_name() const;

    // Whether this machine will accept an invitation, and what the others are
    // told about it. Going into a game sets it on its own; the rest is the
    // player's choice.
    void set_presence(machine_presence state);
    machine_presence presence() const;

    // True once every address on this machine's networks has been tried
    // several times over with no answer. Long enough to mean something is
    // wrong rather than slow, so the launcher can stop saying "searching"
    // and say what a user can actually do about it.
    bool search_exhausted() const;

    // --- agreeing on a game -------------------------------------------
    // Asks one machine, or every machine found, for a game. Either makes
    // this machine the host, which is Player 1 and chooses what to play.
    void invite(uint64_t nonce);
    void invite_everyone();
    // Answers the invitation that arrived from another machine.
    void answer_invite(bool allow);
    // Withdraws this machine's invitations, or leaves a session it had
    // agreed to. Safe to call in any state.
    void cancel_invite();

    bool hosting() const;
    // Set when every machine this one asked has refused. The lobby shows the
    // refusal and offers to ask again; asking clears it.
    bool invitation_refused() const;
    void clear_refusal();
    // Set when another machine has invited this one and it has not answered.
    std::optional<lobby_machine> pending_invitation() const;
    // This machine's player number: 1 when hosting a session somebody has
    // accepted, 2..N when a host has placed it, 0 when not in a session.
    int node() const;
    // Machines in the session, including this one. 0 when there is none.
    int agreed_count() const;

    // --- starting the game --------------------------------------------
    // Host only. Publishes the game, and with it the roster: `places` caps
    // how many machines take part, which is how a two-player netplay title
    // starts on two machines out of five.
    void launch_game(std::string_view short_name, int places);
    bool launch_pending() const;
    std::optional<std::string> take_launch();
    // The host's address, handed to the cabinet this machine starts.
    uint32_t host_ipv4() const;

    // This machine's game has ended. Stops advertising the session, so the
    // other machines see it dissolve and come back to the lobby too.
    void leave_session();
    // True once a session this machine was in has broken up: the host
    // withdrew or went away, or - when this machine is the host - one of the
    // cabinets left. Machines in a game poll this so that closing on any one
    // of them brings all of them back to the lobby together.
    bool session_ended() const;

    // True when every machine that would take part has the named game.
    bool everyone_has_game(std::string_view short_name, int places) const;

    // --- in-game traffic -----------------------------------------------
    // Once a game is running, the cabinets keep talking over this same
    // socket rather than opening a second link of their own. It is already
    // up, already unicast to every machine in the roster, and already
    // through whatever firewalls are in the way - a second link would have
    // to win all of that again from inside a running game.
    //
    // Sends an opaque payload to every machine in the session. Safe to call
    // from the emulation thread; UDP sends on one socket are.
    void send_to_session(const void* data, std::size_t bytes);
    // Installs the handler the receive loop calls for arriving session
    // payloads. Called on the lobby thread, so the handler must be brief.
    void set_session_receiver(
        std::function<void(const void*, std::size_t)> receiver);
    // The largest payload send_to_session will carry.
    static constexpr std::size_t max_session_payload = 512;

    // --- diagnostics ---------------------------------------------------
    // Another machine's console output, relayed continuously over the same
    // link that found it. Kept after that machine goes quiet, because the
    // lines it printed just before it died are the ones worth reading.
    std::vector<std::string> peer_log(uint64_t nonce) const;
    bool has_any_log() const;

    // Called by the receive loop for each relayed slice; public only because
    // it is the receive path, not something a caller should reach for.
    void absorb_peer_log(uint64_t nonce, uint32_t first_line,
                         const char* text, std::size_t bytes);

private:
    void run();

    std::thread m_thread;
    std::atomic_bool m_stop{false};
    std::atomic_bool m_connected{false};
    std::atomic_bool m_search_exhausted{false};
    std::atomic_bool m_hosting{false};
    std::atomic_bool m_declined{false};
    std::atomic_uint8_t m_presence{0};
    std::atomic_int m_node{0};
    std::atomic_int m_agreed{0};
    std::atomic_uint64_t m_local_games{0};
    std::atomic_uint32_t m_launch_sequence{0};
    std::atomic_uint32_t m_host_ipv4{0};
    std::atomic_uint8_t m_local_invite{0};
    // How many cabinets were in the session when the game started, so a
    // machine dropping out is visible as the count falling below it.
    std::atomic_int m_launched_count{0};
    std::atomic_bool m_in_session{false};
    uint64_t m_nonce{};
    std::string m_local_name;

    static constexpr std::size_t peer_log_capacity = 400;

    // Guards the machine table, the relayed logs and the invitation set.
    mutable std::mutex m_mutex;
    std::vector<lobby_machine> m_machines;
    std::vector<uint64_t> m_invited;     // who this machine has asked
    uint64_t m_host_nonce{};             // who asked this machine
    std::map<uint64_t, std::deque<std::string>> m_logs;
    std::map<uint64_t, uint32_t> m_log_next;

    // Guards the session receiver, which is installed from the host loop
    // and called from the lobby thread.
    mutable std::mutex m_session_mutex;
    std::function<void(const void*, std::size_t)> m_session_receiver;
    std::intptr_t m_socket{-1};

    mutable std::mutex m_launch_mutex;
    std::string m_outgoing_game;
    std::string m_incoming_game;
    int m_launch_places{2};
    uint32_t m_received_launch_sequence{};
};
