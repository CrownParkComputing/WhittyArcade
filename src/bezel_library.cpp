// bezel_library.cpp - fetches cabinet bezels from The Bezel Project and keeps
// them on disk.
//
// The whole point is that nothing here is on the critical path: the worker
// owns the socket and the decoder, the game thread only ever pushes a name in
// and pulls finished pixels out.

#include "bezel_library.h"

#include "platform_paths.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#if defined(MANX_HAVE_CURL)
#include <curl/curl.h>
#endif

// The decoder itself is instantiated once, in src/stb_image_impl.cpp.
#include "stb_image.h"

namespace fs = std::filesystem;

namespace bezel {
namespace {

fs::path bezel_root() {
    const fs::path root = manx_platform::data_root();
    return (root.empty() ? fs::current_path() : root) / "MANX" /
           "artwork" / "bezels";
}

// Games The Bezel Project has no bezel for are remembered, so a board without
// one is not re-fetched on every launch.
fs::path miss_index_path() { return bezel_root() / "missing.txt"; }

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

// Written to a neighbouring temporary and renamed: a fetch interrupted
// mid-download would otherwise leave a truncated PNG that decodes to garbage
// and is then cached forever.
bool write_file_atomically(const fs::path& path, const std::string& bytes) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    const fs::path temporary = path.string() + ".part";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output.good()) return false;
    }
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(temporary, error);
        return false;
    }
    return true;
}

bool decode_bezel(const std::string& bytes, ready_bezel& out) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.data()),
        static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (!pixels) return false;
    out.width = width;
    out.height = height;
    out.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    out.window = find_cutout(out.rgba.data(), width, height);
    // A bezel with no usable opening is worse than none: the game would be
    // squeezed into whatever transparent detail was found. Treat it as a miss.
    return out.window.valid;
}

} // namespace

struct library::worker {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::string> queue;
    std::set<std::string> seen;
    std::vector<ready_bezel> ready;
    std::atomic<bool> stop{false};
    bool started{false};

    ~worker() {
        stop.store(true);
        wake.notify_all();
        if (thread.joinable()) thread.join();
    }

    // Names already known to have no published bezel.
    std::set<std::string> load_misses() {
        std::set<std::string> misses;
        std::ifstream input(miss_index_path());
        std::string line;
        while (std::getline(input, line))
            if (!line.empty()) misses.insert(line);
        return misses;
    }

    void remember_miss(const std::string& short_name) {
        std::error_code error;
        fs::create_directories(bezel_root(), error);
        std::ofstream output(miss_index_path(), std::ios::app);
        if (output) output << short_name << '\n';
    }

    void run() {
        std::set<std::string> misses = load_misses();
        while (!stop.load()) {
            std::string short_name;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] {
                    return stop.load() || !queue.empty();
                });
                if (stop.load()) return;
                short_name = std::move(queue.front());
                queue.pop_front();
            }
            // A board already known to have no published artwork still gets
            // a surround - it simply skips the network entirely.
            const bool known_missing = misses.count(short_name) != 0;

            const fs::path cached =
                fs::path(artwork_path(bezel_root().string(), short_name));
            std::string bytes = known_missing ? std::string() : read_file(cached);
            bool publish_generated = known_missing;
            if (!known_missing && bytes.empty()) {
                bytes = fetch(artwork_url(short_name));
                if (stop.load()) return;
                if (bytes.empty()) {
                    // Remembered so it is never fetched again, but the board
                    // is still framed: The Bezel Project's arcade collection
                    // has nothing at all for the 3D-era boards, and a wall
                    // where only half the cabinets have surrounds looks
                    // broken rather than authentic.
                    misses.insert(short_name);
                    remember_miss(short_name);
                    publish_generated = true;
                } else {
                    write_file_atomically(cached, bytes);
                }
            }
            ready_bezel decoded;
            decoded.short_name = short_name;
            if (!publish_generated && !decode_bezel(bytes, decoded)) {
                misses.insert(short_name);
                remember_miss(short_name);
                publish_generated = true;
            }
            if (publish_generated) {
                constexpr int art_width = 1920;
                constexpr int art_height = 1440;
                decoded.rgba = generated_bezel(art_width, art_height, 4, 3);
                if (decoded.rgba.empty()) continue;
                decoded.width = art_width;
                decoded.height = art_height;
                decoded.window =
                    find_cutout(decoded.rgba.data(), art_width, art_height);
                if (!decoded.window.valid) continue;
            }
            std::lock_guard<std::mutex> lock(mutex);
            ready.push_back(std::move(decoded));
        }
    }

#if defined(MANX_HAVE_CURL)
    static std::size_t append(char* data, std::size_t size, std::size_t count,
                              void* userdata) {
        auto* body = static_cast<std::string*>(userdata);
        body->append(data, size * count);
        return size * count;
    }

    static int abort_when_stopping(void* userdata, curl_off_t, curl_off_t,
                                   curl_off_t, curl_off_t) {
        return static_cast<std::atomic<bool>*>(userdata)->load() ? 1 : 0;
    }

    std::string fetch(const std::string& url) {
        CURL* handle = curl_easy_init();
        if (!handle) return {};
        std::string body;
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle, CURLOPT_USERAGENT, "MANX");
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT, 30L);
        // Not the main thread, so libcurl must not reach for alarm()/SIGALRM.
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
        // Drop a pending transfer as soon as shutdown is asked for, rather
        // than holding exit back for the timeout above.
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, abort_when_stopping);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &stop);
        const CURLcode result = curl_easy_perform(handle);
        long status = 0;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(handle);
        if (result != CURLE_OK || status != 200) return {};
        return body;
    }
#else
    std::string fetch(const std::string&) { return {}; }
#endif
};

library::library() : m_worker(std::make_unique<worker>()) {}
library::~library() = default;

bool library::configured() {
#if defined(MANX_HAVE_CURL)
    return true;
#else
    return false;
#endif
}

void library::request(const std::string& short_name) {
    if (!configured() || short_name.empty()) return;
    std::lock_guard<std::mutex> lock(m_worker->mutex);
    // seen covers queued, in-flight and finished names alike, so re-requesting
    // on every board change is free.
    if (!m_worker->seen.insert(short_name).second) return;
    m_worker->queue.push_back(short_name);
    if (!m_worker->started) {
        m_worker->started = true;
        m_worker->thread = std::thread([this] { m_worker->run(); });
    }
    m_worker->wake.notify_one();
}

std::vector<ready_bezel> library::take_ready() {
    std::lock_guard<std::mutex> lock(m_worker->mutex);
    return std::exchange(m_worker->ready, {});
}

} // namespace bezel
