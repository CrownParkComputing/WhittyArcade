// Offline tests for the Firestore wire layer.
//
// No network, no account, no libcurl - canned JSON in, decisions out. The
// cases here are the ones that would otherwise be found on a second machine
// at midnight: Firestore encodes integers as strings, so a port silently
// reads as zero, and a crashed cabinet leaves a member entry behind that
// nothing else ever deletes.

#include "online_wire.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
}

// A stand-in for inet_pton, so this test needs no socket headers.
uint32_t fake_parse_ipv4(const std::string& text) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(text.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    const uint8_t octets[4] = {static_cast<uint8_t>(a), static_cast<uint8_t>(b),
                               static_cast<uint8_t>(c), static_cast<uint8_t>(d)};
    uint32_t packed = 0;
    std::memcpy(&packed, octets, sizeof(packed));
    return packed;
}

std::string member_json(const std::string& uid, const std::string& public_ip,
                        const std::string& public_port,
                        const std::string& lan_ip,
                        const std::string& lan_port,
                        const std::string& updated) {
    return R"(")" + uid + R"(": {"mapValue": {"fields": {
        "ownerUid": {"stringValue": "alice"},
        "name": {"stringValue": "Cabinet"},
        "node": {"integerValue": "2"},
        "publicIp": {"stringValue": ")" + public_ip + R"("},
        "publicPort": {"integerValue": ")" + public_port + R"("},
        "lanIp": {"stringValue": ")" + lan_ip + R"("},
        "lanPort": {"integerValue": ")" + lan_port + R"("},
        "hasGame": {"booleanValue": true},
        "ready": {"booleanValue": true},
        "link": {"stringValue": "punching"},
        "rttMs": {"integerValue": "17"},
        "updatedAt": {"timestampValue": ")" + updated + R"("}
    }}})";
}

