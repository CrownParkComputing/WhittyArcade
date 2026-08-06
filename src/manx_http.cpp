#include "manx_http.h"

#if defined(MANX_HAVE_CURL)
#include <curl/curl.h>
#endif

namespace manx_http {

#if defined(MANX_HAVE_CURL)
namespace {

std::size_t append(char* data, std::size_t size, std::size_t count,
                   void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(data, size * count);
    return size * count;
}

int abort_when_cancelled(void* userdata, curl_off_t, curl_off_t, curl_off_t,
                         curl_off_t) {
    const auto* cancel = static_cast<const std::atomic_bool*>(userdata);
    return cancel && cancel->load() ? 1 : 0;
}

// Every library in the tree relied on libcurl initialising itself on first
// use, which is only thread safe from libcurl 7.84 onwards and silently
// racy before it. One function-local static settles it for all of them.
void ensure_global_init() {
    static const bool ready = [] {
        return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    }();
    (void)ready;
}

const char* custom_verb(method verb) {
    switch (verb) {
    case method::patch: return "PATCH";
    case method::put:   return "PUT";
    case method::del:   return "DELETE";
    default:            return nullptr;
    }
}

} // namespace

bool available() { return true; }

response perform(const request& call) {
    response result;
    if (call.url.empty()) return result;
    ensure_global_init();

    CURL* handle = curl_easy_init();
    if (!handle) return result;

    curl_easy_setopt(handle, CURLOPT_URL, call.url.c_str());
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "MANX");
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT,
                     static_cast<long>(call.connect_timeout_seconds));
    curl_easy_setopt(handle, CURLOPT_TIMEOUT,
                     static_cast<long>(call.timeout_seconds));
    // Not the main thread, so libcurl must not reach for alarm()/SIGALRM.
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, append);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &result.body);

    if (call.cancel) {
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION,
                         abort_when_cancelled);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA,
                         const_cast<std::atomic_bool*>(call.cancel));
    }

    if (call.verb == method::post) curl_easy_setopt(handle, CURLOPT_POST, 1L);
    if (const char* verb = custom_verb(call.verb))
        curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, verb);

    if (call.verb != method::get && call.verb != method::del) {
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, call.body.c_str());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(call.body.size()));
    }

    curl_slist* headers = nullptr;
    if (call.verb != method::get && call.verb != method::del &&
        !call.content_type.empty())
        headers = curl_slist_append(
            headers, ("Content-Type: " + call.content_type).c_str());
    if (!call.bearer.empty())
        headers = curl_slist_append(
            headers, ("Authorization: Bearer " + call.bearer).c_str());
    // An unrequested 100-continue on every POST costs a round trip and some
    // proxies never answer it at all.
    for (const std::string& extra : call.headers)
        if (!extra.empty())
            headers = curl_slist_append(headers, extra.c_str());
    headers = curl_slist_append(headers, "Expect:");
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

    const CURLcode outcome = curl_easy_perform(handle);
    if (outcome == CURLE_OK)
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &result.status);
    else
        // Leave the status at 0 and keep whatever body arrived. A caller
        // cannot act on a CURLcode, but it can tell "never happened" from
        // "happened and was refused", which is the distinction that matters.
        result.status = 0;

    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);
    return result;
}

#else

bool available() { return false; }

response perform(const request&) { return {}; }

#endif

} // namespace manx_http
