#include "online_link.h"

#include "manx_cloud_config.h"
#include "manx_http.h"
#include "platform_paths.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <random>
#include <set>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// Crockford base32, exactly as web/public/js/model.js has it: no I, L, O or
// U, so nothing can be misread across a room and no code spells a word.
constexpr char code_alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

int64_t unix_now() {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string random_code(int length) {
    std::random_device random;
    std::string code;
    // Rejection sampling rather than a modulo: this code briefly guards a
    // real credential, and a biased alphabet would shrink the space it is
    // guarding it with.
    const unsigned alphabet = sizeof(code_alphabet) - 1;
    const unsigned limit = (256u / alphabet) * alphabet;
    while (static_cast<int>(code.size()) < length) {
        const unsigned byte = random() & 0xffu;
        if (byte >= limit) continue;
        code.push_back(code_alphabet[byte % alphabet]);
    }
    return code;
}

uint32_t parse_ipv4(const std::string& text) {
    if (text.empty()) return 0;
    in_addr address{};
    if (inet_pton(AF_INET, text.c_str(), &address) != 1) return 0;
    return address.s_addr;
}

std::string address_text(uint32_t ipv4) {
    in_addr value{};
    value.s_addr = ipv4;
    char text[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &value, text, sizeof(text))) return {};
    return text;
}

const char* presence_name(machine_presence presence) {
    switch (presence) {
    case machine_presence::in_game:     return "in_game";
    case machine_presence::unavailable: return "unavailable";
    default:                            return "available";
    }
}

const char* nat_name(nat_kind kind) {
    switch (kind) {
    case nat_kind::cone:      return "cone";
    case nat_kind::symmetric: return "symmetric";
    default:                  return "unknown";
    }
}

std::string platform_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

// A cheap fingerprint of what this build is, so two machines running
// different versions can be told apart before their boards drift rather than
// ninety seconds into a match.
std::string build_hash() {
    const std::string stamp = std::string(__DATE__) + __TIME__;
    uint64_t mixed = 1469598103934665603ull;
    for (const char character : stamp) {
        mixed ^= static_cast<unsigned char>(character);
        mixed *= 1099511628211ull;
    }
    char text[17]{};
    std::snprintf(text, sizeof(text), "%016llx",
                  static_cast<unsigned long long>(mixed));
    return text;
}

std::string hash_of(const std::vector<std::string>& list) {
    uint64_t mixed = 1469598103934665603ull;
    for (const std::string& entry : list)
        for (const char character : entry) {
            mixed ^= static_cast<unsigned char>(character);
            mixed *= 1099511628211ull;
        }
    char text[17]{};
    std::snprintf(text, sizeof(text), "%016llx",
                  static_cast<unsigned long long>(mixed));
    return text;
}

std::string host_name() {
    if (const char* forced = std::getenv("MANX_MACHINE_NAME"))
        if (*forced) return std::string(forced).substr(0, 63);
    char host[64]{};
#if defined(_WIN32)
    DWORD size = sizeof(host);
    if (GetComputerNameA(host, &size) && host[0]) return host;
#else
    if (gethostname(host, sizeof(host) - 1) == 0 && host[0]) return host;
#endif
    return "cabinet";
}

// --- the credential on disk ---------------------------------------------
// Only the refresh token, never the password. The password exists for the
// few seconds between the website writing it and this machine using it, and
// is never written down here.
struct stored_credential {
    // Chosen by this cabinet and kept for ever, so signing out and back in -
    // even as somebody else - does not strand the machine's document.
    std::string machine_id;
    std::string uid;          // the signed-in account
    std::string email;
    std::string refresh_token;
};

fs::path credential_path() {
    const fs::path root = manx_platform::config_root();
    return (root.empty() ? fs::current_path() : root) / "MANX" / "cloud.ini";
}

stored_credential load_credential() {
    stored_credential credential;
    std::ifstream input(credential_path());
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) continue;
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        if (key == "machine_id") credential.machine_id = value;
        else if (key == "uid") credential.uid = value;
        else if (key == "email") credential.email = value;
        else if (key == "refresh_token") credential.refresh_token = value;
    }
    return credential;
}

void save_credential(const stored_credential& credential) {
    const fs::path path = credential_path();
    std::error_code error;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), error);
    // Written beside the target and renamed, so an interrupted write leaves
    // the previous credential rather than half of a new one.
    const fs::path temporary = fs::path(path).concat(".part");
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return;
        output << "machine_id=" << credential.machine_id << '\n'
               << "uid=" << credential.uid << '\n'
               << "email=" << credential.email << '\n'
               << "refresh_token=" << credential.refresh_token << '\n';
        if (!output.good()) return;
    }
#if !defined(_WIN32)
    // Nobody else on this machine has any business reading it.
    ::chmod(temporary.c_str(), S_IRUSR | S_IWUSR);
#endif
    fs::rename(temporary, path, error);
}

void forget_credential() {
    std::error_code error;
    fs::remove(credential_path(), error);
}

json parse_or_empty(const std::string& text) {
    const json parsed = json::parse(text, nullptr, false);
    return parsed.is_discarded() ? json::object() : parsed;
}

std::string field_text(const json& document, const char* name) {
    const auto found = document.find(name);
    if (found == document.end() || !found->is_string()) return {};
    return found->get<std::string>();
}

// What went wrong, in words somebody can act on.
//
// Google's REST APIs put a machine-readable reason in the body, and it is
// usually the whole answer - ADMIN_ONLY_OPERATION means the Anonymous
// sign-in provider is switched off, PERMISSION_DENIED means the rules said
// no. Collapsing all of that into "cannot reach MANX online" throws away the
// only useful thing the server said.
std::string explain_failure(const manx_http::response& answer,
                            const char* doing) {
    if (answer.status == 0)
        return std::string("Cannot reach MANX online while ") + doing +
               ". Check this machine's internet connection.";

    const json body = parse_or_empty(answer.body);
    std::string reason;
    if (const auto error = body.find("error"); error != body.end()) {
        if (error->is_object()) {
            reason = field_text(*error, "message");
            if (reason.empty()) reason = field_text(*error, "status");
        } else if (error->is_string()) {
            reason = error->get<std::string>();
        }
    }

    if (reason.rfind("ADMIN_ONLY_OPERATION", 0) == 0)
        return "Anonymous sign-in is switched off for this Firebase "
               "project. Enable it under Authentication, Sign-in method.";
    if (reason.rfind("PERMISSION_DENIED", 0) == 0 || answer.status == 403)
        return "The database rules refused this. Have they been deployed "
               "(npm run rules)?";
    if (answer.status == 429)
        return "MANX online has used up today's free quota.";
    if (reason.empty())
        reason = "HTTP " + std::to_string(answer.status);
    return std::string("Could not ") + doing + ": " + reason;
}

} // namespace

// ---------------------------------------------------------------------------

online_link::online_link(multiplayer_lobby& lobby) : m_lobby(lobby) {
    if (!available()) {
        m_state.store(online_state::disabled);
        m_status = manx_http::available()
                       ? "This build is not pointed at a Firebase project."
                       : "This build has no network support (no libcurl).";
        return;
    }
    m_state.store(online_state::signed_out);
    m_status = "Not signed in.";
    m_thread = std::thread(&online_link::run, this);
    m_events = std::thread(&online_link::watch_events, this);
}

online_link::~online_link() {
    m_stop.store(true);
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
    if (m_events.joinable()) m_events.join();
}

bool online_link::available() {
    return manx_http::available() && manx_cloud::configured();
}

