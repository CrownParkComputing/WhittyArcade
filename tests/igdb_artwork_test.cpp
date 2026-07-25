// Hermetic tests for the pure IGDB artwork helpers: menu-label parsing,
// cache-stem derivation, URL/body construction, OAuth token (de)serialising,
// IGDB response parsing and the negative-cache (miss index) decisions. None
// of this touches the network, a filesystem or a real clock, so it runs the
// same everywhere ctest does.
#include "igdb_artwork.h"

#include <cstdint>
#include <string>

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

int main() {
    // -----------------------------------------------------------------
    // search_title: menu label -> the game's plain title.
    // -----------------------------------------------------------------
    if (igdb::search_title(
            "Ridge Racer (World, RR2 Ver.B)  (ridgerac.zip)  [ready]") !=
        "Ridge Racer")
        return 1;
    if (igdb::search_title("Ghosts'n Goblins") != "Ghosts'n Goblins") return 2;
    if (igdb::search_title("Foo [Not Working]") != "Foo") return 3;
    // Interior whitespace runs collapse to one space; edges are trimmed.
    if (igdb::search_title("  Pac-Man   Deluxe   (set 1)") != "Pac-Man Deluxe")
        return 4;
    if (!igdb::search_title("").empty()) return 5;
    if (!igdb::search_title("   ").empty()) return 6;
    // A label that is nothing but a qualifier has no title at all.
    if (!igdb::search_title("(bad set)").empty()) return 7;

    // -----------------------------------------------------------------
    // cache_stem: search title -> filesystem-safe stem.
    // -----------------------------------------------------------------
    if (igdb::cache_stem("Ridge Racer") != "ridge_racer") return 10;
    // Punctuation (including an apostrophe) becomes a single separator, and
    // never leaks a leading or trailing underscore.
    if (igdb::cache_stem("Ghosts'n Goblins") != "ghosts_n_goblins") return 11;
    if (igdb::cache_stem("  Foo") != "foo") return 12;
    if (igdb::cache_stem("Foo!!!") != "foo") return 13;
    if (igdb::cache_stem("Foo___Bar") != "foo_bar") return 14;
    if (!igdb::cache_stem("").empty()) return 15;
    if (!igdb::cache_stem("###").empty()) return 16;
    // Long titles are capped (stem_limit is 96 in igdb_artwork.cpp) so a
    // cache filename can never exceed a filesystem's name-length limit.
    {
        const std::string long_title(200, 'a');
        const std::string stem = igdb::cache_stem(long_title);
        if (stem.size() != 96) return 17;
        if (stem != std::string(96, 'a')) return 18;
    }

    // -----------------------------------------------------------------
    // oauth_url / search_body / cover_url: request construction.
    // -----------------------------------------------------------------
    if (igdb::oauth_url("abc", "xyz") !=
        "https://id.twitch.tv/oauth2/token?client_id=abc&client_secret=xyz"
        "&grant_type=client_credentials")
        return 20;
    // Percent-encoding covers anything that is not alnum/-/_/./~.
    if (igdb::oauth_url("a b", "c&d") !=
        "https://id.twitch.tv/oauth2/token?client_id=a%20b&client_secret="
        "c%26d&grant_type=client_credentials")
        return 21;

    if (igdb::search_body("Pac-Man") !=
        "search \"Pac-Man\"; fields name,cover.image_id; "
        "where platforms = (52,12); limit 8;")
        return 22;
    {
        // A quote or a backslash in the title must not break out of the
        // Apicalypse string.
        const std::string title = std::string("Foo\"Bar\\Baz");
        const std::string body = igdb::search_body(title);
        if (body != "search \"Foo\\\"Bar\\\\Baz\"; fields name,cover.image_id; "
                    "where platforms = (52,12); limit 8;")
            return 23;
    }

    // A subtitle IGDB does not carry is worth one shorter retry.
    if (igdb::fallback_title("Ace Driver: Racing Evolution") != "Ace Driver")
        return 24;
    if (igdb::fallback_title("Ace Driver - Victory Lap") != "Ace Driver")
        return 25;
    // Nothing to fall back to, so nothing is attempted.
    if (!igdb::fallback_title("Galaxian").empty()) return 26;
    // A leading separator must not produce an empty search.
    if (!igdb::fallback_title(": Subtitle Only").empty()) return 27;

    if (igdb::cover_url("co1abc") !=
        "https://images.igdb.com/igdb/image/upload/t_cover_big/co1abc.jpg")
        return 30;
    if (!igdb::cover_url("").empty()) return 31;
    // Image ids are opaque tokens: a dot, slash or anything else that could
    // escape the path segment is refused rather than pasted into a URL.
    if (!igdb::cover_url("co1.jpg").empty()) return 32;
    if (!igdb::cover_url("../secret").empty()) return 33;
    if (!igdb::cover_url("has space").empty()) return 34;
    if (igdb::cover_url("co1_ABC-9").empty()) return 35;

    // -----------------------------------------------------------------
    // oauth_token / token_usable.
    // -----------------------------------------------------------------
    {
        igdb::oauth_token token;
        if (igdb::token_usable(token, 1000)) return 40; // empty value
        token.value = "tok";
        token.expires_at = 999;
        if (igdb::token_usable(token, 1000)) return 41; // already expired
        token.expires_at = 1001;
        if (!igdb::token_usable(token, 1000)) return 42; // still good
        if (igdb::token_usable(token, 1001)) return 43; // boundary: not < now
    }

    // -----------------------------------------------------------------
    // parse_token_response: Twitch OAuth response -> oauth_token.
    // -----------------------------------------------------------------
    {
        igdb::oauth_token token;
        if (!igdb::parse_token_response(
                R"({"access_token":"tok123","expires_in":3600})", 1000, token))
            return 50;
        if (token.value != "tok123") return 51;
        // Retired 5 minutes early: 1000 + (3600 - 300).
        if (token.expires_at != 1000 + 3300) return 52;
    }
    {
        // No expires_in: lifetime is treated as zero, so the early-refresh
        // clamp bottoms out at zero rather than going negative.
        igdb::oauth_token token;
        if (!igdb::parse_token_response(R"({"access_token":"tok"})", 500,
                                        token))
            return 53;
        if (token.expires_at != 500) return 54;
    }
    {
        igdb::oauth_token token;
        if (igdb::parse_token_response(R"({"expires_in":3600})", 0, token))
            return 55; // missing access_token
        if (igdb::parse_token_response(R"({"access_token":""})", 0, token))
            return 56; // empty access_token
        if (igdb::parse_token_response("not json", 0, token)) return 57;
        if (igdb::parse_token_response("[1,2,3]", 0, token)) return 58;
    }

    // -----------------------------------------------------------------
    // parse_stored_token / format_stored_token: on-disk token round trip.
    // -----------------------------------------------------------------
    {
        igdb::oauth_token token;
        token.value = "abc123";
        token.expires_at = 987654;
        const std::string stored = igdb::format_stored_token(token);
        if (!contains(stored, "access_token=abc123")) return 60;
        if (!contains(stored, "expires_at=987654")) return 61;

        igdb::oauth_token parsed;
        if (!igdb::parse_stored_token(stored, parsed)) return 62;
        if (parsed.value != token.value) return 63;
        if (parsed.expires_at != token.expires_at) return 64;
    }
    {
        igdb::oauth_token parsed;
        // Unknown keys are ignored, and whitespace around a value is
        // trimmed.
        if (!igdb::parse_stored_token(
                "future_field=1\naccess_token= abc \nexpires_at=42\n", parsed))
            return 65;
        if (parsed.value != "abc") return 66;
        if (parsed.expires_at != 42) return 67;
    }
    {
        igdb::oauth_token parsed;
        // No access_token line at all: nothing usable was stored.
        if (igdb::parse_stored_token("expires_at=42\n", parsed)) return 68;
    }
    {
        igdb::oauth_token parsed;
        // A malformed expires_at line leaves the field at its default rather
        // than failing the whole parse -- the token can still be tried and a
        // 401 will drive re-authentication.
        if (!igdb::parse_stored_token("access_token=abc\nexpires_at=nope\n",
                                      parsed))
            return 69;
        if (parsed.expires_at != 0) return 70;
    }

    // -----------------------------------------------------------------
    // choose_cover_image_id: IGDB /v4/games response -> best cover.
    // -----------------------------------------------------------------
    {
        // An exact title match wins even when it is not IGDB's top result.
        const std::string json =
            R"([{"name":"Galaxian 3","cover":{"image_id":"cover3"}},)"
            R"({"name":"Galaxian","cover":{"image_id":"coverExact"}}])";
        if (igdb::choose_cover_image_id(json, "Galaxian") != "coverExact")
            return 80;
    }
    {
        // No exact match: fall back to the first covered entry.
        const std::string json =
            R"([{"name":"Something Else","cover":{"image_id":"cover1"}}])";
        if (igdb::choose_cover_image_id(json, "Galaxian") != "cover1")
            return 81;
    }
    {
        // Entries without a cover, or a cover without an image id, are
        // skipped entirely rather than treated as a fallback.
        const std::string json =
            R"([{"name":"No Cover"},)"
            R"({"name":"Empty Cover","cover":{}},)"
            R"({"name":"Blank Image","cover":{"image_id":""}},)"
            R"({"name":"Real","cover":{"image_id":"coverReal"}}])";
        if (igdb::choose_cover_image_id(json, "Nothing Matches") != "coverReal")
            return 82;
    }
    {
        // Title matching folds case, spacing and punctuation.
        const std::string json =
            R"([{"name":"Ghosts 'N Goblins!","cover":{"image_id":"cx"}}])";
        if (igdb::choose_cover_image_id(json, "ghosts'n   goblins") != "cx")
            return 83;
    }
    {
        // An empty wanted title never counts as an exact match.
        const std::string json =
            R"([{"name":"","cover":{"image_id":"cy"}}])";
        if (igdb::choose_cover_image_id(json, "") != "cy") return 84;
    }
    if (!igdb::choose_cover_image_id("not json", "Anything").empty())
        return 85;
    if (!igdb::choose_cover_image_id(R"({"not":"an array"})", "Anything")
             .empty())
        return 86;
    if (!igdb::choose_cover_image_id("[]", "Anything").empty()) return 87;

    // -----------------------------------------------------------------
    // Negative cache: parse_miss_index / format_miss_index / miss_recorded.
    // -----------------------------------------------------------------
    {
        const igdb::miss_index parsed =
            igdb::parse_miss_index("stem_one=100\nstem_two=200\n");
        if (parsed.size() != 2) return 90;
        if (parsed.at("stem_one") != 100) return 91;
        if (parsed.at("stem_two") != 200) return 92;
    }
    {
        // Malformed or non-positive entries are dropped while parsing.
        const igdb::miss_index parsed = igdb::parse_miss_index(
            "=100\nstem_bad=notanumber\nstem_zero=0\nstem_neg=-5\n"
            "stem_ok=50\n");
        if (parsed.size() != 1) return 93;
        if (parsed.at("stem_ok") != 50) return 94;
    }
    {
        igdb::miss_index misses{{"fresh", 900}, {"stale", 100}};
        const int64_t now = 900 + igdb::miss_lifetime_seconds - 1;
        const std::string formatted = igdb::format_miss_index(misses, now);
        if (!contains(formatted, "fresh=900")) return 95;
        // "stale" recorded at 100 is older than the fortnight cutoff by now.
        if (contains(formatted, "stale")) return 96;
    }
    {
        const igdb::miss_index misses{{"seen", 1000}};
        if (!igdb::miss_recorded(misses, "seen", 1000)) return 97;
        if (!igdb::miss_recorded(
                misses, "seen", 1000 + igdb::miss_lifetime_seconds - 1))
            return 98;
        if (igdb::miss_recorded(misses, "seen",
                                1000 + igdb::miss_lifetime_seconds))
            return 99;
        if (igdb::miss_recorded(misses, "unseen", 1000)) return 100;
    }

    // -----------------------------------------------------------------
    // cover_image::valid().
    // -----------------------------------------------------------------
    {
        igdb::cover_image image;
        if (image.valid()) return 110; // zero-sized by default
        image.width = 2;
        image.height = 2;
        image.pixels.assign(16, 0);
        if (!image.valid()) return 111;
        image.pixels.assign(15, 0); // one byte short of 2x2x4
        if (image.valid()) return 112;
    }

    // -----------------------------------------------------------------
    // epoch_seconds: sanity check only -- this is the one function that
    // touches a real clock, so all it can assert is "looks like now".
    // -----------------------------------------------------------------
    {
        const int64_t now = igdb::epoch_seconds();
        if (now < 1'700'000'000) return 120; // before this code was written
    }

    // -----------------------------------------------------------------
    // cover_library::configured(): credentials come from the environment
    // ahead of whatever (if anything) CMake compiled in, and an empty
    // client id or secret means the whole path is inert.
    // -----------------------------------------------------------------
    // Deliberately not asserted here: whether this build has compiled-in
    // credentials is a CMake configure-time choice this test does not
    // control, and mutating WHITTY_IGDB_CLIENT_ID/SECRET process-wide would
    // race any other test that happens to run in the same process image.
    (void)igdb::cover_library::configured();

    return 0;
}
