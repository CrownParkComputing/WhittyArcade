// online_wire.h - everything about talking to Firestore that is not talking
// to Firestore.
//
// The other half of this contract - collection names, field names, enum
// spellings, the code alphabet - is public/js/model.js in the MANXOnline
// repo. A change made in one and not the other does not fail loudly: it fails
// as a cabinet that simply never appears in a lobby.
//
// Token arithmetic, the typed-value encoding Firestore's REST API uses, and
// the decision about which members of a cloud lobby are worth punching at.
// All of it pure, so it can be tested from canned JSON with no network, no
// account and no libcurl - which matters, because the fiddly part of this
// feature is not the HTTP, it is that Firestore returns integers as strings
// and nobody notices until a port number is silently zero.
#pragma once

#include "lobby_targets.h"
#include "json.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace online_wire {

// --- tokens ------------------------------------------------------------
// An ID token lasts an hour. Refreshing on expiry means every hour there is
// one request that fails first and succeeds second; refreshing early means
// there never is one.
constexpr int64_t refresh_margin_seconds = 300;

struct token_state {
    std::string id_token;
    std::string refresh_token;
    int64_t expires_unix{};

    bool usable(int64_t now_unix) const {
        return !id_token.empty() && now_unix < expires_unix;
    }
    bool needs_refresh(int64_t now_unix) const {
        if (refresh_token.empty()) return false;
        if (id_token.empty()) return true;
        return now_unix >= expires_unix - refresh_margin_seconds;
    }
};

// Firebase sends expiresIn as a string of seconds. Anything unreadable is
// treated as already expired: a token whose lifetime is unknown is one that
// should be refreshed before it is trusted, not one to gamble on.
inline int64_t expiry_from_expires_in(int64_t now_unix,
                                      const std::string& expires_in) {
    if (expires_in.empty()) return now_unix;
    char* end = nullptr;
    const long long seconds = std::strtoll(expires_in.c_str(), &end, 10);
    if (end == expires_in.c_str() || seconds <= 0) return now_unix;
    return now_unix + static_cast<int64_t>(seconds);
}

// What to do about a 401 from Firestore. Once: the token expired early, so
// refresh and try again. Twice: the token was revoked or the account is
// gone, and retrying for ever would be a busy loop against an answer that is
// not going to change.
enum class auth_decision { retry_with_refresh, sign_out };

inline auth_decision next_after_unauthorized(int attempts_already_made) {
    return attempts_already_made == 0 ? auth_decision::retry_with_refresh
                                      : auth_decision::sign_out;
}

// --- Firestore's typed values -----------------------------------------
inline nlohmann::json value_string(const std::string& text) {
    return nlohmann::json{{"stringValue", text}};
}

// Note the string. Firestore encodes 64-bit integers as decimal strings in
// JSON, in both directions, because JSON numbers cannot carry them safely.
inline nlohmann::json value_int(int64_t number) {
    return nlohmann::json{{"integerValue", std::to_string(number)}};
}

inline nlohmann::json value_bool(bool flag) {
    return nlohmann::json{{"booleanValue", flag}};
}

inline std::string rfc3339(int64_t unix_seconds) {
    const std::time_t when = static_cast<std::time_t>(unix_seconds);
    std::tm parts{};
#if defined(_WIN32)
    gmtime_s(&parts, &when);
#else
    gmtime_r(&when, &parts);
#endif
    char text[32]{};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &parts);
    return text;
}

inline nlohmann::json value_time(int64_t unix_seconds) {
    return nlohmann::json{{"timestampValue", rfc3339(unix_seconds)}};
}

inline nlohmann::json value_strings(const std::vector<std::string>& list) {
    nlohmann::json values = nlohmann::json::array();
    for (const std::string& entry : list) values.push_back(value_string(entry));
    return nlohmann::json{{"arrayValue", {{"values", values}}}};
}

inline nlohmann::json value_map(nlohmann::json fields) {
    return nlohmann::json{{"mapValue", {{"fields", std::move(fields)}}}};
}

inline std::string base64_encode(const std::vector<uint8_t>& bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t a = bytes[i];
        const uint32_t b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const uint32_t c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const uint32_t value = (a << 16) | (b << 8) | c;
        result.push_back(alphabet[(value >> 18) & 63]);
        result.push_back(alphabet[(value >> 12) & 63]);
        result.push_back(i + 1 < bytes.size() ? alphabet[(value >> 6) & 63]
                                              : '=');
        result.push_back(i + 2 < bytes.size() ? alphabet[value & 63] : '=');
    }
    return result;
}