void online_link::publish(online_state state, std::string status) {
    const online_state was = m_state.exchange(state);
    // Narrated to the terminal as well as the screen. The screen is on a
    // cabinet that may be across the room, in a menu somebody has already
    // navigated away from, and it keeps no history - so when signing in
    // fails the one place the reason survives is here.
    if (was != state || state == online_state::error)
        std::printf("MANX online: %s\n", status.c_str());
    std::fflush(stdout);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // An answer to something somebody has just asked outlives the next
        // routine heartbeat. Otherwise "nobody online is called that" is
        // replaced by "Signed in." a few seconds later and the question
        // reads as though it was never answered. A state that has actually
        // changed still wins: that is news, not noise.
        const int64_t said = m_announced_unix.load();
        if (was == state && said != 0 && unix_now() - said < 20) return;
        m_status = std::move(status);
    }
    touch();
}

void online_link::announce(std::string status) {
    std::printf("MANX online: %s\n", status.c_str());
    std::fflush(stdout);
    m_announced_unix.store(unix_now());
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status = std::move(status);
    }
    touch();
}

void online_link::touch() { m_revision.fetch_add(1); }

void online_link::register_account(std::string display_name,
                                   std::string email, std::string password,
                                   bool remember) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::register_account, std::move(email),
                          std::move(password), std::move(display_name), 0,
                          remember});
    m_wake.notify_all();
}

void online_link::sign_in(std::string email, std::string password,
                          bool remember) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::sign_in, std::move(email),
                          std::move(password), {}, 0, remember});
    m_wake.notify_all();
}

std::string online_link::display_name() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_display_name;
}

void online_link::sign_out() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::sign_out, {}, {}, {}, 0, false});
    m_wake.notify_all();
}

void online_link::forget_machine() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::forget, {}, {}, {}, 0, false});
    m_wake.notify_all();
}

void online_link::create_lobby(bool open_to_anyone,
                               std::string game_short_name,
                               std::string game_display_name, int places,
                               std::string mode) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::host, std::move(game_short_name),
                          std::move(mode), std::move(game_display_name),
                          places, open_to_anyone});
    m_wake.notify_all();
}

void online_link::start_lobby() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::start, {}, {}, {}, 0, false});
    m_wake.notify_all();
}

void online_link::refresh_lobbies() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::browse, {}, {}, {}, 0, false});
    m_wake.notify_all();
}

std::vector<online_lobby> online_link::lobbies() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lobbies;
}

std::optional<online_lobby> online_link::take_new_lobby() {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::optional<online_lobby> found;
    found.swap(m_new_lobby);
    return found;
}

void online_link::refresh_friends() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::friends, {}, {}, {}, 0, false});
    m_wake.notify_all();
}

std::vector<online_friend> online_link::friends() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_friends;
}

void online_link::add_friend(std::string name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::befriend, std::move(name), {}, {},
                          0, false});
    m_wake.notify_all();
}

void online_link::answer_friend(std::string uid, bool accept) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::answer_friend, std::move(uid), {},
                          {}, 0, accept});
    m_wake.notify_all();
}

void online_link::join_lobby(std::string lobby_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::join, std::move(lobby_id), {}, {},
                          0, false});
    m_wake.notify_all();
}

void online_link::leave_lobby() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::leave, {}, {}, {}, 0, false});
    m_wake.notify_all();
}

void online_link::set_installed_games(std::vector<std::string> short_names) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (short_names == m_games) return;
    m_games = std::move(short_names);
    m_games_dirty = true;
    m_wake.notify_all();
}

void online_link::set_foreground(bool foreground) {
    if (m_foreground.exchange(foreground) == foreground) return;
    m_wake.notify_all();
}

std::string online_link::account_email() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_email;
}

bool online_link::remembered() const {
    return m_remembered.load();
}

bool online_link::signed_in() const {
    const online_state state = m_state.load();
    return state == online_state::online || state == online_state::in_lobby;
}

std::string online_link::machine_name() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_machine_name;
}

std::string online_link::joined_lobby() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lobby_id;
}

std::vector<online_wire::member> online_link::members() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_members;
}

std::string online_link::lobby_game() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lobby_game;
}

std::string online_link::lobby_game_name() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lobby_game_name;
}

int online_link::lobby_places() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lobby_places;
}

bool online_link::hosting_lobby() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lobby_host;
}

bool online_link::lobby_starting() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lobby_starting;
}

int online_link::lobby_port_base() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_port_base;
}

std::string online_link::peer_link_address() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const online_wire::member& member : m_members) {
        if (member.machine_uid == m_uid) continue;
        // The address on this network first. Two cabinets in one house reach
        // each other directly on it, and the public one between them is the
        // router's own outside face - which is the address that does not
        // work.
        if (!member.lan_ip.empty()) return member.lan_ip;
        if (!member.public_ip.empty()) return member.public_ip;
    }
    return {};
}

online_state online_link::state() const { return m_state.load(); }

std::string online_link::status_text() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

uint64_t online_link::revision() const { return m_revision.load(); }

// ---------------------------------------------------------------------------
// The lobby service.
//
// Everything about a lobby - who is in it, where they are, what is being
// played, and the numbers both boards must agree on before either exists -
// is settled here rather than by writing documents at each other. The
// service is the one participant both cabinets can hear before they can
// hear each other, which is what a handshake needs and what a database
// cannot be.

namespace {

// One member of a lobby, as the service describes it.
online_wire::member member_from_service(const json& entry) {
    online_wire::member member;
    const auto text = [&entry](const char* name) -> std::string {
        const auto found = entry.find(name);
        return found != entry.end() && found->is_string()
                   ? found->get<std::string>() : std::string();
    };
    const auto number = [&entry](const char* name) -> int64_t {
        const auto found = entry.find(name);
        return found != entry.end() && found->is_number()
                   ? found->get<int64_t>() : 0;
    };
    member.machine_uid = text("uid");
    member.owner_uid = text("uid");
    member.name = text("name");
    member.node = static_cast<int>(number("node"));
    member.public_ip = text("publicIp");
    member.public_port = static_cast<uint16_t>(number("publicPort"));
    member.lan_ip = text("lanIp");
    member.lan_port = static_cast<uint16_t>(number("lanPort"));
    const auto has = entry.find("hasGame");
    member.has_game = has != entry.end() && has->is_boolean()
                          ? has->get<bool>() : true;
    member.rtt_ms = static_cast<int>(number("rttMs"));
    member.ready = true;
    member.link = "connected";
    member.updated_unix = unix_now();
    return member;
}

online_lobby lobby_from_service(const json& entry) {
    online_lobby lobby;
    const auto text = [&entry](const char* name) -> std::string {
        const auto found = entry.find(name);
        return found != entry.end() && found->is_string()
                   ? found->get<std::string>() : std::string();
    };
    const auto number = [&entry](const char* name) -> int {
        const auto found = entry.find(name);
        return found != entry.end() && found->is_number()
                   ? found->get<int>() : 0;
    };
    lobby.id = text("id");
    lobby.host = text("host");
    lobby.game = text("gameName").empty() ? text("game") : text("gameName");
    lobby.members = number("members");
    lobby.places = number("places");
    const auto open = entry.find("open");
    lobby.open_to_anyone = open != entry.end() && open->is_boolean()
                               ? open->get<bool>() : true;
    return lobby;
}

} // namespace

