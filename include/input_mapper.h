// Launcher-facing editor for persistent keyboard and controller mappings.
#pragma once

#include "arcade_types.h"

#include <string>
#include <vector>

class launcher_menu;

void show_input_mapper(launcher_menu& menu,
                       const std::vector<rom_choice>& installed_games);
void show_game_input_mapper(launcher_menu& menu,
                            const std::vector<rom_choice>& installed_games,
                            const std::string& game_short_name);
