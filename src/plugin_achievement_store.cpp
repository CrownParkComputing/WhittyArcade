#include "plugin_achievement_store.h"

#include "platform_paths.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {

bool valid_short_name(std::string_view name) {
    if (name.empty() || name.size() > 96) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char value) {
        return std::isalnum(value) || value == '_' || value == '-';
    });
}

} // namespace

fs::path plugin_achievement_path(std::string_view short_name) {
    if (!valid_short_name(short_name)) return {};
    fs::path root = manx_platform::data_root();
    if (root.empty()) root = fs::current_path();
    return root / "MANX" / "achievements" /
           (std::string(short_name) + ".state");
}

bool load_plugin_achievements(
    std::string_view short_name,
    std::vector<manx_game_achievement_state>& states,
    std::string& error) {
    states.clear();
    error.clear();
    const fs::path path = plugin_achievement_path(short_name);
    if (path.empty()) {
        error = "invalid game short name";
        return false;
    }
    std::ifstream input(path);
    if (!input) {
        std::error_code ec;
        if (!fs::exists(path, ec)) return true;
        error = "could not open " + path.string();
        return false;
    }
    std::string magic;
    if (!(input >> magic) || magic != "MANX-ACHIEVEMENTS-1") {
        error = "achievement state has an invalid header";
        return false;
    }
    manx_game_achievement_state state{};
    while (input >> state.id >> state.progress >> state.unlocked) {
        if (state.id == 0) {
            error = "achievement state contains id 0";
            states.clear();
            return false;
        }
        state.unlocked = state.unlocked ? 1u : 0u;
        const auto duplicate = std::find_if(
            states.begin(), states.end(), [&](const auto& existing) {
                return existing.id == state.id;
            });
        if (duplicate != states.end()) {
            error = "achievement state contains a duplicate id";
            states.clear();
            return false;
        }
        states.push_back(state);
        if (states.size() > MANX_GAME_ACHIEVEMENT_MAX_COUNT) {
            error = "achievement state exceeds the supported count";
            states.clear();
            return false;
        }
    }
    if (!input.eof()) {
        error = "achievement state is truncated or malformed";
        states.clear();
        return false;
    }
    return true;
}

bool save_plugin_achievements(
    std::string_view short_name,
    const std::vector<manx_game_achievement_state>& states,
    std::string& error) {
    error.clear();
    const fs::path path = plugin_achievement_path(short_name);
    if (path.empty()) {
        error = "invalid game short name";
        return false;
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "could not create achievement directory: " + ec.message();
        return false;
    }
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            error = "could not create " + temporary.string();
            return false;
        }
        output << "MANX-ACHIEVEMENTS-1\n";
        for (const auto& state : states)
            output << state.id << ' ' << state.progress << ' '
                   << (state.unlocked ? 1u : 0u) << '\n';
        output.flush();
        if (!output.good()) {
            error = "could not finish writing achievement state";
            return false;
        }
    }
    fs::rename(temporary, path, ec);
    if (!ec) return true;
    ec.clear();
    fs::remove(path, ec);
    ec.clear();
    fs::rename(temporary, path, ec);
    if (!ec) return true;
    error = "could not replace achievement state: " + ec.message();
    return false;
}
