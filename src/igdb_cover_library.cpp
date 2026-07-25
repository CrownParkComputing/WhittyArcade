// igdb_cover_library.cpp - the background half of the IGDB artwork path.
//
// One worker thread owns every network request, every cache file and the OAuth
// token; the launcher only ever hands it menu labels and collects finished
// covers. That is what keeps artwork off the critical path in both directions:
// no menu can stall waiting for a lookup, and no game launch waits for one
// either, because the destructor aborts an in-flight transfer rather than
// letting it run out its timeout.
#include "igdb_artwork.h"

#include "platform_paths.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>

#if defined(WHITTY_HAVE_CURL)
#include <curl/curl.h>

// The decoder is instantiated once, in src/stb_image_impl.cpp, which also
// carries the limits this path wants.
#include "stb_image.h"
#endif

namespace igdb {

#if defined(WHITTY_HAVE_CURL)

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// IGDB application credentials.
//
// These are configured in ONE place: the WHITTY_IGDB_CLIENT_ID and
// WHITTY_IGDB_CLIENT_SECRET cache variables at the top of CMakeLists.txt, which
// arrive here as compile definitions. WhittyArcade has no secrets store, and
// settings.ini is a plain user-editable preferences file that the first-run
// wizard rewrites, so a client secret does not belong in it. Injecting at
// configure time matches the Android reference's BuildConfig approach and lets a
// build ship with no keys at all (configure them empty). The matching
// environment variables override both for a single run.
// ---------------------------------------------------------------------------
#ifndef WHITTY_IGDB_CLIENT_ID
#define WHITTY_IGDB_CLIENT_ID ""
#endif
#ifndef WHITTY_IGDB_CLIENT_SECRET
#define WHITTY_IGDB_CLIENT_SECRET ""
#endif

constexpr std::size_t max_transfer_bytes = 8u << 20;
constexpr int transport_failure_limit = 3;

std::once_flag curl_ready;

const char* environment_value(const char* name) {
    const char* value = std::getenv(name);
    return value && *value ? value : nullptr;
}

std::string client_id() {
    if (const char* value = environment_value("WHITTY_IGDB_CLIENT_ID"))
        return value;
    return WHITTY_IGDB_CLIENT_ID;
}

std::string client_secret() {
    if (const char* value = environment_value("WHITTY_IGDB_CLIENT_SECRET"))
        return value;
    return WHITTY_IGDB_CLIENT_SECRET;
}

// Silent by default. A cabinet with no network must not print a line per game,
// and a missing cover is not something the user can act on; WHITTY_IGDB_DEBUG=1
// turns the path chatty for the rare occasion a lookup needs looking at.
bool diagnostics_enabled() {
    static const bool enabled = environment_value("WHITTY_IGDB_DEBUG") != nullptr;
    return enabled;
}

template <typename... arguments>
void report(const char* format, arguments... values) {
    if (!diagnostics_enabled()) return;
    std::fprintf(stderr, format, values...);
    std::fputc('\n', stderr);
}

fs::path artwork_root() {
    const fs::path root = whitty_platform::data_root();
    return (root.empty() ? fs::current_path() : root) / "WhittyArcade" /
           "artwork";
}

fs::path token_path() {
    const fs::path root = whitty_platform::config_root();
    return (root.empty() ? fs::current_path() : root) / "WhittyArcade" /
           "igdb-token.ini";
}

fs::path miss_index_path() { return artwork_root() / "misses.ini"; }

std::string read_file(const fs::path& path) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error || size == 0 || size > max_transfer_bytes) return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::string contents(static_cast<std::size_t>(size), '\0');
    input.read(contents.data(), static_cast<std::streamsize>(size));
    if (!input) return {};
    return contents;
}

// Written through a temporary and renamed into place: a launcher killed
// mid-download would otherwise leave a truncated image that decodes to garbage
// and is then treated as a valid cache hit on every later launch.
void write_file(const fs::path& path, const std::string& contents) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    const fs::path staging = fs::path(path).concat(".part");
    {
        std::ofstream output(staging, std::ios::binary | std::ios::trunc);
        if (!output) return;
        output.write(contents.data(),
                     static_cast<std::streamsize>(contents.size()));
        if (!output) return;
    }
    fs::rename(staging, path, error);
    if (error) fs::remove(staging, error);
}

std::size_t append_body(char* data, std::size_t size, std::size_t count,
                        void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    const std::size_t bytes = size * count;
    // Returning short tells libcurl to abort, which is the right answer for a
    // response far larger than any cover: something other than IGDB replied.
    if (body->size() + bytes > max_transfer_bytes) return 0;
    body->append(data, bytes);
    return bytes;
}

int abort_when_stopping(void* userdata, curl_off_t, curl_off_t, curl_off_t,
                        curl_off_t) {
    const auto* stopping = static_cast<const std::atomic_bool*>(userdata);
    return stopping->load(std::memory_order_acquire) ? 1 : 0;
}

