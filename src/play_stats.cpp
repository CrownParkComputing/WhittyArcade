#include "play_stats.h"

#include "platform_paths.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

fs::path stats_path() {
    const fs::path root = manx_platform::data_root();
    return (root.empty() ? fs::current_path() : root) / "MANX" /
           "play_stats.txt";
}

} // namespace

std::map<std::string, play_stat> load_play_stats() {
    std::map<std::string, play_stat> stats;
    std::ifstream input(stats_path());
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string name;
        play_stat stat;
        if (fields >> name >> stat.count >> stat.last_played && !name.empty())
            stats[name] = stat;
    }
    return stats;
}

void record_play(const std::string& short_name) {
    if (short_name.empty()) return;
    std::map<std::string, play_stat> stats = load_play_stats();
    play_stat& stat = stats[short_name];
    ++stat.count;
    stat.last_played = static_cast<long long>(std::time(nullptr));
    std::error_code error;
    fs::create_directories(stats_path().parent_path(), error);
    std::ofstream output(stats_path(), std::ios::trunc);
    for (const auto& entry : stats)
        output << entry.first << ' ' << entry.second.count << ' '
               << entry.second.last_played << '\n';
}