void online_link::watch_events() {
    int64_t quiet_until = 0;
    while (!m_stop.load()) {
        std::string session;
        int64_t since = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            session = m_session;
            since = m_event_seq;
        }
        if (session.empty() || unix_now() < quiet_until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            continue;
        }

        manx_http::request ask;
        ask.verb = manx_http::method::get;
        ask.url = manx_cloud::lobby_service() + "/api/events?since=" +
                  std::to_string(since);
        ask.headers.push_back("X-MANX-Session: " + session);
        // Longer than the service's own wait, so the answer that arrives is
        // the service saying "nothing yet" rather than this end giving up on
        // a request that was about to be answered.
        ask.timeout_seconds = 40;
        ask.cancel = &m_stop;
        const manx_http::response answer = manx_http::perform(ask);
        if (m_stop.load()) break;

        if (answer.status == 401) {
            // The session has gone - the service restarted, or this cabinet
            // was quiet long enough to be swept. The worker opens a new one
            // on its next turn round.
            std::lock_guard<std::mutex> lock(m_mutex);
            m_session.clear();
            m_event_seq = 0;
            continue;
        }
        if (!answer.ok()) {
            // Backed off rather than hammered: an unreachable service is a
            // deployment being restarted, not something to spin against.
            quiet_until = unix_now() + 5;
            continue;
        }

        const json body = parse_or_empty(answer.body);
        const auto events = body.find("events");
        if (events == body.end() || !events->is_array()) continue;
        for (const json& event : *events) {
            const auto seq = event.find("seq");
            if (seq != event.end() && seq->is_number()) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_event_seq = std::max(m_event_seq, seq->get<int64_t>());
            }
            const auto kind = event.find("type");
            const std::string type =
                kind != event.end() && kind->is_string()
                    ? kind->get<std::string>() : std::string();
            if (type == "ended") {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lobby_id.clear();
                m_members.clear();
                m_lobby_game.clear();
                m_lobby_game_name.clear();
                m_lobby_starting = false;
                m_lobby_host = false;
                continue;
            }
            const auto detail = event.find("lobby");
            if (detail == event.end() || !detail->is_object()) continue;
            apply_lobby(*detail, type == "go");
        }
        touch();
    }
}

// One place where a lobby the service describes becomes what the launcher
// draws and what the lobby thread punches at. Called from both threads, so
// it takes the lock itself and does the peer handover outside it.
void online_link::apply_lobby(const json& detail, bool going) {
    const online_lobby summary = lobby_from_service(detail);
    std::vector<online_wire::member> members;
    const auto list = detail.find("members");
    if (list != detail.end() && list->is_array())
        for (const json& entry : *list)
            members.push_back(member_from_service(entry));

    std::string me;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        me = m_uid;
        m_lobby_id = summary.id;
        m_lobby_game = detail.value("game", std::string());
        m_lobby_game_name = detail.value("gameName", m_lobby_game);
        m_lobby_places = summary.places > 0 ? summary.places : 2;
        m_lobby_host = detail.value("hostUid", std::string()) == m_uid;
        const std::string state = detail.value("state", std::string("open"));
        m_lobby_starting = going || state == "starting" || state == "running";
        m_members = members;
        m_port_base = detail.value("portBase", 0);
        m_seed = detail.value("seed", 0);
        m_delay_frames = detail.value("delayFrames", 3);
    }

    // The handover. Every address the service knows about, both candidates
    // for each machine, handed to the lobby thread - which from here treats
    // a machine in another country exactly as it treats one in the room.
    const std::vector<lobby_link::remote_peer> peers =
        online_wire::peers_from_members(members, me, unix_now(), parse_ipv4);
    m_lobby.set_remote_peers(peers);
    publish(online_state::in_lobby,
            peers.empty() ? "In a lobby. Waiting for another machine."
                          : "In a lobby. Connecting to " +
                                std::to_string(peers.size()) + " address(es).");
}

// ---------------------------------------------------------------------------
// The worker. Everything below this line runs on m_thread and nowhere else.

