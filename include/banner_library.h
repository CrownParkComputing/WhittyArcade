// banner_library.h - board photos and publisher logos from Wikimedia.
//
// A board page's banner is the article's lead image (a PCB photo for the
// boards this program cares about); a publisher's is its logo from Wikimedia
// Commons. Fetched in the background, cached on disk, decoded to RGBA -
// the same shape as the bezel and cover libraries, and like them a page
// whose artwork has not arrived simply shows text until it does.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace banner {

struct banner_image {
    std::vector<uint8_t> rgba;
    int width{};
    int height{};
    bool valid() const { return !rgba.empty() && width > 0 && height > 0; }
};

struct ready_banner {
    std::string key;
    banner_image image;
};

class library {
public:
    library();
    ~library();

    library(const library&) = delete;
    library& operator=(const library&) = delete;

    // Queues one banner. kind_commons_logo looks up "File:<title> logo.svg"
    // on Wikimedia Commons (publishers); kind_article takes the lead image
    // of the named Wikipedia article (boards). Repeat requests for a key
    // already queued, fetched or known missing cost nothing.
    enum class kind { article, commons_logo };
    void request(const std::string& key, kind source,
                 const std::string& title);

    // Banners finished since the last call; the caller keeps the pixels.
    std::vector<ready_banner> take_ready();

private:
    struct worker;
    std::unique_ptr<worker> m_worker;
};

} // namespace banner
