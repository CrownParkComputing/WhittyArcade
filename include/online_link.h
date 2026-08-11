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
#include "json.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

// One joinable lobby, as the browse screen needs to draw it.
struct online_lobby {
    std::string id;
    std::string host;      // who made it, in words
    std::string game;
    int members{};
    int places{};
    bool open_to_anyone{};
    bool stale{};
};

// Somebody this account is friends with, or has asked to be, or has been
// asked by. One shape for all three, because the friends screen draws them
// in one list and the difference is a badge, not a category.
struct online_friend {
    std::string uid;
    std::string name;      // their display name, empty until their profile arrives
    bool accepted{};       // false while the request is still unanswered
    bool incoming{};       // they asked us, so this is the one to answer
    bool online{};         // their profile says they are about
};

// The title's own identity for one ranked board.  Xbox 360 recomp plugins
// obtain these values from XSessionWriteStats and the signed SPA/XDBF table;
// native plugins may provide their own stable ids.  Keeping the property id
// in the key matters because two views are allowed to share a board number
// while ranking different values.
struct online_leaderboard_id {
    uint32_t title_id{};
    uint32_t leaderboard_id{};
    uint32_t property_id{};
    bool lower_is_better{};
};

struct online_leaderboard_submission {
    struct property {
        uint32_t property_id{};
        uint64_t value{};
    };
    online_leaderboard_id board;
    uint64_t value{};
    std::vector<property> metadata;
    std::string build_hash;
    // Empty until deterministic replay capture exists.  It is recorded now so
    // the storage schema does not have to change when verification arrives.
    std::string replay_hash;
};

struct online_leaderboard_entry {
    std::string uid;
    std::string display_name;
    uint64_t value{};
    bool verified{};
    int64_t submitted_unix{};
    std::vector<online_leaderboard_submission::property> metadata;
};

struct online_cloud_save {
    uint32_t title_id{};
    std::string file_name;
    std::vector<uint8_t> payload;
    std::string checksum;
    int64_t updated_unix{};
    // Written only after Firestore accepts this exact checksum. It lets the
    // next launch distinguish an unsynced local save from a safely mirrored
    // one without trusting wall-clock ordering between two cabinets.
    std::string sync_marker_path;
};

struct online_cloud_save_result {
    enum class state { unavailable, missing, available, error } status{
        state::unavailable};
    online_cloud_save save;
    std::string message;
};

enum class online_state : uint8_t {
    disabled = 0,   // no libcurl, or no project configured
    signed_out,     // configured, nobody signed in
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
    // `remember` decides whether the token that grants access is written to
    // this cabinet's disk. Saying no keeps the session to this run only,
    // which is what a shared or public machine wants.
    void register_account(std::string display_name, std::string email,
                          std::string password, bool remember);
    void sign_in(std::string email, std::string password, bool remember);
    std::string account_email() const;
    std::string display_name() const;

    void sign_out();       // forgets the token, remembers the address
    void forget_machine(); // wipes the credential file entirely
    bool signed_in() const;
    // True when this machine already has an account remembered on disk, and
    // is therefore going to sign itself in shortly. Distinct from signed_in:
    // that is false for the first few seconds of every start, which is not
    // the same thing as nobody having an account.
    bool remembered() const;
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
    // An open lobby is visible to anyone signed in; a locked one is only
    // reachable by somebody who has been given the code.
    // The game is chosen when the lobby is made, not after everybody has
    // arrived. Somebody reading a list of lobbies is choosing what to play,
    // and a lobby with no game in it is not something anyone can choose.
    // `places` is how many machines it takes; `mode` is the manifest's
    // multiplayer spelling, which the rules validate.
    void create_lobby(bool open_to_anyone, std::string game_short_name,
                      std::string game_display_name, int places,
                      std::string mode);
    // Host only: stop waiting and go. A two-place lobby does this by itself
    // the moment the second machine is in - there is nothing left to decide
    // and nobody to wait for.
    void start_lobby();
    void join_lobby(std::string lobby_id);
    void leave_lobby();
    std::string joined_lobby() const;
    // Lobbies this account may join: the open ones, and the friends-only
    // ones its friends have made. Refreshed while the join screen is up.
    void refresh_lobbies();
    std::vector<online_lobby> lobbies() const;
    std::vector<online_wire::member> members() const;

    // What the joined lobby is for, and where it has got to.
    std::string lobby_game() const;        // MAME short name, empty until known
    std::string lobby_game_name() const;   // what to call it on screen
    int lobby_places() const;
    bool hosting_lobby() const;            // this account made it
    bool lobby_starting() const;           // the host has said go
    // The ports the service settled on for this session's linked game, and
    // where the other cabinet is. Both are known before either board exists,
    // which is exactly when the real comm hardware needs them.
    int lobby_port_base() const;
    std::string peer_link_address() const;

    // A lobby that has appeared since the last time anybody looked, taken
    // rather than read: the launcher asks somebody whether they want to join
    // it, and asking twice about the same lobby is worse than not asking at
    // all. Empty when there is nothing new.
    std::optional<online_lobby> take_new_lobby();

