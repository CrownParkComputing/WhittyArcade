#include "game_plugin_host.h"

// Loading a shared library, on both systems that do it differently.
//
// dlfcn.h is POSIX and does not exist on Windows, which broke the Windows
// build the day the plugin host arrived: MANX.exe stopped being produced at
// all because one translation unit could not find a header. The three calls
// this file makes have exact Windows equivalents, so they are spelled once
// here rather than scattered through the file behind conditionals.
#if defined(_WIN32)
#include <windows.h>

namespace {
void* plugin_open(const char* path) {
    return reinterpret_cast<void*>(LoadLibraryA(path));
}
void* plugin_symbol(void* handle, const char* name) {
    return reinterpret_cast<void*>(
        GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
}
void plugin_close(void* handle) {
    FreeLibrary(reinterpret_cast<HMODULE>(handle));
}
// Windows reports the reason as a number rather than a string, and a bare
// number in an error message is a number somebody has to go and look up.
std::string plugin_error() {
    const DWORD code = GetLastError();
    if (code == 0) return {};
    char* text = nullptr;
    const DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string message = length && text ? std::string(text, length)
                                         : "error " + std::to_string(code);
    if (text) LocalFree(text);
    while (!message.empty() &&
           (message.back() == '\n' || message.back() == '\r'))
        message.pop_back();
    return message;
}
} // namespace
#else
#include <dlfcn.h>

namespace {
void* plugin_open(const char* path) {
    // RTLD_LOCAL matters: two games may each carry their own copy of a
    // symbol, and a global load would let one win for both.
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
void* plugin_symbol(void* handle, const char* name) {
    return dlsym(handle, name);
}
void plugin_close(void* handle) { dlclose(handle); }
std::string plugin_error() {
    const char* message = dlerror();
    return message != nullptr ? message : std::string();
}
} // namespace
#endif

#include <string>

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

loaded_plugin::~loaded_plugin() {
    // The handle is closed last and only here. Every instance a plugin handed
    // out points at code inside this library, so closing it while a session is
    // alive would leave the host calling into unmapped memory - a crash whose
    // stack blames the game rather than the unload that caused it.
    if (m_handle != nullptr) plugin_close(m_handle);
}

bool loaded_plugin::open(const std::string& library_path, std::string& error) {
    m_handle = plugin_open(library_path.c_str());
    if (m_handle == nullptr) {
        const std::string message = plugin_error();
        error = message.empty() ? "could not load the plugin" : message;
        return false;
    }
    // RTLD_LOCAL above matters: two games may each carry their own copy of a
    // helper with the same name, and a global load would let the first one
    // loaded answer for both.
    auto entry = reinterpret_cast<manx_game_entry_fn>(
        plugin_symbol(m_handle, MANX_GAME_ENTRY_SYMBOL));
    if (entry == nullptr) {
        error = "not a game plugin: no " MANX_GAME_ENTRY_SYMBOL " symbol";
        plugin_close(m_handle);
        m_handle = nullptr;
        return false;
    }
    const manx_game_api* api = entry();
    if (api == nullptr) {
        error = "plugin entry point returned nothing";
        plugin_close(m_handle);
        m_handle = nullptr;
        return false;
    }
    if (api->abi_version != MANX_GAME_ABI_VERSION) {
        error = "built for plugin ABI " + std::to_string(api->abi_version) +
                ", this arcade speaks " +
                std::to_string(MANX_GAME_ABI_VERSION);
        plugin_close(m_handle);
        m_handle = nullptr;
        return false;
    }
    // A missing function pointer would fault on the frame that first used it,
    // which could be minutes into a game. Checked once, here, so a malformed
    // plugin is refused at discovery instead.
    if (api->describe == nullptr || api->create == nullptr ||
        api->destroy == nullptr || api->run_frame == nullptr ||
        api->reset == nullptr || api->set_paused == nullptr ||
        api->score == nullptr || api->state_checksum == nullptr ||
        api->take_audio_cues == nullptr ||
        api->describe_audio_cues == nullptr) {
        error = "plugin table has a null entry";
        plugin_close(m_handle);
        m_handle = nullptr;
        return false;
    }
    // Optional extension: its own symbol and version preserve binary
    // compatibility with every ABI-2 plugin already installed.  If a plugin
    // advertises the extension it must be complete; silently ignoring a bad
    // table would lose scores while claiming online support.
    auto stats_entry = reinterpret_cast<manx_game_stats_entry_fn>(
        plugin_symbol(m_handle, MANX_GAME_STATS_ENTRY_SYMBOL));
    if (stats_entry != nullptr) {
        const manx_game_stats_api* stats = stats_entry();
        if (stats == nullptr ||
            stats->abi_version != MANX_GAME_STATS_ABI_VERSION ||
            stats->take_events == nullptr) {
            error = "plugin has an invalid stats extension";
            plugin_close(m_handle);
            m_handle = nullptr;
            return false;
        }
        m_stats_api = stats;
    }
    // Persistent achievements are host-owned and optional. The extension is a
    // separate symbol so installed ABI-2 plugins remain binary-compatible.
    auto achievements_entry = reinterpret_cast<manx_game_achievements_entry_fn>(
        plugin_symbol(m_handle, MANX_GAME_ACHIEVEMENTS_ENTRY_SYMBOL));
    if (achievements_entry != nullptr) {
        const manx_game_achievements_api* achievements = achievements_entry();
        if (achievements == nullptr ||
            achievements->abi_version != MANX_GAME_ACHIEVEMENTS_ABI_VERSION ||
            achievements->describe == nullptr ||
            achievements->restore == nullptr ||
            achievements->take_events == nullptr) {
            error = "plugin has an invalid achievements extension";
            plugin_close(m_handle);
            m_handle = nullptr;
            return false;
        }
        m_achievements_api = achievements;
    }
    // Continuous mixed audio is a separate optional extension. Existing
    // cue-only plugins remain ABI-2 compatible, while recomp plugins can pass
    // through the console's real music, speech and effects mix.
    auto pcm_entry = reinterpret_cast<manx_game_pcm_entry_fn>(
        plugin_symbol(m_handle, MANX_GAME_PCM_ENTRY_SYMBOL));
    if (pcm_entry != nullptr) {
        const manx_game_pcm_api* pcm = pcm_entry();
        if (pcm == nullptr ||
            pcm->abi_version != MANX_GAME_PCM_ABI_VERSION ||
            pcm->take_blocks == nullptr) {
            error = "plugin has an invalid PCM extension";
            plugin_close(m_handle);
            m_handle = nullptr;
            return false;
        }
        m_pcm_api = pcm;
    }
    m_api = api;
    return true;
}

void game_plugin_library::consider(const std::string& library_path,
                                   const std::string& bundle_path) {
    loaded_plugin plugin;
    std::string error;
    if (!plugin.open(library_path, error)) {
        m_rejected.push_back(rejected_plugin{library_path, error});
        return;
    }
    manx_game_info info{};
    plugin.api()->describe(&info);
    if (info.short_name == nullptr || *info.short_name == '\0') {
        m_rejected.push_back(
            rejected_plugin{library_path, "plugin describes no short name"});
        return;
    }
    discovered_game game;
    game.short_name = info.short_name;
    game.display_name =
        info.display_name != nullptr ? info.display_name : info.short_name;
    game.publisher = info.publisher != nullptr ? info.publisher : "";
    game.library_path = library_path;
    game.bundle_path = bundle_path;
    game.max_players = info.max_players != 0 ? info.max_players : 1;
    game.supports_network = info.supports_network != 0;
    game.refresh_hz = info.refresh_hz > 0.0 ? info.refresh_hz : 60.0;

    // Two plugins claiming one short name would fight over the same scores,
    // artwork and bundle. First found wins and the second is reported, because
    // silently preferring one is how a stale copy shadows an updated game.
    const auto clash = std::find_if(
        m_games.begin(), m_games.end(), [&](const discovered_game& existing) {
            return existing.short_name == game.short_name;
        });
    if (clash != m_games.end()) {
        m_rejected.push_back(rejected_plugin{
            library_path,
            "short name '" + game.short_name + "' already provided by " +
                clash->library_path});
        return;
    }
    m_games.push_back(std::move(game));
    // plugin closes here: discovery must not hold every game's library open.
}

void game_plugin_library::scan(const std::string& root) {
    m_games.clear();
    m_rejected.clear();
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;

    // Candidates are gathered and SORTED before any is loaded, because the
    // clash rule below is "first one found wins" and the file system hands
    // entries back in whatever order it likes. Considering them in directory
    // order would let two installs of the same game resolve differently on two
    // machines - and the loser is reported, so the difference would show up as
    // one machine mysteriously running an older copy.
    std::vector<std::pair<std::string, std::string>> candidates;
    for (const fs::directory_entry& entry :
         fs::directory_iterator(root, ec)) {
        // Each query gets its OWN error_code, and the loop never breaks on one.
        //
        // Sharing `ec` with the queries below cost a whole afternoon. A bundle
        // directory with no library in it - which every recomp-imported title
        // is, since those ship sfx/ and art/ and no .so - makes
        // is_regular_file set "No such file or directory", and the next
        // iteration then hit `if (ec) break;` and abandoned the scan. Every
        // game the filesystem happened to return after that one silently did
        // not exist: no rejection, no message, just a launcher missing games.
        // Here it was `spacegiraffe/` doing it, and the two Geometry Wars
        // plugins were only found because they came back first.
        std::error_code query;
        if (entry.is_directory(query)) {
            // <root>/<short name>/<short name>.so
            const fs::path expected =
                entry.path() / (entry.path().filename().string() + ".so");
            if (fs::is_regular_file(expected, query))
                candidates.emplace_back(expected.string(),
                                        entry.path().string());
            continue;
        }
        // A bare .so in the root: the game's data, if it has any, is the root.
        if (entry.is_regular_file(query) && entry.path().extension() == ".so")
            candidates.emplace_back(entry.path().string(), root);
    }
    std::sort(candidates.begin(), candidates.end());
    for (const auto& [library_path, bundle_path] : candidates)
        consider(library_path, bundle_path);

    // Stable order for the launcher, independent of how they were found.
    std::sort(m_games.begin(), m_games.end(),
              [](const discovered_game& a, const discovered_game& b) {
                  return a.short_name < b.short_name;
              });
}

const discovered_game* game_plugin_library::find(
    std::string_view short_name) const noexcept {
    for (const discovered_game& game : m_games)
        if (game.short_name == short_name) return &game;
    return nullptr;
}

std::unique_ptr<loaded_plugin> game_plugin_library::load(
    const discovered_game& game, std::string& error) const {
    auto plugin = std::make_unique<loaded_plugin>();
    if (!plugin->open(game.library_path, error)) return nullptr;
    return plugin;
}
