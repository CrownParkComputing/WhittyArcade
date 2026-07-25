#include "banner_library.h"

#include "platform_paths.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <cstdio>
#include <thread>

#if defined(WHITTY_HAVE_CURL)
#include <curl/curl.h>
#endif

#include "json.hpp"
#include "stb_image.h"

namespace fs = std::filesystem;

namespace banner {
namespace {

fs::path banner_root() {
    const fs::path root = whitty_platform::data_root();
    return (root.empty() ? fs::current_path() : root) / "WhittyArcade" /
           "artwork" / "banners";
}

std::string safe_name(const std::string& key) {
    std::string out;
    for (const char c : key)
        out.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return out;
}

std::string url_encode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out += "%20";
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

#if defined(WHITTY_HAVE_CURL)
std::size_t collect(char* data, std::size_t size, std::size_t count,
                    void* sink) {
    static_cast<std::string*>(sink)->append(data, size * count);
    return size * count;
}

std::string fetch(const std::string& url) {
    CURL* handle = curl_easy_init();
    if (!handle) return {};
    std::string body;
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT,
                     "WhittyArcade (arcade cabinet frontend)");
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
    long status = 0;
    const CURLcode result = curl_easy_perform(handle);
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(handle);
    if (result != CURLE_OK || status != 200) return {};
    return body;
}
#else
std::string fetch(const std::string&) { return {}; }
#endif

// The image URL a request resolves to, or empty. Both endpoints answer JSON;
// both give a raster thumbnail even when the source file is an SVG logo.
std::string resolve_image_url(library::kind source,
                              const std::string& title) {
    std::string json_text;
    if (source == library::kind::commons_logo) {
        json_text = fetch(
            "https://commons.wikimedia.org/w/api.php?action=query&titles="
            "File:" + url_encode(title + " logo.svg") +
            "&prop=imageinfo&iiprop=url&iiurlwidth=640&format=json");
    } else {
        std::string article = title;
        for (char& c : article)
            if (c == ' ') c = '_';
        json_text = fetch(
            "https://en.wikipedia.org/api/rest_v1/page/summary/" +
            url_encode(article));
    }
    if (json_text.empty()) return {};
    const nlohmann::json parsed =
        nlohmann::json::parse(json_text, nullptr, false);
    if (parsed.is_discarded()) return {};
    if (source == library::kind::commons_logo) {
        const auto pages = parsed.find("query");
        if (pages == parsed.end()) return {};
        const auto page_map = pages->find("pages");
        if (page_map == pages->end()) return {};
        for (const auto& page : *page_map) {
            const auto info = page.find("imageinfo");
            if (info == page.end() || !info->is_array() || info->empty())
                continue;
            const auto url = info->front().find("thumburl");
            if (url != info->front().end() && url->is_string())
                return url->get<std::string>();
        }
        return {};
    }
    const auto thumbnail = parsed.find("thumbnail");
    if (thumbnail == parsed.end()) return {};
    const auto url = thumbnail->find("source");
    return url != thumbnail->end() && url->is_string()
               ? url->get<std::string>()
               : std::string();
}

bool decode(const std::string& bytes, banner_image& out) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.data()),
        static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (!pixels) return false;
    out.width = width;
    out.height = height;
    out.rgba.assign(pixels,
                    pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}

} // namespace

struct library::worker {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    std::atomic<bool> stop{false};
    struct job {
        std::string key;
        kind source;
        std::string title;
    };
    std::deque<job> queue;
    std::set<std::string> seen;
    std::vector<ready_banner> ready;
    bool started{false};

    ~worker() {
        stop.store(true);
        wake.notify_all();
        if (thread.joinable()) thread.join();
    }

    void run() {
        for (;;) {
            job next;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return stop.load() ||
                                                !queue.empty(); });
                if (stop.load()) return;
                next = std::move(queue.front());
                queue.pop_front();
            }
            const fs::path cached =
                banner_root() / (safe_name(next.key) + ".img");
            std::string bytes;
            {
                std::ifstream input(cached, std::ios::binary);
                if (input)
                    bytes.assign(std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>());
            }
            if (bytes.empty()) {
                const std::string url =
                    resolve_image_url(next.source, next.title);
                if (stop.load()) return;
                if (url.empty()) {
                    std::fprintf(stderr,
                                 "banner: no image found for %s (%s)\n",
                                 next.key.c_str(), next.title.c_str());
                    continue;
                }
                bytes = fetch(url);
                if (stop.load()) return;
                if (bytes.empty()) {
                    std::fprintf(stderr, "banner: download failed for %s\n",
                                 next.key.c_str());
                    continue;
                }
                std::error_code error;
                fs::create_directories(cached.parent_path(), error);
                std::ofstream output(cached, std::ios::binary);
                output.write(bytes.data(),
                             static_cast<std::streamsize>(bytes.size()));
            }
            ready_banner decoded;
            decoded.key = next.key;
            if (!decode(bytes, decoded.image)) {
                std::fprintf(stderr, "banner: undecodable image for %s\n",
                             next.key.c_str());
                continue;
            }
            std::lock_guard<std::mutex> lock(mutex);
            ready.push_back(std::move(decoded));
        }
    }
};

library::library() : m_worker(std::make_unique<worker>()) {}
library::~library() = default;

void library::request(const std::string& key, kind source,
                      const std::string& title) {
#if !defined(WHITTY_HAVE_CURL)
    (void)key; (void)source; (void)title;
    return;
#else
    if (key.empty() || title.empty()) return;
    std::lock_guard<std::mutex> lock(m_worker->mutex);
    if (!m_worker->seen.insert(key).second) return;
    m_worker->queue.push_back({key, source, title});
    if (!m_worker->started) {
        m_worker->started = true;
        m_worker->thread = std::thread([this] { m_worker->run(); });
    }
    m_worker->wake.notify_one();
#endif
}

std::vector<ready_banner> library::take_ready() {
    std::lock_guard<std::mutex> lock(m_worker->mutex);
    return std::exchange(m_worker->ready, {});
}

} // namespace banner
