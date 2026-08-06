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

void online_link::register_account(std::string display_name,
                                   std::string email, std::string password,
                                   bool remember) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::register_account, std::move(email),
                          std::move(password), std::move(display_name),
                          remember});
    m_wake.notify_all();
}

void online_link::sign_in(std::string email, std::string password,
                          bool remember) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::sign_in, std::move(email),
                          std::move(password), {}, remember});
    m_wake.notify_all();
}

std::string online_link::display_name() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_display_name;
}

void online_link::sign_out() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::sign_out, {}, {}, {}, false});
    m_wake.notify_all();
}

void online_link::forget_machine() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::forget, {}, {}, {}, false});
    m_wake.notify_all();
}

void online_link::create_lobby(bool open_to_anyone) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::host, {}, {}, {}, open_to_anyone});
    m_wake.notify_all();
}

void online_link::join_lobby(std::string lobby_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::join, std::move(lobby_id), {}, {}, false});
    m_wake.notify_all();
}

void online_link::leave_lobby() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_commands.push_back({command::kind::leave, {}, {}, {}, false});
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
    // Remembered across restarts, and across signing out: the address is not
    // a secret and retyping it on a cabinet is exactly the friction this
    // whole screen exists to avoid.
    if (!credential.email.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_email = credential.email;
    }
    bool pending_auth = false;
    bool pending_create = false;
    std::string pending_email;
    std::string pending_password;
    std::string pending_name;
    bool profile_ready = false;
    bool machine_ready = false;
    bool pending_host = false;
    bool pending_host_open = false;
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
                // The address stays so the next sign-in is a password and
                // nothing else; only the token that grants access is
                // destroyed. "Forget this machine" is what wipes it all.
                credential.uid.clear();
                credential.refresh_token.clear();
                save_credential(credential);
                token = {};
                profile_ready = false;
                machine_ready = false;
                current_lobby.clear();
                m_lobby.set_remote_peers({});
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_lobby_id.clear();
                    m_members.clear();
                }
                publish(online_state::signed_out, "Signed out.");
                break;
            case command::kind::forget:
                forget_credential();
                credential = {};
                token = {};
                profile_ready = false;
                machine_ready = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_email.clear();
                    m_display_name.clear();
                }
                publish(online_state::signed_out,
                        "This machine has forgotten everything.");
                break;
            case command::kind::host:
                pending_host = true;
                pending_host_open = entry.flag;
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
                if (!current_lobby.empty() && !credential.machine_id.empty()) {
                    // Take our entry out rather than leaving a stale address
                    // for everybody else to punch at for a minute.
                    json update;
                    update["fields"]["members"]["mapValue"]["fields"] = json::object();
                    call(manx_http::method::patch,
                         document_url("lobbies/" + current_lobby) +
                             "?updateMask.fieldPaths=members." +
                             credential.machine_id,
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
            // A GET that will 401 and drive the refresh in `call`. Cheaper
            // than a second refresh path that could drift from the first.
            const manx_http::response answer =
                call(manx_http::method::get,
                     document_url("machines/" + credential.machine_id), {},
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
            fields["lanIp"] = online_wire::value_string(std::string());
            fields["lanPort"] = online_wire::value_int(0);
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
            } else {
                publish(online_state::error,
                        explain_failure(written, "registering this machine"));
                backoff_until = now + backoff_seconds;
                backoff_seconds = std::min<int64_t>(backoff_seconds * 2, 60);
                continue;
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
                document_url("machines/" + credential.machine_id) + mask,
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
            next_machine_poll = now + (m_foreground.load() ? 5 : 120);
        }

        // --- hosting one ---------------------------------------------------
        if (pending_host && machine_ready) {
            pending_host = false;
            // Six characters, short enough to read out over a phone.
            const std::string code = random_code(6);
            json document;
            auto& fields = document["fields"];
            fields["hostUid"] = online_wire::value_string(credential.uid);
            fields["hostOwnerUid"] = online_wire::value_string(credential.uid);
            fields["gameShortName"] = online_wire::value_string(std::string());
            fields["gameDisplayName"] = online_wire::value_string(std::string());
            fields["mode"] = online_wire::value_string("simultaneous");
            fields["places"] = online_wire::value_int(2);
            fields["state"] = online_wire::value_string("open");
            fields["visibility"] = online_wire::value_string(
                pending_host_open ? "public" : "invite");
            fields["memberUids"] = online_wire::value_strings({credential.uid});
            fields["readerUids"] = online_wire::value_strings({credential.uid});
            fields["startAtMs"] = online_wire::value_int(0);
            fields["seed"] = online_wire::value_int(1);
            fields["delayFrames"] = online_wire::value_int(3);
            fields["buildHash"] = online_wire::value_string(build_hash());
            fields["createdAt"] = online_wire::value_time(now);
            fields["updatedAt"] = online_wire::value_time(now);
            fields["members"] = online_wire::value_map(json::object());
            const manx_http::response made = call(
                manx_http::method::patch, document_url("lobbies/" + code),
                document.dump(), true);
            if (made.ok()) {
                current_lobby = code;
                next_lobby_poll = 0;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_lobby_id = code;
                }
                std::printf("MANX online: hosting lobby %s\n", code.c_str());
                publish(online_state::in_lobby,
                        "Lobby " + code + " - read this code out to whoever "
                        "is joining.");
            } else {
                publish(online_state::error,
                        explain_failure(made, "creating the lobby"));
            }
        }

        // --- the lobby ----------------------------------------------------
        if (!current_lobby.empty() && now >= next_lobby_poll) {
            const auto endpoint = m_lobby.public_endpoint();
            json entry;
            auto& member = entry["fields"]["members"]["mapValue"]["fields"]
                                [credential.machine_id]["mapValue"]["fields"];
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
                     credential.machine_id + "&updateMask.fieldPaths=updatedAt",
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
                        members, credential.machine_id, now, parse_ipv4);
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
            // All this exchanges is an ip and a port. The punch itself takes
            // milliseconds once both ends know where to aim, so any wait
            // here is this poll and nothing else - joining a lobby should
            // feel immediate, not like a page loading.
            //
            // So: as fast as the service will sensibly allow while the
            // addresses are still being swapped, then back off hard once the
            // machines are talking directly, and stop entirely during a
            // match, when the cloud has no part to play at all.
            const bool in_game = m_lobby.presence() == machine_presence::in_game;
            if (in_game)                      next_lobby_poll = now + 60;
            else if (m_lobby.connected())     next_lobby_poll = now + 10;
            else                              next_lobby_poll = now;   // no wait
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
