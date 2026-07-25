#include "igdb_artwork.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>

namespace igdb {
namespace {

using json_document = nlohmann::json;

// Longest cache stem written to disk. Long enough for every catalog title and
// short enough that a stem plus its extension clears the filename limit on
// every filesystem WhittyArcade is installed on.
constexpr std::size_t stem_limit = 96;

json_document parse_document(std::string_view text) {
    // Non-throwing parse: a truncated or HTML error page from a captive portal
    // is an ordinary outcome here, not an exception-worthy one.
    return json_document::parse(text.begin(), text.end(), nullptr, false);
}

bool trimmable(unsigned char character) {
    return std::isspace(character) != 0;
}

// Letters and digits only, folded to lower case. Used for title equality so
// punctuation and spacing differences -- "Ghosts'n Goblins" against IGDB's
// "Ghosts 'n Goblins" -- still count as the same game.
std::string compact(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const unsigned char character : text) {
        if (std::isalnum(character))
            result.push_back(
                static_cast<char>(std::tolower(character)));
    }
    return result;
}

std::string percent_encoded(std::string_view text) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(text.size());
    for (const unsigned char character : text) {
        if (std::isalnum(character) || character == '-' || character == '_' ||
            character == '.' || character == '~') {
            result.push_back(static_cast<char>(character));
            continue;
        }
        result.push_back('%');
        result.push_back(digits[character >> 4]);
        result.push_back(digits[character & 0x0f]);
    }
    return result;
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && trimmable(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && trimmable(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

bool parse_epoch(std::string_view text, int64_t& value) {
    const std::string_view clean = trimmed(text);
    const auto result = std::from_chars(clean.data(),
                                        clean.data() + clean.size(), value);
    return result.ec == std::errc{} &&
           result.ptr == clean.data() + clean.size();
}

std::string_view key_of(std::string_view line, std::string_view& value) {
    const std::size_t separator = line.find('=');
    if (separator == std::string_view::npos) return {};
    value = line.substr(separator + 1);
    return trimmed(line.substr(0, separator));
}

} // namespace

std::string search_title(std::string_view menu_label) {
    // The catalog's display names already carry a parenthesised MAME qualifier,
    // and rom_library appends "(archive.zip)" plus "[ready]"-style tags, so the
    // first bracket of any kind is where the game's actual name ends.
    const std::size_t bracket = menu_label.find_first_of("([");
    const std::string_view head = bracket == std::string_view::npos ?
        menu_label : menu_label.substr(0, bracket);

    std::string title;
    title.reserve(head.size());
    bool space_pending = false;
    for (const unsigned char character : trimmed(head)) {
        if (trimmable(character)) {
            space_pending = true;
            continue;
        }
        if (space_pending && !title.empty()) title.push_back(' ');
        space_pending = false;
        title.push_back(static_cast<char>(character));
    }
    return title;
}

std::string cache_stem(std::string_view search_title) {
    std::string stem;
    stem.reserve(std::min(search_title.size(), stem_limit));
    bool separator_pending = false;
    for (const unsigned char character : search_title) {
        if (!std::isalnum(character)) {
            separator_pending = true;
            continue;
        }
        if (stem.size() >= stem_limit) break;
        if (separator_pending && !stem.empty()) stem.push_back('_');
        separator_pending = false;
        stem.push_back(static_cast<char>(std::tolower(character)));
    }
    return stem;
}

std::string oauth_url(std::string_view client_id,
                      std::string_view client_secret) {
    return "https://id.twitch.tv/oauth2/token?client_id=" +
           percent_encoded(client_id) + "&client_secret=" +
           percent_encoded(client_secret) + "&grant_type=client_credentials";
}

// IGDB platform ids for the hardware this program emulates: 52 is Arcade,
// 12 is Xbox 360 (the XBLA board). A board added on another platform needs its
// id here or its games will find no cover.
constexpr std::string_view emulated_platform_ids = "52,12";

std::string search_body(std::string_view search_title) {
    std::string escaped;
    escaped.reserve(search_title.size());
    for (const char character : search_title) {
        // An apostrophe is fine inside an Apicalypse string, but a quote or a
        // backslash would end or continue it and corrupt the whole query.
        if (character == '\\' || character == '"') escaped.push_back('\\');
        escaped.push_back(character);
    }
    // image_id is requested directly rather than cover.url: the url form is a
    // t_thumb link that would have to be rewritten to reach a menu-sized image.
    //
    // The platform filter matters more than it looks: unqualified, "Ridge
    // Racer" ranks the PlayStation conversion above the System 22 original and
    // the menu would show the wrong box art for a machine this program is
    // emulating. Restricting to the platforms actually emulated here - arcade,
    // plus Xbox 360 for the XBLA board - keeps a cover tied to the hardware the
    // cabinet really is. "= (...)" is Apicalypse for "this array contains", so
    // a game released on arcade and later ported still matches.
    return "search \"" + escaped + "\"; fields name,cover.image_id; where " +
           "platforms = (" + std::string(emulated_platform_ids) + "); limit 8;";
}

std::string fallback_title(std::string_view search_title) {
    // Only a subtitle is dropped, never a leading fragment: "Ace Driver:
    // Racing Evolution" becomes "Ace Driver", but a title that begins with a
    // separator yields nothing rather than an empty search.
    for (const std::string_view separator : {": ", " - "}) {
        const std::size_t at = search_title.find(separator);
        if (at != std::string_view::npos && at > 0)
            return std::string(search_title.substr(0, at));
    }
    return {};
}

std::string cover_url(std::string_view image_id) {
    // IGDB image ids are opaque tokens. Refuse anything else rather than paste
    // an arbitrary string into a URL path.
    const bool safe = !image_id.empty() &&
        std::all_of(image_id.begin(), image_id.end(), [](char character) {
            const auto value = static_cast<unsigned char>(character);
            return std::isalnum(value) != 0 || character == '_' ||
                   character == '-';
        });
    if (!safe) return {};
    return "https://images.igdb.com/igdb/image/upload/t_cover_big/" +
           std::string(image_id) + ".jpg";
}

bool token_usable(const oauth_token& token, int64_t now_seconds) {
    return !token.value.empty() && now_seconds < token.expires_at;
}

bool parse_token_response(std::string_view json, int64_t now_seconds,
                          oauth_token& token) {
    const json_document document = parse_document(json);
    if (!document.is_object()) return false;
    const auto access = document.find("access_token");
    if (access == document.end() || !access->is_string()) return false;
    std::string value = access->get<std::string>();
    if (value.empty()) return false;

    int64_t lifetime = 0;
    if (const auto expires = document.find("expires_in");
        expires != document.end() && expires->is_number_integer())
        lifetime = expires->get<int64_t>();

    // Retire the token five minutes early. One that expires mid-request costs a
    // failed lookup, which the negative cache would then remember as a miss and
    // hold for a fortnight.
    constexpr int64_t early_refresh_seconds = 300;
    token.value = std::move(value);
    token.expires_at =
        now_seconds + std::max<int64_t>(0, lifetime - early_refresh_seconds);
    return true;
}

bool parse_stored_token(std::string_view text, oauth_token& token) {
    oauth_token parsed;
    while (!text.empty()) {
        const std::size_t break_at = text.find('\n');
        const std::string_view line = text.substr(0, break_at);
        text = break_at == std::string_view::npos ? std::string_view{} :
                                                    text.substr(break_at + 1);
        std::string_view value;
        const std::string_view key = key_of(line, value);
        if (key == "access_token") parsed.value = std::string(trimmed(value));
        else if (key == "expires_at") parse_epoch(value, parsed.expires_at);
    }
    if (parsed.value.empty()) return false;
    token = std::move(parsed);
    return true;
}

std::string format_stored_token(const oauth_token& token) {
    return "access_token=" + token.value + "\nexpires_at=" +
           std::to_string(token.expires_at) + "\n";
}

std::string choose_cover_image_id(std::string_view json,
                                  std::string_view wanted_title) {
    const json_document document = parse_document(json);
    if (!document.is_array()) return {};
    const std::string wanted = compact(wanted_title);
    std::string best_ranked;
    for (const json_document& entry : document) {
        if (!entry.is_object()) continue;
        const auto cover = entry.find("cover");
        if (cover == entry.end() || !cover->is_object()) continue;
        const auto image = cover->find("image_id");
        if (image == cover->end() || !image->is_string()) continue;
        std::string image_id = image->get<std::string>();
        if (image_id.empty()) continue;

        const auto name = entry.find("name");
        if (!wanted.empty() && name != entry.end() && name->is_string() &&
            compact(name->get<std::string>()) == wanted)
            return image_id;
        // IGDB returns its own relevance order, so the first covered entry is
        // the fallback when no title matches exactly.
        if (best_ranked.empty()) best_ranked = std::move(image_id);
    }
    return best_ranked;
}

miss_index parse_miss_index(std::string_view text) {
    miss_index misses;
    while (!text.empty()) {
        const std::size_t break_at = text.find('\n');
        const std::string_view line = text.substr(0, break_at);
        text = break_at == std::string_view::npos ? std::string_view{} :
                                                    text.substr(break_at + 1);
        std::string_view value;
        const std::string_view key = key_of(line, value);
        int64_t recorded_at = 0;
        if (key.empty() || !parse_epoch(value, recorded_at) || recorded_at <= 0)
            continue;
        misses[std::string(key)] = recorded_at;
    }
    return misses;
}

std::string format_miss_index(const miss_index& misses, int64_t now_seconds) {
    std::string text;
    for (const auto& [stem, recorded_at] : misses) {
        if (stem.empty() ||
            now_seconds - recorded_at >= miss_lifetime_seconds)
            continue;
        text += stem;
        text += '=';
        text += std::to_string(recorded_at);
        text += '\n';
    }
    return text;
}

bool miss_recorded(const miss_index& misses, const std::string& stem,
                   int64_t now_seconds) {
    const auto found = misses.find(stem);
    return found != misses.end() &&
           now_seconds - found->second < miss_lifetime_seconds;
}

int64_t epoch_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace igdb
