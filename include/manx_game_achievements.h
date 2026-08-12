// manx_game_achievements.h - optional persistent-achievement extension.
//
// MANX owns persistence. A plugin declares achievements, receives the saved
// state for the current OS user at creation, and emits absolute progress. This
// keeps plugins away from arbitrary host paths and keeps ABI-2 games valid.
#ifndef MANX_GAME_ACHIEVEMENTS_H
#define MANX_GAME_ACHIEVEMENTS_H

#include "manx_game_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MANX_GAME_ACHIEVEMENTS_ABI_VERSION 1u
#define MANX_GAME_ACHIEVEMENTS_ENTRY_SYMBOL "manx_game_achievements_entry"
#define MANX_GAME_ACHIEVEMENT_MAX_COUNT 256u

enum manx_game_achievement_flags {
    manx_game_achievement_hidden = 1u << 0,
};

typedef struct manx_game_achievement_definition {
    uint32_t id;
    const char* name;
    const char* description;
    uint32_t points;
    uint32_t flags;
    uint64_t target_progress;
} manx_game_achievement_definition;

typedef struct manx_game_achievement_state {
    uint32_t id;
    uint32_t unlocked;
    uint64_t progress;
} manx_game_achievement_state;

typedef struct manx_game_achievement_event {
    uint32_t id;
    uint32_t unlocked;
    uint64_t progress;
} manx_game_achievement_event;

typedef struct manx_game_achievements_api {
    uint32_t abi_version;

    // Returns the definition count. `out` may be null to query it first.
    uint32_t (*describe)(manx_game_achievement_definition* out,
                         uint32_t max_count);

    // Restores host-owned state immediately after create(). Unknown IDs are
    // never supplied. Plugins must not re-emit restored progress as new work.
    void (*restore)(manx_game_instance* instance,
                    const manx_game_achievement_state* states,
                    uint32_t count);

    // Drains absolute progress events. Progress must never decrease; setting
    // unlocked or reaching target_progress makes the unlock permanent.
    uint32_t (*take_events)(manx_game_instance* instance,
                            manx_game_achievement_event* out_events,
                            uint32_t max_events);
} manx_game_achievements_api;

typedef const manx_game_achievements_api*
    (*manx_game_achievements_entry_fn)(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // MANX_GAME_ACHIEVEMENTS_H
