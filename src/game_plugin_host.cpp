#include "game_plugin_host.h"

#include <dlfcn.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

loaded_plugin::~loaded_plugin() {
    // The handle is closed last and only here. Every instance a plugin handed
    // out points at code inside this library, so closing it while a session is
    // alive would leave the host calling into unmapped memory - a crash whose
    // stack blames the game rather than the unload that caused it.
    if (m_handle != nullptr) dlclose(m_handle);
}

bool loaded_plugin::open(const std::string& library_path, std::string& error) {
    m_handle = dlopen(library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (m_handle == nullptr) {
        const char* message = dlerror();
        error = message != nullptr ? message : "dlopen failed";
        return false;
    }
    // RTLD_LOCAL above matters: two games may each carry their own copy of a
    // helper with the same name, and a global load would let the first one
    // loaded answer for both.
    auto entry = reinterpret_cast<whitty_game_entry_fn>(
        dlsym(m_handle, WHITTY_GAME_ENTRY_SYMBOL));
    if (entry == nullptr) {
        error = "not a game plugin: no " WHITTY_GAME_ENTRY_SYMBOL " symbol";
        dlclose(m_handle);
        m_handle = nullptr;
        return false;
    }
    const whitty_game_api* api = entry();
    if (api == nullptr) {
        error = "plugin entry point returned nothing";
        dlclose(m_handle);
        m_handle = nullptr;
        return false;
    }
    if (api->abi_version != WHITTY_GAME_ABI_VERSION) {
        error = "built for plugin ABI " + std::to_string(api->abi_version) +
                ", this arcade speaks " +
                std::to_string(WHITTY_GAME_ABI_VERSION);
        dlclose(m_handle);
        m_handle = nullptr;
        return false;
    }
    // A missing function pointer would fault on the frame that first used it,
    // which could be minutes into a game. Checked once, here, so a malformed
    // plugin is refused at discovery instead.
    if (api->describe == nullptr || api->create == nullptr ||
        api->destroy == nullptr || api->run_frame == nullptr ||
        api->reset == nullptr || api->set_paused == nullptr ||
        api->score == nullptr || api->state_checksum == nullptr) {
        error = "plugin table has a null entry";
        dlclose(m_handle);
        m_handle = nullptr;
        return false;
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
    whitty_game_info info{};
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
        if (ec) break;
        if (entry.is_directory(ec)) {
            // <root>/<short name>/<short name>.so
            const fs::path expected =
                entry.path() / (entry.path().filename().string() + ".so");
            if (fs::is_regular_file(expected, ec))
                candidates.emplace_back(expected.string(),
                                        entry.path().string());
            continue;
        }
        // A bare .so in the root: the game's data, if it has any, is the root.
        if (entry.is_regular_file(ec) && entry.path().extension() == ".so")
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
