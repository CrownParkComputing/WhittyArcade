// A cabinet, without the cabinet.
//
// Everything the account corner does - signing in, publishing a profile,
// claiming a handle, asking somebody to be friends, answering, watching for
// a lobby that has just opened - happens on online_link's worker thread and
// nowhere near the renderer. This drives that worker directly, so two
// "cabinets" are two processes on one machine rather than two machines in
// two rooms, and so the answer to "did the friend request arrive?" is a line
// of text rather than a photograph of a screen.
//
// It talks to the live Firebase project, which is why it is a harness and
// not a test: ctest must never need the internet, an account or a quota.
//
//   online_live_harness <config-dir> status
//   online_live_harness <config-dir> register <name> <email> <password>
//   online_live_harness <config-dir> signin <email> <password>
//   online_live_harness <config-dir> add <name>
//   online_live_harness <config-dir> friends
//   online_live_harness <config-dir> accept
//   online_live_harness <config-dir> host <game-short-name> [seconds]
//   online_live_harness <config-dir> join <lobby-id> [seconds]
//   online_live_harness <config-dir> watch [seconds]
//   online_live_harness <config-dir> signout
//
// The config directory is XDG_CONFIG_HOME, so each cabinet keeps its own
// credential and they do not sign each other out.

#include "online_link.h"
#include "lan_peers.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

// The lobby, stubbed. online_link asks it where this machine is and hands it
// the addresses it finds; none of that is what is under test here, and
// linking the real one would drag in UDP discovery, STUN and the ROM
// catalogue for the sake of two accessors.
multiplayer_lobby::multiplayer_lobby() {}
multiplayer_lobby::~multiplayer_lobby() {}
machine_presence multiplayer_lobby::presence() const {
    return machine_presence::available;
}
void multiplayer_lobby::set_remote_peers(
    std::vector<lobby_link::remote_peer>) {}
void multiplayer_lobby::begin_public_endpoint_probe() {}
std::optional<lobby_link::remote_peer> multiplayer_lobby::public_endpoint() const {
    return std::nullopt;
}
nat_kind multiplayer_lobby::nat() const { return nat_kind::unknown; }
// The one stub that is not a stub. This is what the fix publishes, so a
// harness that returned nothing here would prove nothing.
std::optional<lobby_link::remote_peer> multiplayer_lobby::local_endpoint() const {
    for (const lan::interface_v4& interface : lan::local_interfaces())
        if (interface.address != 0)
            return lobby_link::remote_peer{interface.address, 35109};
    return std::nullopt;
}
int multiplayer_lobby::node() const { return 0; }
bool multiplayer_lobby::connected() const { return false; }
int multiplayer_lobby::worst_rtt_ms() const { return 0; }

