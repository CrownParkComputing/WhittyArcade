// bezel_library.h - the background half of the cabinet-bezel path.
//
// Same contract as igdb::cover_library, and for the same reason: a bezel is
// decoration and must never hold a launch back. request() returns immediately,
// all HTTP and disk work happens on one worker thread, and a fetched bezel is
// cached on disk so a game is looked up at most once. A machine with no
// network, or a game The Bezel Project has never published, degrades to
// exactly one behaviour - the game draws the way it always did.
//
// The pure helpers this builds on (URL, cache path, cutout detection) live in
// bezel_artwork.h and are unit-tested without a network or a filesystem.
#pragma once

#include "bezel_artwork.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bezel {

// A decoded bezel and the viewport the game belongs in.
struct ready_bezel {
    std::string short_name;
    std::vector<uint8_t> rgba;
    int width{};
    int height{};
    cutout window;
};

class library {
public:
    library();
    ~library();

    library(const library&) = delete;
    library& operator=(const library&) = delete;

    // False when this build can never produce a bezel, so callers can skip
    // reserving anything for artwork that cannot arrive.
    static bool configured();

    // Queues one MAME short name. Repeat requests for a name already fetched,
    // queued or known missing cost nothing, so calling this on every board
    // change is fine. The worker thread starts on the first request.
    void request(const std::string& short_name);

    // Hands over the bezels finished since the last call and forgets them:
    // the caller owns the pixels and is expected to upload them once.
    std::vector<ready_bezel> take_ready();

private:
    struct worker;
    std::unique_ptr<worker> m_worker;
};

} // namespace bezel