std::string lobby_json(const std::string& members) {
    return R"({"name": "projects/p/databases/(default)/documents/lobbies/ABC123",
               "fields": {"members": {"mapValue": {"fields": {)" + members +
           R"(}}}}})";
}

} // namespace

int main() {
    using namespace online_wire;

    // --- tokens ---------------------------------------------------------
    {
        check(expiry_from_expires_in(1000, "3600") == 4600,
              "expiresIn is a string of seconds");
        check(expiry_from_expires_in(1000, "") == 1000,
              "a missing lifetime counts as already expired");
        check(expiry_from_expires_in(1000, "not a number") == 1000,
              "an unreadable lifetime counts as already expired");
        check(expiry_from_expires_in(1000, "-5") == 1000,
              "a negative lifetime counts as already expired");

        token_state token;
        token.id_token = "t";
        token.refresh_token = "r";
        token.expires_unix = 1000 + 3600;
        check(token.usable(1000), "a fresh token is usable");
        check(!token.needs_refresh(1000), "and is not refreshed immediately");
        // Refreshed at fifty-five minutes, not at sixty-one.
        check(token.needs_refresh(1000 + 3600 - 299),
              "refresh happens before expiry, not after it");
        check(!token.needs_refresh(1000 + 3600 - 301),
              "and not a moment earlier than it has to");
        check(!token.usable(1000 + 3601), "an expired token is not usable");

        token_state fresh;
        check(!fresh.usable(1000), "an empty token is not usable");
        check(!fresh.needs_refresh(1000),
              "and with nothing to refresh with, there is nothing to do");
    }
    {
        check(next_after_unauthorized(0) == auth_decision::retry_with_refresh,
              "one 401 means refresh and try again");
        check(next_after_unauthorized(1) == auth_decision::sign_out,
              "two 401s means the credential is gone, not slow");
    }

    // --- typed values ----------------------------------------------------
    {
        // The one that bites: Firestore carries integers as decimal strings
        // in both directions, because JSON numbers cannot hold 64 bits.
        check(value_int(35109).at("integerValue").is_string(),
              "an integer is written as a string");
        check(value_int(35109).at("integerValue") == "35109",
              "and with the right digits");
        check(value_string("galaga").at("stringValue") == "galaga",
              "a string is a string");
        check(value_bool(true).at("booleanValue") == true, "a bool is a bool");
        check(rfc3339(0) == "1970-01-01T00:00:00Z",
              "timestamps are RFC 3339 in UTC");
        check(read_time(value_time(1700000000)) == 1700000000,
              "a timestamp survives the round trip");

        const auto list = value_strings({"galaga", "outrun"});
        check(list.at("arrayValue").at("values").size() == 2,
              "an array of short names encodes as an array value");

        const std::vector<uint8_t> binary{0x00, 0x01, 0xFE, 0xFF, 0x42};
        std::vector<uint8_t> decoded;
        check(read_bytes(value_bytes(binary), decoded) && decoded == binary,
              "Firestore bytes survive base64 round trip");
        check(!base64_decode("not-valid!", decoded),
              "malformed cloud-save base64 is rejected");
    }
    {
        check(read_int(nlohmann::json{{"integerValue", "35109"}}) == 35109,
              "the string form reads back as a number");
        // Belt and braces: a document written by one SDK and read through
        // another path has been seen carrying a real JSON number, and a port
        // that silently reads as zero is a machine nobody can reach.
        check(read_int(nlohmann::json{{"integerValue", 35109}}) == 35109,
              "the number form reads back too");
        check(read_int(nlohmann::json{{"stringValue", "35109"}}, -1) == -1,
              "the wrong type falls back rather than guessing");
        check(read_int(nlohmann::json::object(), 7) == 7,
              "a missing field falls back");
        check(read_string(nlohmann::json{{"integerValue", "1"}}, "x") == "x",
              "reading a string from an integer falls back");
        check(read_bool(nlohmann::json::object(), true) == true,
              "a missing bool falls back");
    }

    // --- parsing a lobby --------------------------------------------------
    {
        const std::string text = lobby_json(
            member_json("m1", "81.2.3.4", "35109", "192.168.1.5", "35109",
                        rfc3339(1700000000)) + "," +
            member_json("m2", "90.1.2.3", "40001", "192.168.1.9", "35109",
                        rfc3339(1700000000)));
        const nlohmann::json document =
            nlohmann::json::parse(text, nullptr, false);
        check(!document.is_discarded(), "the canned lobby document parses");

        const auto members = parse_members(document);
        check(members.size() == 2, "both members are found");
        const member* first = nullptr;
        for (const member& entry : members)
            if (entry.machine_uid == "m1") first = &entry;
        check(first != nullptr, "members are keyed by machine uid");
        if (first) {
            check(first->public_port == 35109, "the port survives the string");
            check(first->public_ip == "81.2.3.4", "so does the address");
            check(first->node == 2, "and the player number");
            check(first->rtt_ms == 17, "and the round trip time");
            check(first->has_game && first->ready, "and the flags");
            check(first->link == "punching", "and the link state");
        }

        // Ourselves excluded, both candidates published for everybody else.
        const auto peers =
            peers_from_members(members, "m1", 1700000000, fake_parse_ipv4);
        check(peers.size() == 2,
              "our own row is skipped and the other machine contributes its "
              "public and LAN addresses");
        bool has_public = false, has_lan = false;
        for (const auto& peer : peers) {
            if (peer.port == 40001 && peer.ipv4 == fake_parse_ipv4("90.1.2.3"))
                has_public = true;
            if (peer.port == 35109 && peer.ipv4 == fake_parse_ipv4("192.168.1.9"))
                has_lan = true;
        }
        check(has_public, "the public candidate is seeded");
        // Two machines behind one router see the same public address and
        // hairpinning is not universal, so the LAN candidate has to be there
        // as well or that pair silently fails.
        check(has_lan, "the LAN candidate is seeded too");
    }
    {
        // A cabinet that crashed leaves its entry behind; nothing else
        // deletes it. Punching at a dead address for ever is exactly what
        // "connecting..." looks like to a player.
        const std::string text = lobby_json(
            member_json("m1", "81.2.3.4", "35109", "", "0",
                        rfc3339(1700000000)) + "," +
            member_json("m2", "90.1.2.3", "40001", "", "0",
                        rfc3339(1700000000 - 600)));
        const nlohmann::json document =
            nlohmann::json::parse(text, nullptr, false);
        const auto peers = peers_from_members(parse_members(document), "m1",
                                              1700000000, fake_parse_ipv4);
        check(peers.empty(), "a member ten minutes stale is not punched at");
    }
    {
        const std::string text = lobby_json(
            member_json("m2", "", "0", "", "0", rfc3339(1700000000)));
        const nlohmann::json document =
            nlohmann::json::parse(text, nullptr, false);
        const auto peers = peers_from_members(parse_members(document), "m1",
                                              1700000000, fake_parse_ipv4);
        check(peers.empty(),
              "a member that has not measured its address yet is skipped");
    }
    {
        // Same address twice - a machine whose LAN and public candidates
        // coincide - must not be sent two identical hellos.
        const std::string text = lobby_json(
            member_json("m2", "81.2.3.4", "35109", "81.2.3.4", "35109",
                        rfc3339(1700000000)));
        const nlohmann::json document =
            nlohmann::json::parse(text, nullptr, false);
        const auto peers = peers_from_members(parse_members(document), "m1",
                                              1700000000, fake_parse_ipv4);
        check(peers.size() == 1, "duplicate candidates collapse to one");
    }
    {
        // Rubbish must not throw and must not become a peer.
        const nlohmann::json broken =
            nlohmann::json::parse("{\"fields\": {\"members\": 7}}", nullptr,
                                  false);
        check(!broken.is_discarded(), "the malformed document still parses");
        check(parse_members(broken).empty(),
              "a members field of the wrong shape yields nothing");
        const nlohmann::json rubbish =
            nlohmann::json::parse("not json at all", nullptr, false);
        check(rubbish.is_discarded(),
              "genuinely broken JSON is discarded, not thrown");
    }

    {
        // Handles. The rules refuse anything that is not already lower case,
        // and a name somebody has to punctuate exactly is a name nobody can
        // add as a friend.
        check(handle_for("Jon") == "jon", "a handle is lower case");
        check(handle_for("Jon Whittingham") == "jonwhittingham",
              "spaces are not part of a handle");
        check(handle_for("D.J. O'Neill-2") == "djoneill2",
              "punctuation is dropped rather than escaped");
        check(handle_for("").empty(), "an empty name has no handle");
        check(handle_for("!!!").empty(),
              "a name with nothing to keep has no handle");
        check(handle_for(std::string(40, 'a')).size() == 32,
              "a handle is capped at the length the rules allow");
    }
    {
        // The friendship id has to come out the same on both cabinets, or
        // each side writes a document the other never reads.
        check(friendship_id("alice", "bob") == "alice_bob",
              "sorted already, so joined as it stands");
        check(friendship_id("bob", "alice") == "alice_bob",
              "the same pair from the other side is the same document");
        check(friendship_id("2zz", "10a") == "10a_2zz",
              "uids sort as strings, digits and all");
    }
    {
        // What has appeared since last time. Asking twice about the same
        // lobby is worse than not asking at all.
        std::set<std::string> seen;
        check(first_unseen({"AAA", "BBB"}, seen, false).empty(),
              "the first sweep remembers and says nothing");
        check(seen.size() == 2, "and it really did remember them");
        check(first_unseen({"AAA", "BBB"}, seen, true).empty(),
              "nothing new is nothing to say");
        check(first_unseen({"AAA", "CCC"}, seen, true) == "CCC",
              "a lobby that was not there before is news");
        check(first_unseen({"AAA", "CCC"}, seen, true).empty(),
              "and it is only news once");
        check(first_unseen({"DDD", "EEE"}, seen, false).empty(),
              "an unprimed sweep still swallows what it sees");
        check(first_unseen({"DDD", "EEE"}, seen, true).empty(),
              "so those two are never announced afterwards");
        // A lobby with no id at all is a document whose name did not parse,
        // and offering somebody a lobby that cannot be joined is worse than
        // offering nothing.
        check(first_unseen({""}, seen, true).empty(), "an id-less row is skipped");
    }

    if (failures == 0) std::printf("online_wire_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
