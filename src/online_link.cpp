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
constexpr int pairing_code_length = 8;

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

// The code has to leave this machine and be typed somewhere else, and the
// launcher has no clipboard, no text field and no way to select anything on
// screen. So it goes everywhere it can usefully go: the terminal, a file, and
// the desktop clipboard. Reading eight characters off a screen and typing
// them on a phone is fine; being unable to copy them when the browser is on
// the same desktop is just annoying.
void announce_pairing_code(const std::string& code) {
    std::printf("\n"
                "  +--------------------------------------------+\n"
                "  |  MANX online pairing code:  %s  |\n"
                "  +--------------------------------------------+\n"
                "  Enter it under Cabinets -> Add a cabinet.\n\n",
                code.c_str());
    std::fflush(stdout);

    const fs::path root = manx_platform::config_root();
    const fs::path where =
        (root.empty() ? fs::current_path() : root) / "MANX" / "pairing-code.txt";
    std::error_code error;
    if (where.has_parent_path())
        fs::create_directories(where.parent_path(), error);
    if (std::ofstream out(where, std::ios::trunc); out) out << code << '\n';

#if !defined(_WIN32)
    // The code is eight characters of [0-9A-Z] by construction, so there is
    // nothing here a shell could misread.
    if (!std::getenv("MANX_NO_CLIPBOARD")) {
        const char* tool = std::getenv("WAYLAND_DISPLAY")
            ? "wl-copy 2>/dev/null" : "xclip -selection clipboard 2>/dev/null";
        if (FILE* pipe = popen(tool, "w")) {
            std::fputs(code.c_str(), pipe);
            if (pclose(pipe) == 0)
                std::printf("  (also copied to the clipboard)\n");
        }
    }
#endif
}

// --- the credential on disk ---------------------------------------------
// Only the refresh token, never the password. The password exists for the
// few seconds between the website writing it and this machine using it, and
// is never written down here.
struct stored_credential {
    std::string machine_uid;
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
        if (key == "machine_uid") credential.machine_uid = value;
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
        output << "machine_uid=" << credential.machine_uid << '\n'
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
}

online_link::~online_link() {
    m_stop.store(true);
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

bool online_link::available() {
    return manx_http::available() && manx_cloud::configured();
}

void online_link::publish(online_state state, std::string status) {
    const online_state was = m_state.exchange(state);
    // Narrated to the terminal as well as the screen. The screen is on a
    // cabinet that may be across the room, in a menu somebody has already
    // navigated away from, and it keeps no history - so when pairing fails
    // the one place the reason survives is here.
    if (was != state || state == online_state::error)
        std::printf("MANX online: %s\n", status.c_str());
    std::fflush(stdout);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status = std::move(status);
    }
    touch();
}

void online_link::touch() { m_revision.fetch_add(1); }

void online_link::start_pairing() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::pair, {}});
    m_wake.notify_all();
}

void online_link::cancel_pairing() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::cancel_pair, {}});
    m_wake.notify_all();
}

void online_link::sign_out() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::sign_out, {}});
    m_wake.notify_all();
}

void online_link::join_lobby(std::string lobby_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::join, std::move(lobby_id)});
    m_wake.notify_all();
}

void online_link::leave_lobby() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::leave, {}});
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

std::string online_link::pairing_code() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pairing_code;
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

online_state online_link::state() const { return m_state.load(); }

std::string online_link::status_text() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

uint64_t online_link::revision() const { return m_revision.load(); }

// ---------------------------------------------------------------------------
// The worker. Everything below this line runs on m_thread and nowhere else.

