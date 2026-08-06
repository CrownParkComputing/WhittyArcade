// manx_http.h - one HTTP client for the whole tree.
//
// There were two hand-rolled copies of this before it existed, in
// bezel_library.cpp and banner_library.cpp, and they had already drifted:
// one of them was missing CURLOPT_NOSIGNAL on a worker thread, which is a
// latent crash rather than a style difference. A third copy was needed for
// the online link, which wants verbs, headers and the bodies of failures.
// Three copies is where duplication stops being cheaper than a header.
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace manx_http {

enum class method { get, post, patch, put, del };

struct request {
    method verb{method::get};
    std::string url;
    std::string body;                          // empty for GET and DELETE
    std::string content_type{"application/json"};
    std::string bearer;                        // an ID token, or empty
    // Anything else the caller needs sent, one per entry, already in
    // "Name: value" form. The lobby service identifies a session this way.
    std::vector<std::string> headers;
    int connect_timeout_seconds{5};
    int timeout_seconds{15};
    // Polled while the transfer runs, so shutting down never has to wait out
    // a timeout on a request nobody is going to read.
    const std::atomic_bool* cancel{nullptr};
};

struct response {
    long status{0};      // 0 means the request never completed at all
    std::string body;    // kept on failure too: Firebase puts the reason in it
    bool ok() const { return status >= 200 && status < 300; }
};

// False in a build without libcurl. Every call then returns status 0, which
// is the same shape as "no network" - so callers have one degraded path to
// handle rather than two.
bool available();

// Blocking. Never call it on the render thread.
response perform(const request& call);

} // namespace manx_http