cover_image decode_cover(const std::string& bytes) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.data()),
        static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (!pixels) return {};
    cover_image image;
    image.width = width;
    image.height = height;
    image.pixels.assign(pixels, pixels + static_cast<std::size_t>(width) *
                                             static_cast<std::size_t>(height) *
                                             4);
    stbi_image_free(pixels);
    return image;
}

// Separated on purpose: "the request never reached IGDB" must never be recorded
// as "IGDB has no cover for this game", or one offline launch would poison the
// negative cache for a fortnight.
struct http_response {
    bool reached_server{};
    long status{};
    std::string body;
};

} // namespace

struct cover_library::worker {
    explicit worker(std::function<void()> ready) : notify(std::move(ready)) {}

    ~worker() {
        stop.store(true, std::memory_order_release);
        wake.notify_all();
        if (thread.joinable()) thread.join();
    }

    // Called from the menu thread only, so the thread object needs no lock of
    // its own; the worker never touches it.
    void ensure_thread() {
        if (thread.joinable() || stop.load(std::memory_order_acquire)) return;
        thread = std::thread(&worker::run, this);
    }

    void run() {
        std::call_once(curl_ready, [] {
            // Deliberately never paired with curl_global_cleanup. The System
            // 246 board dlopen()s a PCSX2 module linked against the same
            // libcurl, and tearing global state out from under it on the way to
            // a game would be far worse than leaving it up until the process
            // exits.
            curl_global_init(CURL_GLOBAL_DEFAULT);
        });
        handle = curl_easy_init();
        for (;;) {
            std::string label;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] {
                    return stop.load(std::memory_order_acquire) ||
                           !pending.empty();
                });
                if (stop.load(std::memory_order_acquire)) break;
                label = std::move(pending.front());
                pending.pop_front();
            }
            process(label);
        }
        if (handle) curl_easy_cleanup(handle);
        handle = nullptr;
    }

    void process(const std::string& menu_label) {
        const std::string title = search_title(menu_label);
        const std::string stem = cache_stem(title);
        if (title.empty() || stem.empty()) return;

        const fs::path cached = artwork_root() / (stem + ".jpg");
        if (const std::string bytes = read_file(cached); !bytes.empty()) {
            cover_image image = decode_cover(bytes);
            if (image.valid()) {
                publish(menu_label, std::move(image));
                return;
            }
            // A cached file that no longer decodes is worse than no cache at
            // all, because it would suppress the fetch that could replace it.
            std::error_code error;
            fs::remove(cached, error);
        }

        // Everything below here needs the network.
        if (network_unreachable) return;
        load_misses();
        if (miss_recorded(misses, stem, epoch_seconds())) return;
        if (!ensure_token()) return;

        http_response found = search(title);
        if (found.reached_server && found.status == 401) {
            // The stored token was rejected: revoked, or the credentials
            // changed since it was issued. Drop it and try once more.
            token = {};
            if (ensure_token()) found = search(title);
        }
        if (!found.reached_server) {
            note_transport_failure();
            return;
        }
        transport_failures = 0;
        if (found.status != 200) {
            // A rate limit or an outage says nothing about this game, so leave
            // the negative cache alone and let a later launch ask again.
            report("IGDB search for \"%s\" returned HTTP %ld", title.c_str(),
                   found.status);
            return;
        }

        std::string url = cover_url(choose_cover_image_id(found.body, title));
        if (url.empty()) {
            // IGDB often lists the arcade original without its subtitle, so
            // one shorter lookup before giving up on the game entirely.
            const std::string shorter = fallback_title(title);
            if (!shorter.empty()) {
                const http_response retry = search(shorter);
                if (retry.reached_server && retry.status == 200)
                    url = cover_url(choose_cover_image_id(retry.body, shorter));
            }
        }
        if (url.empty()) {
            record_miss(stem);
            return;
        }
        const http_response art = perform(url, {}, nullptr);
        if (!art.reached_server) {
            note_transport_failure();
            return;
        }
        transport_failures = 0;
        cover_image image = art.status == 200 ? decode_cover(art.body)
                                             : cover_image{};
        if (!image.valid()) {
            record_miss(stem);
            return;
        }
        write_file(cached, art.body);
        publish(menu_label, std::move(image));
    }

    http_response search(const std::string& title) {
        const std::string body = search_body(title);
        return perform("https://api.igdb.com/v4/games",
                       {"Client-ID: " + client_id(),
                        "Authorization: Bearer " + token.value,
                        "Content-Type: text/plain"},
                       &body);
    }

    bool ensure_token() {
        if (!token_loaded) {
            token_loaded = true;
            if (const std::string stored = read_file(token_path());
                !stored.empty())
                parse_stored_token(stored, token);
        }
        if (token_usable(token, epoch_seconds())) return true;

        static const std::string no_body;
        const http_response response =
            perform(oauth_url(client_id(), client_secret()), {}, &no_body);
        if (!response.reached_server) {
            note_transport_failure();
            return false;
        }
        transport_failures = 0;
        if (response.status != 200 ||
            !parse_token_response(response.body, epoch_seconds(), token)) {
            report("IGDB authentication failed (HTTP %ld)", response.status);
            return false;
        }
        store_token();
        return true;
    }

    void store_token() {
        const fs::path path = token_path();
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        {
            std::ofstream output(path, std::ios::trunc);
            if (!output) return;
            output << format_stored_token(token);
        }
        // A bearer token is a credential rather than cache data, so it does not
        // inherit the umask other WhittyArcade files are happy with.
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, error);
    }

    http_response perform(const std::string& url,
                          const std::vector<std::string>& headers,
                          const std::string* post_body) {
        http_response response;
        if (!handle) return response;
        curl_easy_reset(handle);
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle, CURLOPT_USERAGENT, "WhittyArcade");
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT, 20L);
        // The worker is not the main thread, so libcurl must not reach for
        // alarm()/SIGALRM to bound name resolution.
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append_body);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);
        // Abort as soon as the launcher wants to shut down. Without this a
        // pending connect would hold a game launch back for the timeout above.
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, abort_when_stopping);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &stop);
        if (post_body) {
            curl_easy_setopt(handle, CURLOPT_POST, 1L);
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS, post_body->c_str());
            curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(post_body->size()));
        }
        curl_slist* list = nullptr;
        for (const std::string& header : headers)
            list = curl_slist_append(list, header.c_str());
        if (list) curl_easy_setopt(handle, CURLOPT_HTTPHEADER, list);

        const CURLcode result = curl_easy_perform(handle);
        if (list) curl_slist_free_all(list);
        response.reached_server = result == CURLE_OK;
        if (result == CURLE_OK)
            curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);
        else if (result != CURLE_ABORTED_BY_CALLBACK)
            report("IGDB request failed: %s", curl_easy_strerror(result));
        return response;
    }

    void note_transport_failure() {
        // Three failures in a row means there is no route out. Give up for the
        // rest of the run so an offline cabinet does not spend a connect timeout
        // on every game in the library.
        if (++transport_failures < transport_failure_limit) return;
        network_unreachable = true;
        report("IGDB artwork idle for this run: the API is unreachable");
    }

    void load_misses() {
        if (misses_loaded) return;
        misses_loaded = true;
        misses = parse_miss_index(read_file(miss_index_path()));
    }

    void record_miss(const std::string& stem) {
        const int64_t now = epoch_seconds();
        misses[stem] = now;
        write_file(miss_index_path(), format_miss_index(misses, now));
    }

    void publish(const std::string& menu_label, cover_image image) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            finished.push_back({menu_label, std::move(image)});
        }
        if (notify) notify();
    }

    std::function<void()> notify;
    std::thread thread;
    std::atomic_bool stop{false};

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::string> pending;
    std::vector<ready_cover> finished;
    std::set<std::string> requested;

    // Worker-thread-only state below this line.
    CURL* handle{};
    oauth_token token;
    miss_index misses;
    bool token_loaded{};
    bool misses_loaded{};
    bool network_unreachable{};
    int transport_failures{};
};