    // --- friends ----------------------------------------------------------
    // Everyone this account is friends with, plus the requests either side
    // has yet to answer. Refreshed on its own timer while the friends screen
    // is up, and slowly the rest of the time, because an unanswered request
    // is something to be told about rather than something to go and look for.
    std::vector<online_friend> friends() const;
    void refresh_friends();
    // Exact match on a display name. handles/{name} exists precisely so a
    // cabinet can find one person without being able to read out the whole
    // directory - so there is no search-as-you-type here, and cannot be.
    void add_friend(std::string name);
    void answer_friend(std::string uid, bool accept);

    // --- persistent online leaderboards ---------------------------------
    // Both operations are asynchronous like every other network operation in
    // this class.  Firestore rules keep only the account's best value and
    // force client submissions to remain visibly unverified.
    void submit_score(online_leaderboard_submission submission);
    void refresh_leaderboard(online_leaderboard_id board);
    std::vector<online_leaderboard_entry> leaderboard() const;
    std::string leaderboard_status() const;

    // Save fetch is the one deliberately synchronous cloud operation: it is
    // called before a title opens its local container, where downloading in
    // the background would race the game's first read. It waits only when an
    // account is already online and is bounded by timeout_ms.
    online_cloud_save_result fetch_cloud_save(uint32_t title_id,
                                               int timeout_ms = 8000);
    void submit_cloud_save(online_cloud_save save);

    // --- what the screen draws --------------------------------------------
    online_state state() const;
    std::string status_text() const;
    // Bumped whenever anything drawable changed, so the lobby screen's
    // existing redraw-on-change idiom works for the cloud too.
    uint64_t revision() const;

private:
    struct cloud_fetch_waiter {
        std::mutex mutex;
        std::condition_variable ready;
        bool done{};
        uint32_t title_id{};
        online_cloud_save_result result;
    };

    struct command {
        enum class kind {
            register_account, sign_in, sign_out, forget, host, join, leave,
            browse, friends, befriend, answer_friend, start,
            submit_score, fetch_leaderboard, fetch_cloud_save,
            submit_cloud_save
        };
        kind what;
        std::string argument;   // email, the lobby id, or the game
        std::string secret;     // password, or the multiplayer mode
        std::string extra;      // display name, or the game's title
        int number{};           // places, when hosting
        bool flag{};            // stay signed in, or open to anyone
        online_leaderboard_submission score;
        online_cloud_save cloud_save;
        std::shared_ptr<cloud_fetch_waiter> cloud_fetch;
    };

    void run();
    // The held-open request that carries news from the lobby service.
    //
    // Its own thread, because it is meant to block: it asks the service for
    // the next thing that happens and is answered the moment it happens. On
    // the worker's thread that wait would sit in front of the heartbeat and
    // the friends poll, so it does not live there.
    void watch_events();
    // Turns the service's description of a lobby into what the launcher
    // draws and what the lobby thread punches at.
    void apply_lobby(const nlohmann::json& detail, bool going);
    void publish(online_state state, std::string status);
    // Says something without claiming the machine's state changed. "No
    // account is called that" is an answer to a question somebody asked, not
    // a cabinet that has fallen off the internet - and publishing it as an
    // error would grey the corner out and make the launcher believe it was
    // signed out.
    void announce(std::string status);
    void touch();

    multiplayer_lobby& m_lobby;
    std::thread m_thread;
    std::thread m_events;
    std::atomic_bool m_stop{false};
    std::atomic<online_state> m_state{online_state::disabled};
    std::atomic_uint64_t m_revision{0};
    std::atomic_bool m_foreground{false};
    std::atomic_bool m_remembered{false};
    // When the last announcement was made, so the routine heartbeat does not
    // rub out an answer to something somebody has just asked.
    std::atomic_int64_t m_announced_unix{0};

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<command> m_commands;
    std::string m_status;
    std::string m_email;
    std::string m_display_name;
    std::string m_lobby_id;
    // Handed out by the lobby service when the cabinet presents its Firebase
    // token. Empty until it has one, which is the whole test for "can this
    // machine do lobbies yet".
    std::string m_session;
    int64_t m_event_seq{0};
    std::string m_machine_uid;
    std::string m_machine_name;
    std::vector<std::string> m_games;
    std::vector<online_wire::member> m_members;
    std::vector<online_lobby> m_lobbies;
    std::vector<online_friend> m_friends;
    std::vector<online_leaderboard_entry> m_leaderboard;
    std::string m_leaderboard_status;
    online_leaderboard_id m_leaderboard_id;
    std::string m_uid;          // this account, as the service knows it
    std::string m_lobby_game;
    std::string m_lobby_game_name;
    int m_lobby_places{};
    bool m_lobby_host{};
    bool m_lobby_starting{};
    // Settled by the service and told to every machine, so two cabinets
    // cannot work them out separately and disagree.
    int m_port_base{};
    int64_t m_seed{};
    int m_delay_frames{3};
    std::optional<online_lobby> m_new_lobby;
    bool m_games_dirty{false};
};
