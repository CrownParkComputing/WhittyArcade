// online_link.h - the cabinet's side of MANX online.
//
// What it does is narrower than it looks. It does not run a lobby, agree a
// game, or carry a single byte of gameplay. Its whole job is to get two
// machines that cannot see each other to exchange public addresses, hand
// those addresses to multiplayer_lobby, and then get out of the way: once
// the hellos are flowing, a machine in another country is an ordinary entry
// in the same roster, and the invitation, the player numbers, the launch and
// the lockstep link all work exactly as they do on a LAN.
//
// Everything here is asynchronous. Public methods queue work and return, so
// nothing on the render thread ever waits for a network round trip.
#pragma once

#include "multiplayer_lobby.h"
#include "online_wire.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class online_state : uint8_t {
    disabled = 0,   // no libcurl, or no project configured
    signed_out,     // configured, nobody signed in, no pairing under way
    registering,    // creating an account from the cabinet
    signing_in,
    online,         // signed in, publishing presence
    in_lobby,       // members mirrored into the lobby as remote peers
    error,
};

class online_link {
public:
    explicit online_link(multiplayer_lobby& lobby);
    ~online_link();

    online_link(const online_link&) = delete;
    online_link& operator=(const online_link&) = delete;

    // False when this build has no libcurl or no Firebase project. The
    // launcher hides the Internet card rather than offering something that
    // cannot work.
    static bool available();

    // --- getting an account ---------------------------------------------
    // Done on the cabinet, with no website and no second device. A machine
    // is not an identity of its own any more: it is one of the machines
    // belonging to whoever is signed in on it, and it registers itself the
    // moment that happens.
    void register_account(std::string display_name, std::string email,
                          std::string password);
    void sign_in(std::string email, std::string password);
    std::string account_email() const;
    std::string display_name() const;

    void sign_out();       // forgets the token, remembers the address
    void forget_machine(); // wipes the credential file entirely
    bool signed_in() const;
    std::string machine_name() const;

    // --- being visible ---------------------------------------------------
    // MAME short names, not the lobby's index mask: two machines with
    // different plugins installed number the same game differently, so the
    // mask means nothing between them.
    void set_installed_games(std::vector<std::string> short_names);
    // The online screen is open, so poll often enough to feel live. Closed,
    // the cabinet drops back to a slow heartbeat - which is most of what
    // keeps this inside the free tier.
    void set_foreground(bool foreground);

    // --- lobbies ----------------------------------------------------------
    // Hosting one, or joining somebody else's by the code they read out.
    void create_lobby();
    void join_lobby(std::string lobby_id);
    void leave_lobby();
    std::string joined_lobby() const;
    std::vector<online_wire::member> members() const;

    // --- what the screen draws --------------------------------------------
    online_state state() const;
    std::string status_text() const;
    // Bumped whenever anything drawable changed, so the lobby screen's
    // existing redraw-on-change idiom works for the cloud too.
    uint64_t revision() const;

private:
    struct command {
        enum class kind {
            register_account, sign_in, sign_out, forget, host, join, leave
        };
        kind what;
        std::string argument;   // email, or the lobby id
        std::string secret;     // password
        std::string extra;      // display name
    };

    void run();
    void publish(online_state state, std::string status);
    void touch();

    multiplayer_lobby& m_lobby;
    std::thread m_thread;
    std::atomic_bool m_stop{false};
    std::atomic<online_state> m_state{online_state::disabled};
    std::atomic_uint64_t m_revision{0};
    std::atomic_bool m_foreground{false};

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<command> m_commands;
    std::string m_status;
    std::string m_email;
    std::string m_display_name;
    std::string m_lobby_id;
    std::string m_machine_uid;
    std::string m_machine_name;
    std::vector<std::string> m_games;
    std::vector<online_wire::member> m_members;
    bool m_games_dirty{false};
};