cover_library::cover_library(std::function<void()> ready)
    : m_worker(configured() ? std::make_unique<worker>(std::move(ready))
                            : nullptr) {}

cover_library::~cover_library() = default;

bool cover_library::configured() {
    return !client_id().empty() && !client_secret().empty();
}

void cover_library::request(const std::vector<std::string>& menu_labels) {
    if (!m_worker) return;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(m_worker->mutex);
        for (const std::string& label : menu_labels) {
            if (label.empty()) continue;
            if (!m_worker->requested.insert(label).second) continue;
            m_worker->pending.push_back(label);
            queued = true;
        }
    }
    if (!queued) return;
    m_worker->ensure_thread();
    m_worker->wake.notify_all();
}

std::vector<ready_cover> cover_library::take_ready() {
    if (!m_worker) return {};
    std::vector<ready_cover> ready;
    std::lock_guard<std::mutex> lock(m_worker->mutex);
    ready.swap(m_worker->finished);
    return ready;
}

#else // WHITTY_HAVE_CURL

// No HTTP backend was compiled in, so the whole path is inert. Callers still
// link against it unconditionally; configured() reporting false is what stops a
// menu reserving space for artwork that can never arrive.
struct cover_library::worker {};

cover_library::cover_library(std::function<void()>) {}
cover_library::~cover_library() = default;
bool cover_library::configured() { return false; }
void cover_library::request(const std::vector<std::string>&) {}
std::vector<ready_cover> cover_library::take_ready() { return {}; }

#endif // WHITTY_HAVE_CURL

} // namespace igdb