inline bool base64_decode(std::string_view text, std::vector<uint8_t>& out) {
    const auto value_of = [](char character) -> int {
        if (character >= 'A' && character <= 'Z') return character - 'A';
        if (character >= 'a' && character <= 'z') return character - 'a' + 26;
        if (character >= '0' && character <= '9') return character - '0' + 52;
        if (character == '+') return 62;
        if (character == '/') return 63;
        return -1;
    };
    out.clear();
    if (text.size() % 4 != 0) return false;
    out.reserve((text.size() / 4) * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const int a = value_of(text[i]);
        const int b = value_of(text[i + 1]);
        const bool pad_c = text[i + 2] == '=';
        const bool pad_d = text[i + 3] == '=';
        const int c = pad_c ? 0 : value_of(text[i + 2]);
        const int d = pad_d ? 0 : value_of(text[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || (pad_c && !pad_d) ||
            ((pad_c || pad_d) && i + 4 != text.size())) {
            out.clear();
            return false;
        }
        const uint32_t value = (uint32_t(a) << 18) | (uint32_t(b) << 12) |
                               (uint32_t(c) << 6) | uint32_t(d);
        out.push_back(static_cast<uint8_t>(value >> 16));
        if (!pad_c) out.push_back(static_cast<uint8_t>(value >> 8));
        if (!pad_d) out.push_back(static_cast<uint8_t>(value));
    }
    return true;
}

inline nlohmann::json value_bytes(const std::vector<uint8_t>& bytes) {
    return nlohmann::json{{"bytesValue", base64_encode(bytes)}};
}

inline bool read_bytes(const nlohmann::json& value,
                       std::vector<uint8_t>& out) {
    const auto found = value.find("bytesValue");
    return found != value.end() && found->is_string() &&
           base64_decode(found->get<std::string>(), out);
}

// --- reading them back -------------------------------------------------
inline const nlohmann::json* find_field(const nlohmann::json& document,
                                        const std::string& name) {
    const auto fields = document.find("fields");
    if (fields == document.end() || !fields->is_object()) return nullptr;
    const auto found = fields->find(name);
    if (found == fields->end() || !found->is_object()) return nullptr;
    return &*found;
}

inline std::string read_string(const nlohmann::json& value,
                               const std::string& fallback = {}) {
    const auto found = value.find("stringValue");
    if (found == value.end() || !found->is_string()) return fallback;
    return found->get<std::string>();
}

// Accepts both encodings. Firestore always sends the string form, but a
// document written by the JavaScript SDK and read back through a different
// path has been seen as a number, and a port that silently reads as zero is
// a machine nobody can connect to.
inline int64_t read_int(const nlohmann::json& value, int64_t fallback = 0) {
    if (const auto found = value.find("integerValue"); found != value.end()) {
        if (found->is_string()) {
            const std::string text = found->get<std::string>();
            char* end = nullptr;
            const long long parsed = std::strtoll(text.c_str(), &end, 10);
            if (end != text.c_str()) return static_cast<int64_t>(parsed);
            return fallback;
        }
        if (found->is_number_integer()) return found->get<int64_t>();
    }
    if (const auto found = value.find("doubleValue");
        found != value.end() && found->is_number())
        return static_cast<int64_t>(found->get<double>());
    return fallback;
}

inline bool read_bool(const nlohmann::json& value, bool fallback = false) {
    const auto found = value.find("booleanValue");
    if (found == value.end() || !found->is_boolean()) return fallback;
    return found->get<bool>();
}

// Only the shapes Firestore actually emits, and only to the second. This is
// used to decide whether a member entry is stale, not to display a clock.
inline int64_t read_time(const nlohmann::json& value, int64_t fallback = 0) {
    const auto found = value.find("timestampValue");
    if (found == value.end() || !found->is_string()) return fallback;
    const std::string text = found->get<std::string>();
    std::tm parts{};
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day,
                    &hour, &minute, &second) != 6)
        return fallback;
    parts.tm_year = year - 1900;
    parts.tm_mon = month - 1;
    parts.tm_mday = day;
    parts.tm_hour = hour;
    parts.tm_min = minute;
    parts.tm_sec = second;
#if defined(_WIN32)
    return static_cast<int64_t>(_mkgmtime(&parts));
#else
    return static_cast<int64_t>(timegm(&parts));
#endif
}

inline std::vector<std::string> read_strings(const nlohmann::json& value) {
    std::vector<std::string> list;
    const auto array = value.find("arrayValue");
    if (array == value.end() || !array->is_object()) return list;
    const auto values = array->find("values");
    if (values == array->end() || !values->is_array()) return list;
    for (const auto& entry : *values) list.push_back(read_string(entry));
    return list;
}

// --- names -------------------------------------------------------------
// A display name as the handles collection spells it. The rules require
// handle == handle.lower() and cap it at 32, and a name somebody has to
// punctuate exactly is a name nobody can add as a friend - so this is what
// both writing a handle and searching for one go through.
inline std::string handle_for(const std::string& name) {
    std::string handle;
    for (const char character : name) {
        const unsigned char raw = static_cast<unsigned char>(character);
        if (raw >= 'A' && raw <= 'Z')
            handle.push_back(static_cast<char>(raw - 'A' + 'a'));
        else if ((raw >= 'a' && raw <= 'z') || (raw >= '0' && raw <= '9'))
            handle.push_back(static_cast<char>(raw));
        if (handle.size() == 32) break;
    }
    return handle;
}

