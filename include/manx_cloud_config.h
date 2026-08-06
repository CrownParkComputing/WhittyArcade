// Which Firebase project this build talks to.
//
// The API key is NOT a secret. It names a project and authorises nothing:
// it is visible in the source of every Firebase web page, and it is visible
// in this binary to anybody who runs `strings` on it. All the security lives
// in web/firestore.rules. Do not try to hide it; do try to keep the rules
// honest.
//
// Both can be overridden at build time (-DMANX_FIREBASE_KEY=\"...\") or at
// run time (MANX_FIREBASE_KEY / MANX_FIREBASE_PROJECT), which is how a fork
// points at its own project without editing the tree.
#pragma once

#include <cstdlib>
#include <string>

#if !defined(MANX_FIREBASE_PROJECT_ID)
#define MANX_FIREBASE_PROJECT_ID "manx-online-network"
#endif

// The project's own key, so every build reaches it without being launched
// with an environment variable somebody has to remember. A cabinet that has
// to be started a particular way is a cabinet that reports itself "not set
// up" on the second machine and nowhere says why.
//
// This is not a secret being committed. A Firebase web API key names a
// project and authorises nothing at all: it is served in the page source of
// the website and is readable in this binary with `strings`. What protects
// the data is firestore.rules in the MANXOnline repo.
#if !defined(MANX_FIREBASE_API_KEY)
#define MANX_FIREBASE_API_KEY "AIzaSyDMp5CcExwsw5lM5Oo-a6PMB4ZwlwSkn7k"
#endif

namespace manx_cloud {

inline std::string project_id() {
    if (const char* forced = std::getenv("MANX_FIREBASE_PROJECT"))
        if (*forced) return forced;
    return MANX_FIREBASE_PROJECT_ID;
}

inline std::string api_key() {
    if (const char* forced = std::getenv("MANX_FIREBASE_KEY"))
        if (*forced) return forced;
    return MANX_FIREBASE_API_KEY;
}

// Where the lobby service lives.
//
// Lobbies, addresses and the handshake are no longer Firestore documents
// that two cabinets poll at each other: they are a conversation with an app
// we run. Firebase keeps what it is good at - saying who somebody is, and
// holding what outlives a session.
//
// Overridable so a developer can point a cabinet at a service running on
// their own machine without rebuilding it.
inline std::string lobby_service() {
    if (const char* forced = std::getenv("MANX_LOBBY_URL"))
        if (*forced) return forced;
    return "https://manx.crownparkcomputing.com";
}

inline bool configured() {
    return !project_id().empty() && !api_key().empty();
}

// Where the documents live. Assembled here so a typo is one typo rather than
// one per call site.
inline std::string documents_root() {
    return "https://firestore.googleapis.com/v1/projects/" + project_id() +
           "/databases/(default)/documents";
}

inline std::string identity_url(const std::string& method) {
    return "https://identitytoolkit.googleapis.com/v1/accounts:" + method +
           "?key=" + api_key();
}

inline std::string refresh_url() {
    return "https://securetoken.googleapis.com/v1/token?key=" + api_key();
}

} // namespace manx_cloud
