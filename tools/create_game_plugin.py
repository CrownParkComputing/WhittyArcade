#!/usr/bin/env python3
"""Create a buildable MANX native game plugin with saved achievements."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


def cpp_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Scaffold a MANX ABI-2 game plugin with persistent achievements."
    )
    parser.add_argument("short_name", help="lower-case launcher key, e.g. my_game")
    parser.add_argument("display_name", help="human-readable game title")
    parser.add_argument(
        "--output", type=Path, help="destination (default: ./<short_name>)"
    )
    args = parser.parse_args()

    if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,63}", args.short_name):
        parser.error("short_name must match [a-z0-9][a-z0-9_-]{0,63}")
    destination = (args.output or Path.cwd() / args.short_name).resolve()
    if destination.exists() and any(destination.iterdir()):
        parser.error(f"destination is not empty: {destination}")
    destination.mkdir(parents=True, exist_ok=True)

    source = f'''#include "manx_game_achievements.h"
#include "manx_game_plugin.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

struct manx_game_instance {{
    uint64_t frames{{}};
    uint64_t first_run_progress{{}};
    bool first_run_unlocked{{}};
    std::vector<uint8_t> pixels = std::vector<uint8_t>(320u * 180u * 4u, 0u);
    std::deque<manx_game_achievement_event> achievement_events;
}};

namespace {{

constexpr uint32_t kFirstRunAchievement = 1u;

void describe(manx_game_info* info) {{
    *info = {{"{cpp_string(args.short_name)}", "{cpp_string(args.display_name)}",
             "Your Studio", 1u, 0u, 60.0}};
}}

manx_game_instance* create(const char*) {{ return new manx_game_instance; }}
void destroy(manx_game_instance* game) {{ delete game; }}

void run_frame(manx_game_instance* game, const manx_game_input*, uint32_t,
               manx_game_frame* out) {{
    ++game->frames;
    const uint8_t shade = static_cast<uint8_t>((game->frames / 2u) & 0xFFu);
    for (size_t at = 0; at < game->pixels.size(); at += 4) {{
        game->pixels[at + 0] = shade;
        game->pixels[at + 1] = 64u;
        game->pixels[at + 2] = 128u;
        game->pixels[at + 3] = 255u;
    }}

    if (!game->first_run_unlocked) {{
        game->first_run_progress = std::min<uint64_t>(game->frames, 60u);
        if (game->first_run_progress == 60u) game->first_run_unlocked = true;
        game->achievement_events.push_back({{
            kFirstRunAchievement,
            game->first_run_unlocked ? 1u : 0u,
            game->first_run_progress}});
    }}

    *out = {{game->pixels.data(), 320u, 180u, 16u, 9u}};
}}

void reset(manx_game_instance* game) {{ game->frames = 0; }}
void set_paused(manx_game_instance*, uint32_t) {{}}
uint64_t score(manx_game_instance* game) {{ return game->frames; }}
uint64_t checksum(manx_game_instance* game) {{ return game->frames * 1099511628211ull; }}
uint32_t take_audio(manx_game_instance*, manx_game_audio_cue*, uint32_t) {{ return 0; }}
uint32_t describe_audio(const char**, uint32_t) {{ return 0; }}

uint32_t describe_achievements(manx_game_achievement_definition* out,
                               uint32_t max_count) {{
    if (!out) return 1;
    if (max_count == 0) return 0;
    out[0] = {{kFirstRunAchievement, "First Run",
              "Play for one second.", 10u, 0u, 60u}};
    return 1;
}}

void restore_achievements(manx_game_instance* game,
                          const manx_game_achievement_state* states,
                          uint32_t count) {{
    for (uint32_t at = 0; at < count; ++at) {{
        if (states[at].id != kFirstRunAchievement) continue;
        game->first_run_progress = std::min<uint64_t>(states[at].progress, 60u);
        game->first_run_unlocked = states[at].unlocked != 0 ||
                                   game->first_run_progress == 60u;
    }}
}}

uint32_t take_achievement_events(manx_game_instance* game,
                                 manx_game_achievement_event* out,
                                 uint32_t max_events) {{
    uint32_t written = 0;
    while (written < max_events && !game->achievement_events.empty()) {{
        out[written++] = game->achievement_events.front();
        game->achievement_events.pop_front();
    }}
    return written;
}}

const manx_game_api game_api = {{
    MANX_GAME_ABI_VERSION, describe, create, destroy, run_frame, reset,
    set_paused, score, checksum, take_audio, describe_audio}};
const manx_game_achievements_api achievements_api = {{
    MANX_GAME_ACHIEVEMENTS_ABI_VERSION, describe_achievements,
    restore_achievements, take_achievement_events}};

}} // namespace

extern "C" MANX_GAME_EXPORT const manx_game_api* manx_game_entry() {{
    return &game_api;
}}
extern "C" MANX_GAME_EXPORT const manx_game_achievements_api*
manx_game_achievements_entry() {{
    return &achievements_api;
}}
'''

    cmake = f'''cmake_minimum_required(VERSION 3.18)
project({args.short_name}_manx_plugin LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(MANX_SDK_INCLUDE "$ENV{{MANX_SDK_INCLUDE}}" CACHE PATH
    "Directory containing manx_game_plugin.h")
if(NOT EXISTS "${{MANX_SDK_INCLUDE}}/manx_game_plugin.h")
    message(FATAL_ERROR "Set MANX_SDK_INCLUDE to MANX/include")
endif()
add_library({args.short_name} SHARED game_plugin.cpp)
target_include_directories({args.short_name} PRIVATE "${{MANX_SDK_INCLUDE}}")
target_compile_options({args.short_name} PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties({args.short_name} PROPERTIES
    PREFIX "" OUTPUT_NAME "{args.short_name}"
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
'''

    readme = f'''# {args.display_name} — MANX plugin

Generated with `tools/create_game_plugin.py`.

Build:

```sh
MANX_SDK_INCLUDE=/path/to/MANX/include cmake -S . -B build
cmake --build build
```

Install the resulting `{args.short_name}.so` as:

```text
$XDG_DATA_HOME/MANX/games/{args.short_name}/{args.short_name}.so
```

The example declares `First Run`, restores its prior state on launch, and emits
absolute progress. MANX—not the plugin—saves it per OS user at:

```text
$XDG_DATA_HOME/MANX/achievements/{args.short_name}.state
```

Achievement IDs are permanent. Never reuse an ID for a different achievement,
never decrease progress, and always emit absolute rather than delta progress.
'''

    (destination / "game_plugin.cpp").write_text(source, encoding="utf-8")
    (destination / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
    (destination / "README.md").write_text(readme, encoding="utf-8")
    print(f"Created MANX game plugin: {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