// The document id of a friendship: the two uids, sorted and joined. The
// rules check exactly this, which is what makes "one friendship per pair"
// and "you cannot invent a friendship you are not in" structural rather than
// merely validated.
inline std::string friendship_id(const std::string& first,
                                 const std::string& second) {
    return first < second ? first + "_" + second : second + "_" + first;
}

// --- what has appeared since last time ---------------------------------
// The first lobby id in `visible` that `seen` has not got, with everything
// visible remembered on the way through. `primed` false remembers and says
// nothing, which is the first sweep after signing in: a lobby that has been
// open for an hour belongs in the join list, not in a question asked over
// whatever was on screen the moment the launcher opened.
//
// Pure, because "asked twice about the same lobby" is a bug that would
// otherwise only ever show up in front of two cabinets.
inline std::string first_unseen(const std::vector<std::string>& visible,
                                std::set<std::string>& seen, bool primed) {
    std::string found;
    for (const std::string& id : visible) {
        if (id.empty()) continue;
        if (!seen.insert(id).second) continue;
        if (primed && found.empty()) found = id;
    }
    return found;
}

// --- lobby members -----------------------------------------------------
struct member {
    std::string machine_uid;
    std::string owner_uid;
    std::string name;
    std::string public_ip;
    uint16_t public_port{};
    std::string lan_ip;
    uint16_t lan_port{};
    int node{};
    bool has_game{};
    bool ready{};
    std::string link;
    int rtt_ms{};
    int64_t updated_unix{};
};

// One member entry, from the map value the lobby document carries.
inline member parse_member(const std::string& machine_uid,
                           const nlohmann::json& entry) {
    member result;
    result.machine_uid = machine_uid;
    const auto map = entry.find("mapValue");
    if (map == entry.end() || !map->is_object()) return result;
    const auto fields = map->find("fields");
    if (fields == map->end() || !fields->is_object()) return result;

    const auto get = [&fields](const char* name) -> const nlohmann::json* {
        const auto found = fields->find(name);
        return found == fields->end() ? nullptr : &*found;
    };
    if (const auto* at = get("ownerUid")) result.owner_uid = read_string(*at);
    if (const auto* at = get("name")) result.name = read_string(*at);
    if (const auto* at = get("publicIp")) result.public_ip = read_string(*at);
    if (const auto* at = get("publicPort"))
        result.public_port = static_cast<uint16_t>(read_int(*at));
    if (const auto* at = get("lanIp")) result.lan_ip = read_string(*at);
    if (const auto* at = get("lanPort"))
        result.lan_port = static_cast<uint16_t>(read_int(*at));
    if (const auto* at = get("node")) result.node = static_cast<int>(read_int(*at));
    if (const auto* at = get("hasGame")) result.has_game = read_bool(*at);
    if (const auto* at = get("ready")) result.ready = read_bool(*at);
    if (const auto* at = get("link")) result.link = read_string(*at);
    if (const auto* at = get("rttMs")) result.rtt_ms = static_cast<int>(read_int(*at));
    if (const auto* at = get("updatedAt")) result.updated_unix = read_time(*at);
    return result;
}

inline std::vector<member> parse_members(const nlohmann::json& document) {
    std::vector<member> members;
    const nlohmann::json* field = find_field(document, "members");
    if (!field) return members;
    const auto map = field->find("mapValue");
    if (map == field->end() || !map->is_object()) return members;
    const auto fields = map->find("fields");
    if (fields == map->end() || !fields->is_object()) return members;
    for (auto entry = fields->begin(); entry != fields->end(); ++entry)
        members.push_back(parse_member(entry.key(), entry.value()));
    return members;
}

// A member entry nobody has touched for this long belongs to a machine that
// crashed: nothing else deletes it, and punching at a stale address is how a
// lobby ends up permanently "connecting".
constexpr int64_t member_stale_seconds = 60;

// Which addresses are worth sending hellos to.
//
// Both candidates are published for every member, and both are used. Two
// machines behind one router see the same public address, and hairpinning -
// a router sending a packet back in through its own outside face - is not
// universal. Ten lines of ICE-lite here is the difference between "the
// friend who joined online at a LAN party works" and a failure nobody can
// explain.
inline std::vector<lobby_link::remote_peer> peers_from_members(
        const std::vector<member>& members, const std::string& self_uid,
        int64_t now_unix,
        uint32_t (*parse_ipv4)(const std::string&)) {
    std::vector<lobby_link::remote_peer> peers;
    for (const member& entry : members) {
        if (entry.machine_uid == self_uid) continue;
        if (entry.updated_unix != 0 &&
            now_unix - entry.updated_unix > member_stale_seconds)
            continue;
        const auto add = [&peers](uint32_t ipv4, uint16_t port) {
            if (ipv4 == 0 || port == 0) return;
            const lobby_link::remote_peer peer{ipv4, port};
            if (std::find(peers.begin(), peers.end(), peer) == peers.end())
                peers.push_back(peer);
        };
        add(parse_ipv4(entry.public_ip), entry.public_port);
        add(parse_ipv4(entry.lan_ip), entry.lan_port);
    }
    return peers;
}

} // namespace online_wire
