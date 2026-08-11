// manx_game_stats.h - optional persistent-stat extension for a MANX plugin.
// Kept as a separate exported table so existing ABI-2 plugins remain valid.
#ifndef MANX_GAME_STATS_H
#define MANX_GAME_STATS_H

#include "manx_game_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MANX_GAME_STATS_ABI_VERSION 2u
#define MANX_GAME_STATS_ENTRY_SYMBOL "manx_game_stats_entry"
#define MANX_GAME_STAT_MAX_PROPERTIES 16u

enum manx_game_stat_flags {
    manx_game_stat_lower_is_better = 1u << 0,
};

typedef struct manx_game_stat_event {
    uint32_t title_id;
    uint32_t leaderboard_id;
    uint32_t property_id;
    uint32_t flags;
    uint64_t value;
    uint64_t guest_timestamp_ms;
    uint32_t metadata_count;
    uint32_t reserved;
    struct {
        uint32_t property_id;
        uint32_t reserved;
        uint64_t value;
    } metadata[MANX_GAME_STAT_MAX_PROPERTIES];
} manx_game_stat_event;

typedef struct manx_game_stats_api {
    uint32_t abi_version;
    uint32_t (*take_events)(manx_game_instance* instance,
                            manx_game_stat_event* out_events,
                            uint32_t max_events);
} manx_game_stats_api;

typedef const manx_game_stats_api* (*manx_game_stats_entry_fn)(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MANX_GAME_STATS_H