void online_link::run() {
    online_wire::token_state token;
    stored_credential credential = load_credential();
    std::string pairing_code;
    std::string anonymous_uid;
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

    const auto sign_in_with_password = [&](const std::string& email,
                                           const std::string& password) {
        const json request{{"email", email},
                           {"password", password},
                           {"returnSecureToken", true}};
        const manx_http::response answer =
            call(manx_http::method::post,
                 manx_cloud::identity_url("signInWithPassword"),
                 request.dump(), false);
        if (!answer.ok()) return false;
        const json body = parse_or_empty(answer.body);
        token.id_token = field_text(body, "idToken");
        token.refresh_token = field_text(body, "refreshToken");
        token.expires_unix = online_wire::expiry_from_expires_in(
            unix_now(), field_text(body, "expiresIn"));
        return !token.id_token.empty();
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
            case command::kind::pair:
                if (credential.refresh_token.empty() && pairing_code.empty()) {
                    pairing_code = random_code(pairing_code_length);
                    anonymous_uid.clear();
                    token = {};
                    // Deliberately NOT published yet. A code only means
                    // something once it is in the database: shown any
                    // earlier it can still be refused, or regenerated after
                    // a collision, and somebody will already have typed the
                    // one that no longer exists.
                    publish(online_state::pairing,
                            "Getting a code from MANX online...");
                }
                break;
            case command::kind::cancel_pair:
                pairing_code.clear();
                anonymous_uid.clear();
                token = {};
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_pairing_code.clear();
                }
                publish(online_state::signed_out, "Not signed in.");
                break;
            case command::kind::sign_out:
                forget_credential();
                credential = {};
                token = {};
                current_lobby.clear();
                m_lobby.set_remote_peers({});
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_lobby_id.clear();
                    m_members.clear();
                    m_machine_uid.clear();
                }
                publish(online_state::signed_out, "Signed out.");
                break;
            case command::kind::join:
                current_lobby = entry.argument;
                next_lobby_poll = 0;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_lobby_id = current_lobby;
                }
                break;
            case command::kind::leave:
                if (!current_lobby.empty() && !credential.machine_uid.empty()) {
                    // Take our entry out rather than leaving a stale address
                    // for everybody else to punch at for a minute.
                    json update;
                    update["fields"]["members"]["mapValue"]["fields"] = json::object();
                    call(manx_http::method::patch,
                         document_url("lobbies/" + current_lobby) +
                             "?updateMask.fieldPaths=members." +
                             credential.machine_uid,
                         update.dump(), true);
                }
                current_lobby.clear();
                m_lobby.set_remote_peers({});
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_lobby_id.clear();
                    m_members.clear();
                }
                break;
            }
        }

        const int64_t now = unix_now();
        if (now < backoff_until) { sleep_a_moment(500); continue; }

        // --- pairing ----------------------------------------------------
        if (!pairing_code.empty() && credential.refresh_token.empty()) {
            if (anonymous_uid.empty()) {
                std::printf("MANX online: signing in anonymously to claim a "
                            "pairing code\n");
                std::fflush(stdout);
                const manx_http::response answer =
                    call(manx_http::method::post,
                         manx_cloud::identity_url("signUp"),
                         json{{"returnSecureToken", true}}.dump(), false);
                if (!answer.ok()) {
                    publish(online_state::error,
                            explain_failure(answer, "starting the pairing"));
                    backoff_until = now + backoff_seconds;
                    backoff_seconds = std::min<int64_t>(backoff_seconds * 2, 60);
                    continue;
                }
                const json body = parse_or_empty(answer.body);
                anonymous_uid = field_text(body, "localId");
                token.id_token = field_text(body, "idToken");
                token.refresh_token = field_text(body, "refreshToken");
                token.expires_unix = online_wire::expiry_from_expires_in(
                    now, field_text(body, "expiresIn"));

                json document;
                document["fields"]["status"] = online_wire::value_string("waiting");
                document["fields"]["claimedBy"] = online_wire::value_string(anonymous_uid);
                document["fields"]["hostName"] = online_wire::value_string(host_name());
                document["fields"]["platform"] = online_wire::value_string(platform_name());
                document["fields"]["createdAt"] = online_wire::value_time(now);
                // Fifteen minutes, matching the rules and the TTL policy. A
                // code that lives longer is a credential that lives longer.
                document["fields"]["expiresAt"] = online_wire::value_time(now + 14 * 60);
                std::printf("MANX online: publishing pairing code %s\n",
                            pairing_code.c_str());
                std::fflush(stdout);
                const manx_http::response created = call(
                    manx_http::method::patch,
                    document_url("pairings/" + pairing_code), document.dump(),
                    true);
                if (!created.ok())
                    std::printf("MANX online: publishing the code returned "
                                "HTTP %ld %s\n", created.status,
                                created.body.substr(0, 300).c_str());
                if (!created.ok()) {
                    // A refused write here is not a taken code - the rules
                    // are what refuse it, and trying a different code for
                    // ever would hide that. Only a genuine conflict is worth
                    // a retry.
                    if (created.status == 409) {
                        pairing_code = random_code(pairing_code_length);
                        anonymous_uid.clear();
                        continue;
                    }
                    publish(online_state::error,
                            explain_failure(created, "publishing the code"));
                    pairing_code.clear();
                    anonymous_uid.clear();
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_pairing_code.clear();
                    }
                    backoff_until = now + backoff_seconds;
                    backoff_seconds = std::min<int64_t>(backoff_seconds * 2, 60);
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_pairing_code = pairing_code;
                }
                announce_pairing_code(pairing_code);
                publish(online_state::pairing,
                        "Type this code on the MANX website to add this "
                        "machine to your account. It is also on the "
                        "clipboard and in MANX/pairing-code.txt.");
            } else {
                const manx_http::response answer =
                    call(manx_http::method::get,
                         document_url("pairings/" + pairing_code), {}, true);
                if (answer.ok()) {
                    const json document = parse_or_empty(answer.body);
                    const auto status_field =
                        online_wire::find_field(document, "status");
                    if (status_field &&
                        online_wire::read_string(*status_field) == "claimed") {
                        const auto* email_field =
                            online_wire::find_field(document, "email");
                        const auto* password_field =
                            online_wire::find_field(document, "password");
                        const auto* uid_field =
                            online_wire::find_field(document, "machineUid");
                        if (email_field && password_field && uid_field) {
                            publish(online_state::signing_in, "Signing in...");
                            const std::string email =
                                online_wire::read_string(*email_field);
                            const std::string password =
                                online_wire::read_string(*password_field);
                            if (sign_in_with_password(email, password)) {
                                credential.machine_uid =
                                    online_wire::read_string(*uid_field);
                                credential.email = email;
                                credential.refresh_token = token.refresh_token;
                                save_credential(credential);
                                // Destroy the credential in the database the
                                // moment it has been used. Until this
                                // happens there is a real password sitting
                                // in a document.
                                call(manx_http::method::del,
                                     document_url("pairings/" + pairing_code),
                                     {}, true);
                                pairing_code.clear();
                                anonymous_uid.clear();
                                {
                                    std::lock_guard<std::mutex> lock(m_mutex);
                                    m_pairing_code.clear();
                                    m_machine_uid = credential.machine_uid;
                                }
                                publish(online_state::online, "Signed in.");
                                next_machine_poll = 0;
                                next_heartbeat = 0;
                            } else {
                                publish(online_state::error,
                                        "The website paired this machine but "
                                        "signing in failed.");
                            }
                        }
                    }
                }
                sleep_a_moment(2000);
                continue;
            }
        }

        // --- signed in? --------------------------------------------------
        if (credential.refresh_token.empty()) {
            if (pairing_code.empty() && m_state.load() != online_state::signed_out &&
                m_state.load() != online_state::error)
                publish(online_state::signed_out, "Not signed in.");
            sleep_a_moment(1000);
            continue;
        }
        if (!token.usable(now) || token.needs_refresh(now)) {
            if (token.refresh_token.empty())
                token.refresh_token = credential.refresh_token;
            // A GET that will 401 and drive the refresh in `call`. Cheaper
            // than a second refresh path that could drift from the first.
            const manx_http::response answer =
                call(manx_http::method::get,
                     document_url("machines/" + credential.machine_uid), {},
                     true);
            if (token.id_token.empty()) {
                if (!answer.ok() && answer.status == 0) {
                    publish(online_state::error, "No answer from MANX online.");
                    backoff_until = now + backoff_seconds;
                    backoff_seconds = std::min<int64_t>(backoff_seconds * 2, 60);
                } else {
                    // The account is gone, or its password was changed on the
                    // website. Pair again rather than retrying for ever.
                    forget_credential();
                    credential = {};
                    publish(online_state::signed_out,
                            "This machine is no longer paired.");
                }
                continue;
            }
            backoff_seconds = 5;
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
            fields["lanIp"] = online_wire::value_string(std::string());
            fields["lanPort"] = online_wire::value_int(0);
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
                document_url("machines/" + credential.machine_uid) + mask,
                document.dump(), true);
            if (answer.ok()) {
                published_games_hash = games_hash;
                // Presence on the website is derived from this and nothing
                // else, so the gap has to be comfortably shorter than the
                // grace window there - otherwise a machine that is sitting
                // in the launcher, perfectly well, reads as offline. Faster
                // while somebody is looking at the online screen, because
                // that is when "is it on?" is being asked.
                next_heartbeat = now + (m_foreground.load() ? 30 : 120);
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
                     document_url("machines/" + credential.machine_uid), {},
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
                                                  credential.machine_uid) +
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
            next_machine_poll = now + (m_foreground.load() ? 5 : 120);
        }

        // --- the lobby ----------------------------------------------------
        if (!current_lobby.empty() && now >= next_lobby_poll) {
            const auto endpoint = m_lobby.public_endpoint();
            json entry;
            auto& member = entry["fields"]["members"]["mapValue"]["fields"]
                                [credential.machine_uid]["mapValue"]["fields"];
            member["ownerUid"] = online_wire::value_string(machine_owner_uid);
            member["name"] = online_wire::value_string(host_name());
            member["node"] = online_wire::value_int(m_lobby.node());
            member["publicIp"] = online_wire::value_string(
                endpoint ? address_text(endpoint->ipv4) : std::string());
            member["publicPort"] =
                online_wire::value_int(endpoint ? endpoint->port : 0);
            member["lanIp"] = online_wire::value_string(std::string());
            member["lanPort"] = online_wire::value_int(0);
            // Whether this machine actually owns the game the host picked.
            // Reported honestly, so the website can say "3 of 4 cabinets
            // have Galaga" instead of letting a doomed match start and fail
            // at the point where somebody presses a button.
            bool has_game = lobby_game.empty();
            if (!lobby_game.empty()) {
                std::lock_guard<std::mutex> lock(m_mutex);
                has_game = std::find(m_games.begin(), m_games.end(),
                                     lobby_game) != m_games.end();
            }
            member["hasGame"] = online_wire::value_bool(has_game);
            member["ready"] = online_wire::value_bool(true);
            member["link"] = online_wire::value_string(
                m_lobby.connected() ? "connected" : "punching");
            member["rttMs"] = online_wire::value_int(0);
            member["updatedAt"] = online_wire::value_time(now);
            entry["fields"]["updatedAt"] = online_wire::value_time(now);

            call(manx_http::method::patch,
                 document_url("lobbies/" + current_lobby) +
                     "?updateMask.fieldPaths=members." +
                     credential.machine_uid + "&updateMask.fieldPaths=updatedAt",
                 entry.dump(), true);

            const manx_http::response answer =
                call(manx_http::method::get,
                     document_url("lobbies/" + current_lobby), {}, true);
            if (answer.ok()) {
                const json document = parse_or_empty(answer.body);
                if (const auto* game =
                        online_wire::find_field(document, "gameShortName"))
                    lobby_game = online_wire::read_string(*game);
                std::vector<online_wire::member> members =
                    online_wire::parse_members(document);
                const std::vector<lobby_link::remote_peer> peers =
                    online_wire::peers_from_members(
                        members, credential.machine_uid, now, parse_ipv4);
                // The handover. From here the lobby thread does the rest,
                // and a machine on the far side of the world is just another
                // address it sends hellos to.
                m_lobby.set_remote_peers(peers);
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_members = std::move(members);
                }
                publish(online_state::in_lobby,
                        peers.empty()
                            ? "In a lobby. Waiting for another machine."
                            : "In a lobby. Connecting to " +
                                  std::to_string(peers.size()) + " address(es).");
            } else if (answer.status == 404) {
                current_lobby.clear();
                m_lobby.set_remote_peers({});
                std::lock_guard<std::mutex> lock(m_mutex);
                m_lobby_id.clear();
                m_members.clear();
            }
            // Fast while the connection is being made, because that is when
            // the wait is felt; slow afterwards, because nothing changes
            // until somebody presses a button. Nothing at all during a match.
            const bool in_game = m_lobby.presence() == machine_presence::in_game;
            next_lobby_poll = now + (in_game ? 60 : (m_lobby.connected() ? 5 : 2));
        }

        sleep_a_moment(500);
    }

    // On the way out, stop other machines punching at an address that is no
    // longer listening.
    m_lobby.set_remote_peers({});
}
