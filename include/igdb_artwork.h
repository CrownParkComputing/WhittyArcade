// igdb_artwork.h - cover artwork for the game menu, sourced from IGDB.
//
// Artwork is decoration, never a dependency. Every entry point here returns
// immediately, all HTTP and disk work happens on one worker thread, and results
// are cached on disk so a game is looked up at most once. A machine with no
// network, no credentials or no HTTP backend compiled in therefore degrades to
// exactly one behaviour: the menu draws its text rows and no cover appears.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace igdb {

// ---------------------------------------------------------------------------
// Pure helpers. All the parsing and cache-decision logic lives here so it can
// be exercised without a network, a filesystem or a real clock.
// ---------------------------------------------------------------------------

// Menu rows carry a MAME-style hardware qualifier, the archive name and a
// readiness tag -- "Ridge Racer (World, RR2 Ver.B)  (ridgerac.zip)  [ready]".
// IGDB knows none of that, so everything from the first bracket onwards goes.
std::string search_title(std::string_view menu_label);

// Filesystem-safe cache stem. Derived from the search title rather than the
// menu label so the same game reached through a different archive, board or
// readiness state resolves to one cached file.
std::string cache_stem(std::string_view search_title);

std::string oauth_url(std::string_view client_id,
                      std::string_view client_secret);
std::string search_body(std::string_view search_title);

// A shorter form to retry with when the full title finds nothing. Arcade
// releases often carry a subtitle IGDB does not use - it lists "Ace Driver",
// not "Ace Driver: Racing Evolution" - so the text before the first colon or
// dash is worth one more lookup. Empty when the title has no such split, which
// means there is nothing further to try.
std::string fallback_title(std::string_view search_title);
std::string cover_url(std::string_view image_id);

struct oauth_token {
    std::string value;
    // Absolute expiry, already pulled in from the reported lifetime so a token
    // cannot lapse between the validity check and the request that uses it.
    int64_t expires_at{};
};

bool token_usable(const oauth_token& token, int64_t now_seconds);
bool parse_token_response(std::string_view json, int64_t now_seconds,
                          oauth_token& token);
bool parse_stored_token(std::string_view text, oauth_token& token);
std::string format_stored_token(const oauth_token& token);

// Cover image id worth downloading from an IGDB /v4/games response. An exact
// title match beats search ranking, so "Galaxian" cannot settle for
// "Galaxian 3" merely because IGDB scored it higher.
std::string choose_cover_image_id(std::string_view json,
                                  std::string_view wanted_title);

// Negative cache. IGDB has no entry for a fair number of arcade sets, and
// re-asking for every one of them on every launch is the one thing that would
// turn this decoration into API abuse.
using miss_index = std::map<std::string, int64_t>;
constexpr int64_t miss_lifetime_seconds = 14 * 24 * 60 * 60;

miss_index parse_miss_index(std::string_view text);
// Expired entries are dropped while serialising, so the file cannot grow
// without bound as a library gains games IGDB has never heard of.
std::string format_miss_index(const miss_index& misses, int64_t now_seconds);
bool miss_recorded(const miss_index& misses, const std::string& stem,
                   int64_t now_seconds);

int64_t epoch_seconds();

// ---------------------------------------------------------------------------
// Background fetcher
// ---------------------------------------------------------------------------

// Decoded RGBA8888 cover, at the size IGDB delivered it.
struct cover_image {
    int width{};
    int height{};
    std::vector<uint8_t> pixels;

    bool valid() const {
        return width > 0 && height > 0 &&
               pixels.size() == static_cast<std::size_t>(width) *
                                    static_cast<std::size_t>(height) * 4;
    }
};

struct ready_cover {
    std::string menu_label;
    cover_image image;
};

class cover_library {
public:
    // ready runs on the worker thread each time a cover becomes available, so
    // an event-driven menu can wake its own blocking event wait instead of
    // polling for arrivals. It must be cheap and thread-safe; SDL_PushEvent is
    // both. Nothing else in this class calls back into the caller.
    explicit cover_library(std::function<void()> ready = {});
    ~cover_library();

    cover_library(const cover_library&) = delete;
    cover_library& operator=(const cover_library&) = delete;

    // False when this build can never produce a cover: no HTTP backend was
    // compiled in, or no IGDB application credentials were configured. Menus
    // use it to avoid reserving space for artwork that cannot arrive.
    static bool configured();

    // Queues menu labels in the order they should be fetched. Labels already
    // queued are ignored, so re-entering a menu costs nothing. The worker
    // thread starts on the first request and not before.
    void request(const std::vector<std::string>& menu_labels);

    // Hands over the covers finished since the last call and forgets them: the
    // caller owns the pixels and is expected to upload them once.
    std::vector<ready_cover> take_ready();

private:
    struct worker;
    std::unique_ptr<worker> m_worker;
};

} // namespace igdb