namespace {

const char* state_name(online_state state) {
    switch (state) {
    case online_state::disabled:    return "disabled";
    case online_state::signed_out:  return "signed out";
    case online_state::registering: return "registering";
    case online_state::signing_in:  return "signing in";
    case online_state::online:      return "online";
    case online_state::in_lobby:    return "in a lobby";
    default:                        return "error";
    }
}

void wait_a_moment(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

// Signed in and registered, or as far as it is going to get.
bool wait_until_online(online_link& link, int seconds) {
    for (int tick = 0; tick < seconds * 10; ++tick) {
        if (link.signed_in()) return true;
        if (link.state() == online_state::disabled) return false;
        wait_a_moment(100);
    }
    return link.signed_in();
}

void print_state(online_link& link) {
    std::printf("  state:  %s\n", state_name(link.state()));
    std::printf("  who:    %s <%s>\n", link.display_name().c_str(),
                link.account_email().c_str());
    std::printf("  says:   %s\n", link.status_text().c_str());
}

void print_friends(online_link& link) {
    const std::vector<online_friend> people = link.friends();
    if (people.empty()) {
        std::printf("  friends: none\n");
        return;
    }
    for (const online_friend& person : people)
        std::printf("  friend: %-24s %-40s %s%s\n",
                    person.name.empty() ? "(no name yet)" : person.name.c_str(),
                    person.uid.c_str(),
                    person.accepted ? (person.online ? "ONLINE" : "offline")
                                    : (person.incoming ? "ASKED YOU" : "asked"),
                    person.accepted ? "" : " (pending)");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <config-dir> <command> [args]\n", argv[0]);
        return 2;
    }
    ::setenv("XDG_CONFIG_HOME", argv[1], 1);
    const std::string command = argv[2];

    if (!online_link::available()) {
        std::printf("no online support in this build\n");
        return 1;
    }

    multiplayer_lobby lobby;
    online_link link(lobby);
    // The screens that show friends and lobbies ask for the fast poll, and
    // everything here is one of those screens.
    link.set_foreground(true);

    if (command == "register" && argc >= 6) {
        link.register_account(argv[3], argv[4], argv[5], true);
    } else if (command == "signin" && argc >= 5) {
        link.sign_in(argv[3], argv[4], true);
    }

    const bool up = wait_until_online(link, 40);
    std::printf("[%s] %s\n", argv[1], command.c_str());
    print_state(link);
    if (!up && command != "status") {
        std::printf("  not signed in - stopping here\n");
        return 1;
    }

    if (command == "status" || command == "register" || command == "signin") {
        // The profile and handle are written after the machine registers, so
        // give the worker a moment to get there before reporting the name.
        wait_a_moment(4000);
        print_state(link);
        link.refresh_friends();
        wait_a_moment(4000);
        print_friends(link);
        return up ? 0 : 1;
    }

    if (command == "add" && argc >= 4) {
        link.add_friend(argv[3]);
        wait_a_moment(6000);
        std::printf("  says:   %s\n", link.status_text().c_str());
        link.refresh_friends();
        wait_a_moment(5000);
        print_friends(link);
        return 0;
    }

    if (command == "friends") {
        link.refresh_friends();
        wait_a_moment(6000);
        print_friends(link);
        return 0;
    }

    if (command == "accept") {
        link.refresh_friends();
        wait_a_moment(6000);
        for (const online_friend& person : link.friends()) {
            if (!person.incoming) continue;
            std::printf("  accepting %s (%s)\n",
                        person.name.c_str(), person.uid.c_str());
            link.answer_friend(person.uid, true);
            wait_a_moment(6000);
            link.refresh_friends();
            wait_a_moment(5000);
            print_friends(link);
            return 0;
        }
        std::printf("  nothing waiting to be answered\n");
        print_friends(link);
        return 1;
    }

    if (command == "host") {
        const std::string game = argc >= 4 ? argv[3] : "galaga";
        const int seconds = argc >= 5 ? std::atoi(argv[4]) : 60;
        // Two places, so this lobby is one that starts by itself the moment
        // somebody joins - which is the case worth exercising.
        link.create_lobby(true, game, game, 2, "simultaneous");
        for (int tick = 0; tick < seconds; ++tick) {
            // Halfway through, say go without waiting for anybody. Nothing
            // will launch - there is no second machine and no renderer here
            // - but it exercises the one write only a host is allowed to
            // make, which is the write the rules are most likely to refuse.
            if (tick == seconds / 2 && !link.joined_lobby().empty()) {
                std::printf("  saying go\n");
                link.start_lobby();
            }
            if (!link.joined_lobby().empty() && tick % 10 == 0)
                std::printf("  lobby %s for %s - %d place(s), %s%s\n",
                            link.joined_lobby().c_str(),
                            link.lobby_game().c_str(), link.lobby_places(),
                            link.status_text().c_str(),
                            link.lobby_starting() ? " [STARTING]" : "");
            wait_a_moment(1000);
        }
        link.leave_lobby();
        wait_a_moment(3000);
        std::printf("  left: %s\n", link.status_text().c_str());
        return 0;
    }

    if (command == "join" && argc >= 4) {
        link.join_lobby(argv[3]);
        const int seconds = argc >= 5 ? std::atoi(argv[4]) : 45;
        for (int tick = 0; tick < seconds; ++tick) {
            if (tick % 5 == 0)
                std::printf("  lobby %s for %s - %zu member(s), %s%s\n",
                            link.joined_lobby().c_str(),
                            link.lobby_game().c_str(), link.members().size(),
                            link.status_text().c_str(),
                            link.lobby_starting() ? " [GO]" : "");
            wait_a_moment(1000);
        }
        link.leave_lobby();
        wait_a_moment(2000);
        return 0;
    }

    if (command == "watch") {
        const int seconds = argc >= 4 ? std::atoi(argv[3]) : 120;
        std::printf("  watching for a lobby nobody has offered yet\n");
        for (int tick = 0; tick < seconds; ++tick) {
            if (const std::optional<online_lobby> fresh = link.take_new_lobby()) {
                std::printf("  NEW LOBBY %s from %s%s%s\n", fresh->id.c_str(),
                            fresh->host.empty() ? "somebody"
                                                : fresh->host.c_str(),
                            fresh->game.empty() ? "" : " playing ",
                            fresh->game.c_str());
                return 0;
            }
            wait_a_moment(1000);
        }
        std::printf("  nothing new appeared in %d seconds\n", seconds);
        return 1;
    }

    if (command == "signout") {
        link.sign_out();
        wait_a_moment(4000);
        print_state(link);
        return 0;
    }

    std::printf("unknown command\n");
    return 2;
}