void online_link::run() {
    online_wire::token_state token;
    stored_credential credential = load_credential();
    // Remembered across restarts, and across signing out: the address is not
    // a secret and retyping it on a cabinet is exactly the friction this
    // whole screen exists to avoid.
    if (!credential.email.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_email = credential.email;
    }
    // A token on disk means this cabinet has an account and is about to use
    // it. Anything asking "does this machine need to sign in?" wants this
    // answer, not signed_in() - which is false for the first few seconds of
    // every start, on a machine that has been signed in for months.
    m_remembered.store(!credential.refresh_token.empty());
    bool pending_auth = false;
    bool pending_create = false;
    std::string pending_email;
    std::string pending_password;
    std::string pending_name;
    bool profile_ready = false;
    bool machine_ready = false;
    bool pending_host = false;
    bool pending_host_open = false;
    bool pending_start = false;
    bool pending_leave = false;
    std::string pending_join;
    std::string pending_host_game;
    std::string pending_host_title;
    std::string pending_host_mode = "simultaneous";
    int pending_host_places = 2;
    bool pending_browse = false;
    bool pending_friends = false;
    std::string pending_befriend;
    std::vector<std::pair<std::string, bool>> pending_answers;
    // Lobbies this cabinet has already offered to join. Primed - silently -
    // by the first sweep after signing in, because a lobby that has been
    // sitting there for an hour is something to find in the join list, not
    // something to be interrupted about the moment the launcher opens.
    std::set<std::string> announced_lobbies;
    bool lobby_watch_primed = false;
    bool was_foreground = false;
    int64_t next_lobby_watch = 0;
    int64_t next_friend_poll = 0;
    int64_t next_session_try = 0;
    bool pending_remember = true;
    std::string published_games_hash;
    std::string current_lobby;
    int64_t next_machine_poll = 0;
    int64_t next_lobby_poll = 0;
    int64_t next_heartbeat = 0;
    int64_t backoff_until = 0;
    int64_t backoff_seconds = 5;
    int64_t last_command_seq = 0;
    std::string machine_owner_uid;
    std::string lobby_game;

    const auto sleep_a_moment = [this](int milliseconds) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_wake.wait_for(lock, std::chrono::milliseconds(milliseconds),
                        [this] {
                            return m_stop.load() || !m_commands.empty();
                        });
    };

    // Every request goes through here, so the token refresh, the 401 dance
    // and the cancel flag are written once rather than at every call site.
    const auto call = [&](manx_http::method verb, const std::string& url,
                          const std::string& body,
                          bool authorised) -> manx_http::response {
        manx_http::request request;
        request.verb = verb;
        request.url = url;
        request.body = body;
        request.cancel = &m_stop;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (authorised) request.bearer = token.id_token;
            manx_http::response answer = manx_http::perform(request);
            if (answer.status != 401 || !authorised) return answer;
            if (online_wire::next_after_unauthorized(attempt) ==
                online_wire::auth_decision::sign_out) {
                token = {};
                return answer;
            }
            // Refresh once and try the same request again. A token can
            // expire between being checked and being used.
            if (token.refresh_token.empty()) return answer;
            const manx_http::response refreshed = manx_http::perform([&] {
                manx_http::request refresh;
                refresh.verb = manx_http::method::post;
                refresh.url = manx_cloud::refresh_url();
                // The one Firebase endpoint that is not JSON in.
                refresh.content_type = "application/x-www-form-urlencoded";
                refresh.body = "grant_type=refresh_token&refresh_token=" +
                               token.refresh_token;
                refresh.cancel = &m_stop;
                return refresh;
            }());
            if (!refreshed.ok()) { token = {}; return answer; }
            const json body_json = parse_or_empty(refreshed.body);
            token.id_token = field_text(body_json, "id_token");
            const std::string rotated = field_text(body_json, "refresh_token");
            if (!rotated.empty()) token.refresh_token = rotated;
            token.expires_unix = online_wire::expiry_from_expires_in(
                unix_now(), field_text(body_json, "expires_in"));
        }
        return {};
    };

    const auto document_url = [](const std::string& path) {
        return manx_cloud::documents_root() + "/" + path;
    };

    // --- the lobby service ----------------------------------------------
    // One call shape for everything the cabinet asks of it. The session is
    // the credential; the Firebase token is only ever shown once, to get
    // one.
    const auto service = [&](const char* path, const json& body,
                             bool posting = true) -> json {
        std::string session;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            session = m_session;
        }
        if (session.empty()) return json::object();
        manx_http::request request;
        request.verb = posting ? manx_http::method::post
                               : manx_http::method::get;
        request.url = manx_cloud::lobby_service() + path;
        if (posting) request.body = body.dump();
        request.headers.push_back("X-MANX-Session: " + session);
        request.cancel = &m_stop;
        const manx_http::response answer = manx_http::perform(request);
        if (answer.status == 401) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_session.clear();
            m_event_seq = 0;
            return json::object();
        }
        const json parsed = parse_or_empty(answer.body);
        if (!answer.ok()) {
            const auto why = parsed.find("error");
            announce(why != parsed.end() && why->is_string()
                         ? why->get<std::string>()
                         : "The lobby service is not answering.");
            return json::object();
        }
        return parsed;
    };

    // Presenting the Firebase token to get a session. This is the only place
    // the token goes anywhere other than Google, and it is the whole of what
    // Firebase is still doing for lobbies: saying who this is.
    const auto open_session = [&](const std::string& name) {
        json request;
        request["idToken"] = token.id_token;
        request["name"] = name;
        manx_http::request post;
        post.verb = manx_http::method::post;
        post.url = manx_cloud::lobby_service() + "/api/session";
        post.body = request.dump();
        post.cancel = &m_stop;
        const manx_http::response answer = manx_http::perform(post);
        const json body = parse_or_empty(answer.body);
        if (!answer.ok()) {
            // A refusal with a reason is worth repeating: "MANX online is
            // full" is something a player can understand and wait out, and
            // it is otherwise indistinguishable from the cabinet quietly
            // failing to reach anything.
            const auto why = body.find("error");
            if (why != body.end() && why->is_string())
                announce(why->get<std::string>());
            return false;
        }
        const auto id = body.find("session");
        if (id == body.end() || !id->is_string()) return false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_session = id->get<std::string>();
            m_uid = body.value("uid", std::string());
            m_event_seq = 0;
        }
        std::printf("MANX online: lobby service session opened\n");
        return true;
    };

    // Firestore's queries are a different endpoint and a different shape
    // from fetching one document: a POST to :runQuery, answered with an
    // array of {document: ...} whose rows sometimes carry no document at all
    // - keep-alives, to be skipped rather than parsed.
    const auto run_query = [&](const json& structured) {
        std::vector<json> found;
        const manx_http::response answer = call(
            manx_http::method::post,
            manx_cloud::documents_root() + ":runQuery",
            json{{"structuredQuery", structured}}.dump(), true);
        if (!answer.ok()) return found;
        const json rows = parse_or_empty(answer.body);
        if (!rows.is_array()) return found;
        for (const json& row : rows) {
            const auto document = row.find("document");
            if (document != row.end() && document->is_object())
                found.push_back(*document);
        }
        return found;
    };

    // Every friendship this account is in - agreed, asked for, or waiting to
    // be answered - read from the cabinet. The website is an admin view of
    // what is going on, not how anybody uses this.
    const auto fetch_friendships = [&](const std::string& me) {
        std::vector<online_friend> people;
        json where;
        where["fieldFilter"]["field"]["fieldPath"] = "members";
        where["fieldFilter"]["op"] = "ARRAY_CONTAINS";
        where["fieldFilter"]["value"]["stringValue"] = me;
        json structured;
        structured["from"] = json::array({{{"collectionId", "friendships"}}});
        structured["where"] = where;
        structured["limit"] = 100;
        for (const json& document : run_query(structured)) {
            const auto* state = online_wire::find_field(document, "state");
            const std::string answer =
                state ? online_wire::read_string(*state) : std::string();
            // Declined and blocked are answers. Keeping them on the screen
            // would make saying no to somebody a thing you had to keep
            // saying.
            if (answer != "accepted" && answer != "pending") continue;
            const auto* members = online_wire::find_field(document, "members");
            if (!members) continue;
            const auto* asked_by =
                online_wire::find_field(document, "requesterUid");
            const std::string requester =
                asked_by ? online_wire::read_string(*asked_by) : std::string();
            for (const std::string& uid : online_wire::read_strings(*members)) {
                if (uid == me || uid.empty()) continue;
                online_friend person;
                person.uid = uid;
                person.accepted = answer == "accepted";
                person.incoming = !person.accepted && requester == uid;
                people.push_back(std::move(person));
            }
        }
        return people;
    };

    // A name and a light, for somebody who is otherwise a uid. Cheap enough
    // to do on every friends poll: a profile read is one document, and the
    // poll is measured in tens of seconds.
    const auto decorate = [&](std::vector<online_friend>& people) {
        // One blocking request each, on this thread. Somebody with two
        // hundred friends would otherwise spend the whole poll interval
        // fetching names, so the tail keeps its uid and says so rather than
        // quietly looking like a shorter list.
        constexpr std::size_t most_we_will_ask_about = 64;
        if (people.size() > most_we_will_ask_about) {
            std::printf("MANX online: showing the first %zu of %zu friends.\n",
                        most_we_will_ask_about, people.size());
            people.resize(most_we_will_ask_about);
        }
        for (online_friend& person : people) {
            const manx_http::response answer = call(
                manx_http::method::get, document_url("profiles/" + person.uid),
                {}, true);
            if (!answer.ok()) continue;
            const json profile = parse_or_empty(answer.body);
            if (const auto* name =
                    online_wire::find_field(profile, "displayName"))
                person.name = online_wire::read_string(*name);
            // Their own client sets `online` and stops updating lastSeen the
            // moment it is closed, so both have to agree before this says
            // somebody is about.
            bool flagged = false;
            if (const auto* about = online_wire::find_field(profile, "online"))
                flagged = online_wire::read_bool(*about);
            int64_t seen = 0;
            if (const auto* when =
                    online_wire::find_field(profile, "lastSeen"))
                seen = online_wire::read_time(*when);
            person.online = flagged && seen != 0 && unix_now() - seen < 300;
        }
    };

    const auto reload_friends = [&](const std::string& me) {
        std::vector<online_friend> people = fetch_friendships(me);
        decorate(people);
        std::sort(people.begin(), people.end(),
                  [](const online_friend& left, const online_friend& right) {
                      // The ones waiting on an answer first, then whoever is
                      // about, then alphabetically. A request nobody notices
                      // is a request nobody answers.
                      if (left.incoming != right.incoming) return left.incoming;
                      if (left.online != right.online) return left.online;
                      return left.name < right.name;
                  });
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_friends = std::move(people);
        }
        touch();
    };

    while (!m_stop.load()) {
        // --- commands from the launcher --------------------------------
        std::deque<command> queued;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            queued.swap(m_commands);
        }
        for (const command& entry : queued) {
            switch (entry.what) {
            case command::kind::register_account:
            case command::kind::sign_in:
                pending_email = entry.argument;
                pending_password = entry.secret;
                pending_name = entry.extra;
                pending_create = entry.what == command::kind::register_account;
                pending_remember = entry.flag;
                pending_auth = true;
                publish(online_state::registering,
                        pending_create ? "Creating your account..."
                                       : "Signing in...");
                break;
            case command::kind::sign_out:
                // Turn the light off on the way out, while there is still a
                // token to do it with. Otherwise a friend sees somebody as
                // online for five minutes after they left.
                if (profile_ready && !credential.uid.empty()) {
                    json profile;
                    profile["fields"]["online"] = online_wire::value_bool(false);
                    profile["fields"]["lastSeen"] =
                        online_wire::value_time(unix_now());
                    call(manx_http::method::patch,
                         document_url("profiles/" + credential.uid) +
                             "?updateMask.fieldPaths=online"
                             "&updateMask.fieldPaths=lastSeen",
                         profile.dump(), true);
                }
                // The address stays so the next sign-in is a password and
                // nothing else; only the token that grants access is
                // destroyed. "Forget this machine" is what wipes it all.
                credential.uid.clear();
                credential.refresh_token.clear();
                save_credential(credential);
                m_remembered.store(false);
                token = {};
                profile_ready = false;
                machine_ready = false;
                current_lobby.clear();
                m_lobby.set_remote_peers({});
                announced_lobbies.clear();
                lobby_watch_primed = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_lobby_id.clear();
                    m_members.clear();
                    m_friends.clear();
                    m_new_lobby.reset();
                    m_lobby_game.clear();
                    m_lobby_game_name.clear();
                    m_lobby_places = 0;
                    m_lobby_host = false;
                    m_lobby_starting = false;
                }
                publish(online_state::signed_out, "Signed out.");
                break;
            case command::kind::forget:
                forget_credential();
                credential = {};
                token = {};
                profile_ready = false;
                machine_ready = false;
                announced_lobbies.clear();
                lobby_watch_primed = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_email.clear();
                    m_display_name.clear();
                    m_friends.clear();
                    m_new_lobby.reset();
                }
                publish(online_state::signed_out,
                        "This machine has forgotten everything.");
                break;
            case command::kind::browse:
                pending_browse = true;
                break;
            case command::kind::friends:
                pending_friends = true;
                break;
            case command::kind::befriend:
                pending_befriend = entry.argument;
                break;
            case command::kind::answer_friend:
                pending_answers.emplace_back(entry.argument, entry.flag);
                break;
            case command::kind::host:
                pending_host = true;
                pending_host_open = entry.flag;
                pending_host_game = entry.argument;
                pending_host_title = entry.extra;
                pending_host_places = entry.number >= 2 ? entry.number : 2;
                if (!entry.secret.empty()) pending_host_mode = entry.secret;
                break;
            case command::kind::start:
                pending_start = true;
                break;
            case command::kind::join:
                pending_join = entry.argument;
                break;
            case command::kind::leave:
                // One call. The service removes this machine, tells the
                // others, and closes the lobby if this was its host - none
                // of which this cabinet has to reason about any more.
                pending_leave = true;
                break;
            }
        }

        const int64_t now = unix_now();
        // How lively this cabinet is, in three steps rather than two.
        //
        // "Not looking at an online screen" was being treated as "nobody
        // cares", and it is not: a cabinet sitting on the shelf is a cabinet
        // whose owner is standing in front of it, and a lobby somebody opens
        // then took up to a minute and a half to appear. The case that
        // genuinely needs to be cheap is a game in progress, where nobody
        // can act on a lobby anyway - so that is the one that backs off.
        const bool watching = m_foreground.load();
        const bool playing =
            m_lobby.presence() == machine_presence::in_game;
        const auto pace = [watching, playing](int64_t open, int64_t shelf,
                                              int64_t in_game) {
            return watching ? open : (playing ? in_game : shelf);
        };
        // Somebody has just opened a screen that shows friends or lobbies.
        // Whatever is on it should be what is true now, not what was true up
        // to ten minutes ago.
        if (const bool foreground = m_foreground.load();
            foreground != was_foreground) {
            was_foreground = foreground;
            if (foreground) {
                next_friend_poll = 0;
                next_lobby_watch = 0;
            }
        }
        if (now < backoff_until) { sleep_a_moment(500); continue; }

        // --- signing in, or creating an account -------------------------
        if (pending_auth) {
            pending_auth = false;
            const bool creating = pending_create;
            const json request{{"email", pending_email},
                               {"password", pending_password},
                               {"returnSecureToken", true}};
            const manx_http::response answer = call(
                manx_http::method::post,
                manx_cloud::identity_url(creating ? "signUp"
                                                  : "signInWithPassword"),
                request.dump(), false);
            pending_password.clear();
            if (!answer.ok()) {
                publish(online_state::error,
                        explain_failure(answer, creating
                                                    ? "creating your account"
                                                    : "signing in"));
                continue;
            }
            const json body = parse_or_empty(answer.body);
            token.id_token = field_text(body, "idToken");
            token.refresh_token = field_text(body, "refreshToken");
            token.expires_unix = online_wire::expiry_from_expires_in(
                now, field_text(body, "expiresIn"));
            credential.uid = field_text(body, "localId");
            credential.email = pending_email;
            // Only written to disk if this cabinet was told to remember it.
            // The address is kept either way: it is not a secret, and it is
            // the thing nobody wants to retype.
            credential.refresh_token =
                pending_remember ? token.refresh_token : std::string();
            m_remembered.store(!credential.refresh_token.empty());
            // The machine keeps its own id for ever, so signing out and back
            // in - even as somebody else - does not orphan its document.
            if (credential.machine_id.empty())
                credential.machine_id = random_code(20);
            save_credential(credential);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_email = credential.email;
                if (!pending_name.empty()) m_display_name = pending_name;
            }
            profile_ready = false;
            machine_ready = false;
            publish(online_state::online,
                    creating ? "Account created." : "Signed in.");
        }

        // --- signed in? --------------------------------------------------
        if (credential.refresh_token.empty()) {
            if (m_state.load() != online_state::signed_out &&
                m_state.load() != online_state::error &&
                m_state.load() != online_state::registering)
                publish(online_state::signed_out, "Not signed in.");
            sleep_a_moment(1000);
            continue;
        }
        if (!token.usable(now) || token.needs_refresh(now)) {
            if (token.refresh_token.empty())
                token.refresh_token = credential.refresh_token;

            // Ask for a new token, plainly.
            //
            // This used to send a request with no Authorization header at all
            // and rely on the 401 to drive the refresh inside `call`. But a
            // Firestore request with no credential is answered 403, not 401 -
            // so the refresh never happened, the token stayed empty, and the
            // branch below deleted the saved login and announced that the
            // machine was no longer paired. Signing in was remembered
            // perfectly and thrown away on every start.
            manx_http::request renew;
            renew.verb = manx_http::method::post;
            renew.url = manx_cloud::refresh_url();
            renew.content_type = "application/x-www-form-urlencoded";
            renew.body = "grant_type=refresh_token&refresh_token=" +
                         token.refresh_token;
            renew.cancel = &m_stop;
            const manx_http::response answer = manx_http::perform(renew);
            if (answer.ok()) {
                const json body = parse_or_empty(answer.body);
                token.id_token = field_text(body, "id_token");
                const std::string rotated = field_text(body, "refresh_token");
                if (!rotated.empty()) {
                    token.refresh_token = rotated;
                    if (!credential.refresh_token.empty()) {
                        credential.refresh_token = rotated;
                        save_credential(credential);
                    }
                }
                token.expires_unix = online_wire::expiry_from_expires_in(
                    now, field_text(body, "expires_in"));
            }
            if (token.id_token.empty()) {
                // Only a definite answer from the token service means the
                // credential is dead. Anything else - no network, a hiccup,
                // a 500 - is a reason to wait, not to throw away a login
                // that is probably fine.
                const bool refused = answer.status >= 400 && answer.status < 500;
                if (!refused) {
                    publish(online_state::error,
                            "No answer from MANX online. Trying again.");
                    backoff_until = now + backoff_seconds;
                    backoff_seconds = std::min<int64_t>(backoff_seconds * 2, 60);
                } else {
                    credential.uid.clear();
                    credential.refresh_token.clear();
                    save_credential(credential);   // keeps the email
                    publish(online_state::signed_out,
                            "Signed out - please sign in again.");
                }
                continue;
            }
            backoff_seconds = 5;
        }

        // --- this machine's own document ---------------------------------
        // Written in full the first time, because the rules validate a
        // creation as a whole: a heartbeat's handful of fields is an update,
        // and an update of a document that does not exist yet is refused.
        if (!machine_ready) {
            std::vector<std::string> mine;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                mine = m_games;
            }
            json document;
            auto& fields = document["fields"];
            fields["ownerUid"] = online_wire::value_string(credential.uid);
            fields["machineUid"] = online_wire::value_string(credential.machine_id);
            fields["name"] = online_wire::value_string(host_name());
            fields["hostName"] = online_wire::value_string(host_name());
            fields["platform"] = online_wire::value_string(platform_name());
            fields["buildHash"] = online_wire::value_string(build_hash());
            fields["presence"] = online_wire::value_string(
                presence_name(m_lobby.presence()));
            fields["lastSeen"] = online_wire::value_time(now);
            fields["games"] = online_wire::value_strings(mine);
            fields["gamesHash"] = online_wire::value_string(hash_of(mine));
            const auto lan_self = m_lobby.local_endpoint();
            fields["lanIp"] = online_wire::value_string(
                lan_self ? address_text(lan_self->ipv4) : std::string());
            fields["lanPort"] =
                online_wire::value_int(lan_self ? lan_self->port : 0);
            fields["publicIp"] = online_wire::value_string(std::string());
            fields["publicPort"] = online_wire::value_int(0);
            fields["natKind"] = online_wire::value_string("unknown");
            fields["stunAt"] = online_wire::value_time(now);
            fields["diagnostics"] = online_wire::value_map(json::object());
            fields["readerUids"] = online_wire::value_strings({credential.uid});
            fields["commandAck"] = online_wire::value_int(0);
            fields["currentLobbyId"] = online_wire::value_string(std::string());
            const manx_http::response written = call(
                manx_http::method::patch,
                document_url("machines/" + credential.machine_id),
                document.dump(), true);
            if (written.ok()) {
                machine_ready = true;
                published_games_hash.clear();   // force a first heartbeat
                std::printf("MANX online: this machine is registered as %s\n",
                            credential.machine_id.c_str());
                // Say so here rather than waiting for the first heartbeat to
                // be written. A cabinet that signed itself back in on start-
                // up was signed in, registered and working while the corner
                // of the screen still read OFFLINE, because nothing on that
                // path ever published the state.
                if (m_state.load() != online_state::in_lobby) {
                    std::string who;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        who = m_email;
                    }
                    publish(online_state::online,
                            who.empty() ? std::string("Signed in.")
                                        : "Signed in as " + who);
                }
            } else {
                publish(online_state::error,
                        explain_failure(written, "registering this machine"));
                backoff_until = now + backoff_seconds;
                backoff_seconds = std::min<int64_t>(backoff_seconds * 2, 60);
                continue;
            }
        }

        // --- who this account is, to everybody else -----------------------
        // A cabinet that creates an account has to publish a profile too, or
        // the person using it is a bare uid to their own friends and cannot
        // be found by name at all. handles/{name} is create-only - the
        // document existing IS the uniqueness constraint - so claiming one is
        // a write that is expected to fail when the name is taken, and
        // failing it is not an error: the account works, it is just not
        // findable by that name.
        if (!profile_ready && !credential.uid.empty()) {
            std::string name;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                name = m_display_name;
            }
            std::string claimed;
            const manx_http::response existing =
                call(manx_http::method::get,
                     document_url("profiles/" + credential.uid), {}, true);
            if (existing.ok()) {
                const json profile = parse_or_empty(existing.body);
                // A cabinet that signed itself back in from disk knows an
                // email address and nothing else, so the name comes back
                // from the profile rather than being asked for again.
                if (const auto* was =
                        online_wire::find_field(profile, "displayName"))
                    if (name.empty()) name = online_wire::read_string(*was);
                if (const auto* had =
                        online_wire::find_field(profile, "handle"))
                    claimed = online_wire::read_string(*had);
            }
            if (name.empty())
                name = credential.email.substr(0, credential.email.find('@'));
            if (name.empty()) name = host_name();
            name = name.substr(0, 32);

            const std::string wanted = online_wire::handle_for(name);
            if (claimed.empty() && !wanted.empty()) {
                const manx_http::response held =
                    call(manx_http::method::get,
                         document_url("handles/" + wanted), {}, true);
                if (held.ok()) {
                    const json handle = parse_or_empty(held.body);
                    if (const auto* owner =
                            online_wire::find_field(handle, "uid"))
                        if (online_wire::read_string(*owner) == credential.uid)
                            claimed = wanted;
                } else {
                    json document;
                    document["fields"]["uid"] =
                        online_wire::value_string(credential.uid);
                    document["fields"]["displayName"] =
                        online_wire::value_string(name);
                    const manx_http::response made = call(
                        manx_http::method::patch,
                        document_url("handles/" + wanted) +
                            "?currentDocument.exists=false",
                        document.dump(), true);
                    if (made.ok()) claimed = wanted;
                }
                if (claimed.empty())
                    std::printf("MANX online: the name \"%s\" is already "
                                "taken, so friends cannot search for it.\n",
                                name.c_str());
            }

            json profile;
            profile["fields"]["displayName"] = online_wire::value_string(name);
            profile["fields"]["handle"] = online_wire::value_string(claimed);
            profile["fields"]["online"] = online_wire::value_bool(true);
            profile["fields"]["lastSeen"] = online_wire::value_time(now);
            const manx_http::response written =
                call(manx_http::method::patch,
                     document_url("profiles/" + credential.uid),
                     profile.dump(), true);
            if (written.ok()) {
                profile_ready = true;
                next_friend_poll = 0;
                std::lock_guard<std::mutex> lock(m_mutex);
                m_display_name = name;
            }
        }

        // --- a session with the lobby service ------------------------------
        // Opened once the token is good, and reopened whenever the service
        // says the old one has gone - which happens when it is redeployed,
        // and must not need the player to do anything.
        {
            bool have_session = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                have_session = !m_session.empty();
            }
            if (!have_session && !token.id_token.empty() &&
                now >= next_session_try) {
                next_session_try = now + 10;
                std::string name;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    name = m_display_name.empty() ? m_email : m_display_name;
                }
                if (open_session(name)) pending_browse = true;
            }
        }

        // --- publish what this machine is --------------------------------
        std::vector<std::string> games;
        bool games_dirty = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            games = m_games;
            games_dirty = m_games_dirty;
            m_games_dirty = false;
        }
        const std::string games_hash = hash_of(games);
        const bool heartbeat_due = now >= next_heartbeat;
        if (heartbeat_due || games_dirty || games_hash != published_games_hash) {
            m_lobby.begin_public_endpoint_probe();
            const auto endpoint = m_lobby.public_endpoint();

            json document;
            auto& fields = document["fields"];
            fields["hostName"] = online_wire::value_string(host_name());
            fields["platform"] = online_wire::value_string(platform_name());
            fields["buildHash"] = online_wire::value_string(build_hash());
            fields["presence"] =
                online_wire::value_string(presence_name(m_lobby.presence()));
            fields["lastSeen"] = online_wire::value_time(now);
            fields["games"] = online_wire::value_strings(games);
            fields["gamesHash"] = online_wire::value_string(games_hash);
            fields["publicIp"] = online_wire::value_string(
                endpoint ? address_text(endpoint->ipv4) : std::string());
            fields["publicPort"] =
                online_wire::value_int(endpoint ? endpoint->port : 0);
            const auto lan_self = m_lobby.local_endpoint();
            fields["lanIp"] = online_wire::value_string(
                lan_self ? address_text(lan_self->ipv4) : std::string());
            fields["lanPort"] =
                online_wire::value_int(lan_self ? lan_self->port : 0);
            fields["natKind"] =
                online_wire::value_string(nat_name(m_lobby.nat()));
            fields["stunAt"] = online_wire::value_time(now);
            fields["currentLobbyId"] = online_wire::value_string(current_lobby);

            std::string mask;
            for (auto entry = fields.begin(); entry != fields.end(); ++entry)
                mask += (mask.empty() ? "?updateMask.fieldPaths="
                                      : "&updateMask.fieldPaths=") + entry.key();
            const manx_http::response answer = call(
                manx_http::method::patch,
                document_url("machines/" + credential.machine_id) + mask,
                document.dump(), true);
            // The same beat keeps the person visible as well as the machine.
            // Friends read presence from the profile, not from the machine
            // document they are not allowed to see.
            if (profile_ready) {
                json profile;
                profile["fields"]["online"] = online_wire::value_bool(true);
                profile["fields"]["lastSeen"] = online_wire::value_time(now);
                call(manx_http::method::patch,
                     document_url("profiles/" + credential.uid) +
                         "?updateMask.fieldPaths=online"
                         "&updateMask.fieldPaths=lastSeen",
                     profile.dump(), true);
            }
            if (answer.ok()) {
                published_games_hash = games_hash;
                // Presence on the website is derived from this and nothing
                // else, so the gap has to be comfortably shorter than the
                // grace window there - otherwise a machine that is sitting
                // in the launcher, perfectly well, reads as offline. Faster
                // while somebody is looking at the online screen, because
                // that is when "is it on?" is being asked.
                next_heartbeat = now + pace(30, 45, 120);
                if (m_state.load() != online_state::in_lobby)
                    publish(online_state::online, "Signed in.");
            } else if (answer.status == 429 || answer.status == 8) {
                publish(online_state::error,
                        "MANX online has used up today's free quota.");
                backoff_until = now + 300;
            }
        }

        // --- what the website has asked this machine to do ---------------
        if (now >= next_machine_poll) {
            const manx_http::response answer =
                call(manx_http::method::get,
                     document_url("machines/" + credential.machine_id), {},
                     true);
            if (answer.ok()) {
                const json document = parse_or_empty(answer.body);
                if (const auto* name = online_wire::find_field(document, "name")) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_machine_name = online_wire::read_string(*name);
                }
                if (const auto* owner =
                        online_wire::find_field(document, "ownerUid"))
                    machine_owner_uid = online_wire::read_string(*owner);
                if (const auto* field =
                        online_wire::find_field(document, "command")) {
                    // The push channel. The website bumps `seq` and the
                    // cabinet notices on its next poll, which is why nothing
                    // here ever has to run a query.
                    const auto map = field->find("mapValue");
                    if (map != field->end()) {
                        const auto sub = map->find("fields");
                        if (sub != map->end() && sub->is_object()) {
                            int64_t seq = 0;
                            std::string kind, lobby_id;
                            if (const auto at = sub->find("seq"); at != sub->end())
                                seq = online_wire::read_int(*at);
                            if (const auto at = sub->find("kind"); at != sub->end())
                                kind = online_wire::read_string(*at);
                            if (const auto at = sub->find("lobbyId");
                                at != sub->end())
                                lobby_id = online_wire::read_string(*at);
                            if (seq != 0 && seq != last_command_seq) {
                                last_command_seq = seq;
                                if (kind == "join" && !lobby_id.empty()) {
                                    current_lobby = lobby_id;
                                    next_lobby_poll = 0;
                                    std::lock_guard<std::mutex> lock(m_mutex);
                                    m_lobby_id = current_lobby;
                                } else if (kind == "leave") {
                                    current_lobby.clear();
                                    m_lobby.set_remote_peers({});
                                    std::lock_guard<std::mutex> lock(m_mutex);
                                    m_lobby_id.clear();
                                }
                                // Acknowledged so the website can show that
                                // the cabinet heard.
                                json ack;
                                ack["fields"]["commandAck"] =
                                    online_wire::value_int(seq);
                                call(manx_http::method::patch,
                                     document_url("machines/" +
                                                  credential.machine_id) +
                                         "?updateMask.fieldPaths=commandAck",
                                     ack.dump(), true);
                                touch();
                            }
                        }
                    }
                }
            }
            // Two minutes when nobody is looking, five seconds when the
            // online screen is open. The slow rate is most of what keeps a
            // cabinet inside the free tier.
            next_machine_poll = now + pace(5, 30, 180);
        }

        // --- what is there to join ------------------------------------------
        // Swept on a timer as well as on demand. The service answers with
        // the lobbies this account may actually join - the open ones and
        // the ones its friends made - so there is no second query to run
        // and no rules to reason about here.
        if ((pending_browse || now >= next_lobby_watch) && machine_ready) {
            pending_browse = false;
            next_lobby_watch = now + pace(10, 10, 300);

            std::string friend_list;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (const online_friend& person : m_friends)
                    if (person.accepted) {
                        if (!friend_list.empty()) friend_list += ",";
                        friend_list += person.uid;
                    }
            }
            const json answer = service(
                ("/api/lobbies?friends=" + friend_list).c_str(), {}, false);
            const auto listed = answer.find("lobbies");
            if (listed != answer.end() && listed->is_array()) {
                std::vector<online_lobby> found;
                for (const json& entry : *listed) {
                    online_lobby lobby = lobby_from_service(entry);
                    if (!lobby.id.empty() && lobby.id != current_lobby)
                        found.push_back(std::move(lobby));
                }
                // Anything that was not there last time, offered once.
                if (announced_lobbies.size() > 512) {
                    announced_lobbies.clear();
                    lobby_watch_primed = false;
                }
                std::vector<std::string> ids;
                ids.reserve(found.size());
                for (const online_lobby& entry : found)
                    ids.push_back(entry.id);
                const std::string news = online_wire::first_unseen(
                    ids, announced_lobbies,
                    lobby_watch_primed && current_lobby.empty());
                lobby_watch_primed = true;
                std::optional<online_lobby> appeared;
                for (const online_lobby& entry : found)
                    if (entry.id == news) appeared = entry;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_lobbies = found;
                    if (appeared) m_new_lobby = appeared;
                }
                touch();
            }
        }

        // --- friends -------------------------------------------------------
        if (!pending_befriend.empty() && !credential.uid.empty()) {
            const std::string wanted = online_wire::handle_for(pending_befriend);
            const std::string typed = pending_befriend;
            pending_befriend.clear();
            const manx_http::response found =
                wanted.empty()
                    ? manx_http::response{}
                    : call(manx_http::method::get,
                           document_url("handles/" + wanted), {}, true);
            std::string them;
            if (found.ok()) {
                const json handle = parse_or_empty(found.body);
                if (const auto* uid = online_wire::find_field(handle, "uid"))
                    them = online_wire::read_string(*uid);
            }
            if (them.empty()) {
                // The handles collection cannot be listed - that is the whole
                // point of it - so there is no "did you mean" to offer and no
                // way to tell a typo from somebody who is not signed up.
                announce("Nobody online is called \"" + typed + "\".");
            } else if (them == credential.uid) {
                announce("That is you.");
            } else {
                json document;
                auto& fields = document["fields"];
                const std::string first =
                    credential.uid < them ? credential.uid : them;
                const std::string second =
                    credential.uid < them ? them : credential.uid;
                fields["members"] = online_wire::value_strings({first, second});
                fields["requesterUid"] =
                    online_wire::value_string(credential.uid);
                fields["state"] = online_wire::value_string("pending");
                fields["createdAt"] = online_wire::value_time(now);
                const manx_http::response asked = call(
                    manx_http::method::patch,
                    document_url("friendships/" +
                                 online_wire::friendship_id(credential.uid, them)) +
                        "?currentDocument.exists=false",
                    document.dump(), true);
                // A refused create is nearly always the friendship already
                // being there - asked, answered or agreed - which is not a
                // failure worth a red screen. One read says which, so the
                // answer is the true one rather than a guess.
                if (asked.ok()) {
                    announce(typed + " has been asked.");
                } else {
                    const manx_http::response already = call(
                        manx_http::method::get,
                        document_url("friendships/" +
                                     online_wire::friendship_id(credential.uid, them)),
                        {}, true);
                    std::string state;
                    // The document has to be held in a named variable:
                    // find_field returns a pointer into it, and parsing
                    // inline would hand back a pointer to a temporary that
                    // is already gone by the time the body of the `if` runs.
                    // It read as "could not send that request" for a
                    // friendship that was perfectly well already there.
                    const json existing_pair = parse_or_empty(already.body);
                    if (already.ok())
                        if (const auto* at =
                                online_wire::find_field(existing_pair, "state"))
                            state = online_wire::read_string(*at);
                    if (state == "accepted")
                        announce("You are already friends with " + typed + ".");
                    else if (state == "pending")
                        announce(typed + " has already been asked.");
                    else if (state.empty())
                        announce(explain_failure(asked, "sending that request"));
                    else
                        announce(typed + " has said no to that already.");
                }
                pending_friends = true;
            }
        }

        for (const auto& [uid, accept] : pending_answers) {
            json document;
            document["fields"]["state"] =
                online_wire::value_string(accept ? "accepted" : "declined");
            document["fields"]["respondedAt"] = online_wire::value_time(now);
            const manx_http::response answered = call(
                manx_http::method::patch,
                document_url("friendships/" +
                             online_wire::friendship_id(credential.uid, uid)) +
                    "?updateMask.fieldPaths=state"
                    "&updateMask.fieldPaths=respondedAt",
                document.dump(), true);
            if (!answered.ok())
                announce(explain_failure(answered, "answering that request"));
            pending_friends = true;
        }
        pending_answers.clear();

        if ((pending_friends || now >= next_friend_poll) &&
            !credential.uid.empty()) {
            pending_friends = false;
            // A friends poll is a query plus one profile read per person, so
            // the background rate is minutes rather than seconds: what it is
            // for there is noticing a request somebody sent while the
            // launcher was sitting on the shelf, not tracking presence to
            // the second.
            next_friend_poll = now + pace(20, 120, 900);
            reload_friends(credential.uid);
        }

        // --- hosting one ---------------------------------------------------
        // The game, the places and who may join. Everything else a linked
        // game needs settling - the lockstep seed, the frame delay, the comm
        // ports the real boards use - is decided by the service and handed
        // to every machine, so two cabinets cannot arrive at different
        // answers. They used to, and a linked Model 2 game sat on CHECKING
        // NETWORK for ever because of it.
        if (pending_host && machine_ready) {
            pending_host = false;
            json wanted;
            wanted["game"] = pending_host_game;
            wanted["gameName"] = pending_host_title;
            wanted["places"] = std::clamp(pending_host_places, 2, 8);
            wanted["mode"] = pending_host_mode;
            wanted["visibility"] = pending_host_open ? "public" : "friends";
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                std::vector<std::string> readers;
                for (const online_friend& person : m_friends)
                    if (person.accepted) readers.push_back(person.uid);
                wanted["readers"] = readers;
            }
            const json made = service("/api/lobby/create", wanted);
            const auto detail = made.find("lobby");
            if (detail != made.end() && detail->is_object()) {
                apply_lobby(*detail, false);
                current_lobby = detail->value("id", std::string());
                lobby_game = detail->value("game", std::string());
                std::printf("MANX online: hosting lobby %s\n",
                            current_lobby.c_str());
                publish(online_state::in_lobby,
                        "Lobby " + current_lobby + " - read this code out to "
                        "whoever is joining.");
            }
        }

        // --- the host saying go --------------------------------------------
        // Answered by the service pushing `go` to every machine at once,
        // rather than each of them noticing a changed document whenever
        // their next poll happened to fall.
        if (pending_start && !current_lobby.empty()) {
            pending_start = false;
            service("/api/lobby/start", json::object());
        }

        // --- joining and leaving ---------------------------------------------
        if (!pending_join.empty() && machine_ready) {
            const std::string wanted = pending_join;
            pending_join.clear();
            json ask;
            ask["id"] = wanted;
            const json answer = service("/api/lobby/join", ask);
            const auto detail = answer.find("lobby");
            if (detail != answer.end() && detail->is_object()) {
                apply_lobby(*detail, false);
                current_lobby = detail->value("id", std::string());
                lobby_game = detail->value("game", std::string());
                next_lobby_poll = 0;
            }
        }

        if (pending_leave) {
            pending_leave = false;
            if (!current_lobby.empty()) service("/api/lobby/leave", json::object());
            current_lobby.clear();
            lobby_game.clear();
            m_lobby.set_remote_peers({});
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lobby_id.clear();
                m_members.clear();
                m_lobby_game.clear();
                m_lobby_game_name.clear();
                m_lobby_starting = false;
                m_lobby_host = false;
            }
            publish(online_state::online, "Left the lobby.");
        }

        // --- where this machine can be reached ------------------------------
        // The one thing a cabinet still tells the service about itself, and
        // the only one it needs: both address candidates, and whether it
        // actually owns the game. Everything that comes back - who else is
        // here, their addresses, the go - arrives on the event watcher, so
        // there is no reading back what we just wrote.
        if (!current_lobby.empty() && now >= next_lobby_poll) {
            m_lobby.begin_public_endpoint_probe();
            const auto endpoint = m_lobby.public_endpoint();
            const auto lan = m_lobby.local_endpoint();
            json where;
            where["publicIp"] =
                endpoint ? address_text(endpoint->ipv4) : std::string();
            where["publicPort"] = endpoint ? endpoint->port : 0;
            // Two cabinets in one house share a public address, and asking a
            // router to send a packet back in through its own outside face
            // is a thing plenty of them refuse. This is the candidate that
            // actually carries those.
            where["lanIp"] = lan ? address_text(lan->ipv4) : std::string();
            where["lanPort"] = lan ? lan->port : 0;
            bool has_game = lobby_game.empty();
            if (!lobby_game.empty()) {
                std::lock_guard<std::mutex> lock(m_mutex);
                has_game = std::find(m_games.begin(), m_games.end(),
                                     lobby_game) != m_games.end();
            }
            where["hasGame"] = has_game;
            where["rttMs"] = m_lobby.worst_rtt_ms();
            const json answer = service("/api/lobby/address", where);
            const auto detail = answer.find("lobby");
            if (detail != answer.end() && detail->is_object())
                apply_lobby(*detail, false);
            else if (answer.empty())
                // The service has forgotten this lobby, so this machine is
                // not in one either.
                current_lobby.clear();

            // Often while the addresses are still being swapped, rarely once
            // the machines are talking directly, and not at all during a
            // match - the service has no part to play in one.
            const bool in_game =
                m_lobby.presence() == machine_presence::in_game;
            if (in_game)                  next_lobby_poll = now + 60;
            else if (m_lobby.connected()) next_lobby_poll = now + 15;
            else                          next_lobby_poll = now + 2;
        }

        // 200 ms while the addresses are still being exchanged, half a
        // second otherwise. This is the real pacing of a cabinet trying to
        // connect: the poll interval above can say "now" all it likes if the
        // loop then sleeps half a second before acting on it.
        sleep_a_moment(!current_lobby.empty() && !m_lobby.connected() ? 200
                                                                     : 500);
    }

    // On the way out, stop other machines punching at an address that is no
    // longer listening.
    m_lobby.set_remote_peers({});
}
