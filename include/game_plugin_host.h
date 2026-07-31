// game_plugin_host.h - finding and loading game plugins.
//
// A plugin lives in its own folder beside its data, the way a ROM set does:
//
//     <plugins root>/geometrywars/geometrywars.so
//     <plugins root>/geometrywars/...its data...
//
// The folder name is the game's short name, which is the key everything else in
// the arcade already uses - high scores, bezels, banners, cover art and
// persistent data are all looked up by it. Naming the folder is therefore the
// whole of "installing" a game.
//
// Discovery never runs a plugin. It opens each library, checks the ABI version
// and asks it to describe itself, so the launcher can list a game without
// starting it - and so a plugin built against an older contract is reported
// rather than loaded.
#pragma once

#include "whitty_game_plugin.h"

#include <memory>
#include <string>
#include <vector>

// One game found on disk. `library_path` is the .so; `bundle_path` is the
// folder holding it, which is what gets handed to the plugin as its data root.
struct discovered_game {
    std::string short_name;
    std::string display_name;
    std::string publisher;
    std::string library_path;
    std::string bundle_path;
    uint32_t max_players{1};
    bool supports_network{false};
    double refresh_hz{60.0};
};

// Why a plugin on disk was not offered. Kept rather than discarded because a
// game that silently fails to appear is indistinguishable from one that was
// never installed, and that is a support question nobody can answer.
struct rejected_plugin {
    std::string library_path;
    std::string reason;
};

// A loaded plugin's library handle plus its function table. Closing the handle
// invalidates every instance created from it, so the loader owns both together
// and outlives any session built on it.
class loaded_plugin {
public:
    loaded_plugin() = default;
    ~loaded_plugin();
    loaded_plugin(const loaded_plugin&) = delete;
    loaded_plugin& operator=(const loaded_plugin&) = delete;

    bool open(const std::string& library_path, std::string& error);
    const whitty_game_api* api() const noexcept { return m_api; }
    bool valid() const noexcept { return m_api != nullptr; }

private:
    void* m_handle{nullptr};
    const whitty_game_api* m_api{nullptr};
};

class game_plugin_library {
public:
    // Scans `root` one level deep for <folder>/<folder>.so, and also accepts a
    // bare .so sitting directly in the root so a single-file game does not need
    // a folder of its own.
    void scan(const std::string& root);

    const std::vector<discovered_game>& games() const noexcept {
        return m_games;
    }
    const std::vector<rejected_plugin>& rejected() const noexcept {
        return m_rejected;
    }
    const discovered_game* find(std::string_view short_name) const noexcept;

    // Opens a discovered game's library. Returns null if it will not load.
    std::unique_ptr<loaded_plugin> load(const discovered_game& game,
                                        std::string& error) const;

private:
    void consider(const std::string& library_path,
                  const std::string& bundle_path);

    std::vector<discovered_game> m_games;
    std::vector<rejected_plugin> m_rejected;
};
