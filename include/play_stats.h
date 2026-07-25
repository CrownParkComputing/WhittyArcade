// play_stats.h - how often and how recently each game has been played.
#pragma once

#include <map>
#include <string>

// Recorded once per successful board launch, keyed by MAME short name. The
// launcher's "Most Played" view is the entire reason this exists, so the
// format is the simplest thing that survives: one "name count last-epoch"
// line per game.
struct play_stat {
    int count{};
    long long last_played{};
};

void record_play(const std::string& short_name);
std::map<std::string, play_stat> load_play_stats();
