#include "arcade_frontend.h"
#include "arcade_catalog.h"
#include "high_scores.h"
#include "input_mapper.h"
#include "launcher_menu.h"
#include "multiplayer_lobby.h"
#include "persistent_data.h"
#include "platform_file_dialog.h"
#include "rom_library.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {
using board_counts = std::array<int, arcade_board_count>;

int board_slot(arcade_board_type board) {
    const std::size_t index = arcade_board_index(board);
    return index == arcade_board_count ? -1 : static_cast<int>(index);
}

int game_board_slot(std::string_view short_name) {
    const rom_set_manifest* manifest = find_supported_rom_set(short_name);
    return manifest ? board_slot(manifest->board) : -1;
}

int choose_board(launcher_menu& menu, const std::string& title,
                 const std::string& description, const board_counts& counts,
                 bool include_empty, const std::string& back_label) {
    std::vector<int> slots;
    std::vector<std::string> labels;
    const arcade_board_list& boards = arcade_boards();
    for (std::size_t slot = 0; slot < boards.size(); ++slot) {
        if (!include_empty && counts[slot] == 0) continue;
        slots.push_back(static_cast<int>(slot));
        labels.emplace_back(std::string(boards[slot].display_name) + "  (" +
                            std::to_string(counts[slot]) + ")");
    }
    const int selected = menu.select(title, description, labels, back_label);
    return selected >= 0 && selected < static_cast<int>(slots.size()) ?
        slots[static_cast<std::size_t>(selected)] : -1;
}

std::string normalized_path(const fs::path& path) {
    std::error_code error;
    fs::path absolute = fs::absolute(path, error);
    return (error ? path : absolute).lexically_normal().string();
}

bool is_super_system22_set(ridge_racer_rom_set set) {
    return set == ridge_racer_rom_set::time_crisis ||
           set == ridge_racer_rom_set::dirt_dash ||
           set == ridge_racer_rom_set::aqua_jet;
}

const char* dip_switch_name(ridge_racer_rom_set set, int bank, int position) {
    if (set == ridge_racer_rom_set::time_crisis)
        return bank == 0 && position == 7 ? "Test Mode" :
            "Reserved / undocumented - leave OFF";
    if (set == ridge_racer_rom_set::dirt_dash)
        return "Reserved / undocumented - leave OFF";
    const bool ace_family = set == ridge_racer_rom_set::ace_driver ||
                            set == ridge_racer_rom_set::victory_lap;
    if (!ace_family && set != ridge_racer_rom_set::cyber_commando &&
        bank == 0 && position == 0)
        return "Test Mode";
    if (ace_family && bank == 1 && position == 6) return "Test Mode enable";
    if (ace_family && bank == 1 && position == 7) return "Enter Test Mode";
    if (set == ridge_racer_rom_set::ridge_racer_2) {
        if (bank == 0 && position == 3) return "Background debug 1";
        if (bank == 0 && position == 4) return "Background debug 2";
        if (bank == 0 && position == 5) return "Debug link-up";
        if (bank == 0 && position == 7) return "No time-out";
        if (bank == 1 && position == 6) return "Debug polygons";
        if (bank == 1 && position == 7) return "Test Mode 2";
    }
    return "Reserved / undocumented - leave OFF";
}

std::string dip_bank_summary(uint16_t switches, ridge_racer_rom_set set,
                             int bank) {
    std::string result;
    const bool ace_family = set == ridge_racer_rom_set::ace_driver ||
                            set == ridge_racer_rom_set::victory_lap;
    if (is_super_system22_set(set))
        result = bank == 0 ?
            (set == ridge_racer_rom_set::dirt_dash ?
                "SW4 - reserved switches" :
                "SW4 - test mode and reserved switches") :
            "Unused switch bank";
    else if (bank == 0)
        result = set == ridge_racer_rom_set::ridge_racer_2 ?
            "SW2 - test, graphics, link and time debug" :
            (ace_family ? "SW2 - reserved switches" :
                          "SW2 - test mode and reserved switches");
    else
        result = set == ridge_racer_rom_set::ridge_racer_2 ?
            "SW3 - polygon debug and secondary test mode" :
            (ace_family ? "SW3 - test mode and reserved switches" :
                          "SW3 - reserved switches");

    result += "  [ON:";
    bool any_on = false;
    for (int position = 0; position < 8; ++position) {
        if ((switches & (uint16_t{1} << (bank * 8 + position))) != 0) continue;
        result += any_on ? "," : " ";
        result += std::to_string(position + 1);
        any_on = true;
    }
    if (!any_on) result += " none";
    result += "]";
    return result;
}

std::string dip_help_text(ridge_racer_rom_set set) {
    std::string text =
        "Factory setting: ALL OFF. Coinage, difficulty, laps and sound are "
        "configured in the game's service menu, not with these switches.\n\n";
    if (set == ridge_racer_rom_set::time_crisis) {
        text +=
            "Known Time Crisis switch:\n"
            "SW4:8  Test mode\n\n"
            "SW4:1-7 are undocumented and should remain OFF. The second "
            "editor bank is unused by Super System 22.";
    } else if (set == ridge_racer_rom_set::dirt_dash) {
        text +=
            "Dirt Dash SW4:1-8 are undocumented and should remain OFF. Use "
            "F2 for the service/test input. The second editor bank is unused "
            "by Super System 22.";
    } else if (set == ridge_racer_rom_set::aqua_jet) {
        text +=
            "Known Aqua Jet switch:\n"
            "SW4:1  Test mode\n\n"
            "SW4:2-8 are undocumented and should remain OFF. The second "
            "editor bank is unused by Super System 22.";
    } else if (set == ridge_racer_rom_set::ridge_racer_2) {
        text +=
            "Known RR2 switches:\n"
            "SW2:1  Test mode\n"
            "SW2:4-5  Background drawing debug\n"
            "SW2:6  Debug cabinet link\n"
            "SW2:8  Prevent time-out/game over\n"
            "SW3:7  Polygon debug\n"
            "SW3:8  Secondary test mode\n\n"
            "All other positions are undocumented and should remain OFF.";
    } else if (set == ridge_racer_rom_set::ace_driver ||
               set == ridge_racer_rom_set::victory_lap) {
        text +=
            "Known Ace Driver switches:\n"
            "SW3:7-8  Enable and enter test mode\n\n"
            "All other positions are undocumented and should remain OFF.";
    } else if (set == ridge_racer_rom_set::cyber_commando) {
        text += "All physical switch positions are undocumented and should "
                "remain OFF. Use F2 for the service/test input.";
    } else {
        text +=
            "Known switch:\n"
            "SW2:1  Test mode\n\n"
            "SW2:2-8 and every SW3 position are undocumented/reserved and "
            "should remain OFF.";
    }
    return text;
}

void edit_dip_bank(uint16_t& switches, ridge_racer_rom_set set, int bank) {
    for (;;) {
        std::vector<std::string> labels;
        labels.reserve(9);
        for (int position = 0; position < 8; ++position) {
            const int bit = bank * 8 + position;
            const bool on = (switches & (uint16_t{1} << bit)) == 0;
            const int physical_bank =
                is_super_system22_set(set) ? bank + 4 : bank + 2;
            labels.emplace_back("SW" + std::to_string(physical_bank) + ":" +
                std::to_string(position + 1) + "  " +
                dip_switch_name(set, bank, position) +
                (on ? "  [ON]" : "  [OFF]"));
        }
        labels.emplace_back("Back");

        std::vector<SDL_MessageBoxButtonData> buttons;
        buttons.reserve(labels.size());
        for (int position = 0; position < 8; ++position)
            buttons.push_back({0, position, labels[position].c_str()});
        buttons.push_back({SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT |
                           SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT,
                           8, labels[8].c_str()});

        const std::string title = "DIP bank SW" + std::to_string(
            is_super_system22_set(set) ? bank + 4 : bank + 2);
        const SDL_MessageBoxData box{
            SDL_MESSAGEBOX_INFORMATION,
            nullptr,
            title.c_str(),
            "Select a named switch to toggle it. Factory operation is all OFF. "
            "Reserved switches may enable incomplete hardware/debug paths.",
            static_cast<int>(buttons.size()),
            buttons.data(),
            nullptr,
        };
        int selected = 8;
        if (!SDL_ShowMessageBox(&box, &selected) ||
            selected < 0 || selected >= 8)
            return;
        switches ^= static_cast<uint16_t>(uint16_t{1} <<
                                           (bank * 8 + selected));
    }
}

void show_action_result(launcher_menu& menu, const char* title, bool success,
                        const std::string& message) {
    menu.show_text(success ? title : std::string(title) + " - Error", message);
}

void audit_installed_roms(launcher_menu& menu) {
    const std::vector<rom_choice> choices = discover_library_roms({});
    if (choices.empty()) {
        show_action_result(menu, "ROM audit", false,
                           "No supported games were found to audit.");
        return;
    }
    std::ostringstream report;
    std::size_t passed = 0;
    for (const rom_choice& choice : choices) {
        const rom_audit_result result = audit_rom_path(choice.path);
        report << (result.success ? "OK  " : "FAIL  ")
               << (result.set_name.empty() ? choice.label : result.set_name);
        if (!result.success) report << " - " << result.message;
        report << '\n';
        if (result.success) ++passed;
    }
    report << "\n" << passed << " of " << choices.size()
           << " installed games passed.";
    show_action_result(menu, "ROM audit", passed == choices.size(), report.str());
}

void show_rom_library_manager(launcher_menu& menu) {
    for (;;) {
        const std::vector<std::string> items{
            "Audit ROMs",
            "Required MAME sets",
            "ROM / CHD folders",
            "How merged / split sets work",
        };
        const int selected = menu.select(
            "ROM Folders",
            "WhittyArcade reads ROM ZIPs and disc images straight from its ROM "
            "and CHD folders. Drop supported sets in; nothing is imported or "
            "copied.",
            items, "Back to Main Menu");
        if (selected < 0) return;
        if (selected == 0) {
            audit_installed_roms(menu);
        } else if (selected == 1) {
            menu.show_text("Required MAME sets", required_rom_sets_text());
        } else if (selected == 2) {
            const std::string message =
                "WhittyArcade reads games directly from these folders:\n\n"
                "ROM folder:\n" + rom_library_path() +
                "\n\nCHD folder:\n" + chd_library_path() +
                "\n\nDrop supported MAME set ZIPs into the ROM folder and disc "
                "images (rrv1-a.chd) into the CHD folder. Files are read in "
                "place - nothing is copied, extracted or repacked.";
            menu.show_text("ROM / CHD folders", message);
        } else if (selected == 3) {
            menu.show_text(
                "MAME archive layouts",
                "Non-merged: each game ZIP contains every ROM it needs.\n\n"
                "Split: clone ZIPs omit shared parent ROMs. Keep the named "
                "parent ZIP beside the game; WhittyArcade searches both.\n\n"
                "Merged: a parent ZIP contains parent and clone subfolders. "
                "WhittyArcade searches entries by basename, so the merged "
                "parent archive can be selected directly.\n\n"
                "System 22 C71/C74 device firmware remains in its two small "
                "MAME device archives for every layout.");
        }
    }
}

std::string persistent_file_summary(const persistent_game_info& game) {
    std::size_t present = 0;
    for (const auto& file : game.files)
        if (file.exists && file.actual_size == file.expected_size) ++present;
    if (!present) return "no save yet";
    if (present == game.files.size()) return "saved";
    return "incomplete";
}

void show_persistent_game_manager(launcher_menu& menu,
                                  const std::string& short_name) {
    for (;;) {
        const auto games = persistent_games();
        const persistent_game_info* game = find_persistent_game(games, short_name);
        if (!game) return;
        std::ostringstream details;
        for (const persistent_file_info& file : game->files) {
            details << file.label << ": ";
            if (!file.exists) details << "not created";
            else details << file.actual_size << " bytes, " << file.modified;
            details << '\n';
        }
        details << "\nChanges take effect the next time the game starts.";
        const std::string details_text = details.str();
        const std::vector<std::string> items{
            "Create backup",
            "Export...",
            "Import / restore...",
            "Reset to factory defaults...",
        };
        const int selected = menu.select(game->display_name, details_text,
                                         items, "Back to Games");
        if (selected < 0) return;
        persistent_action_result result;
        if (selected == 0) {
            result = backup_persistent_game(short_name);
        } else if (selected == 1) {
            const auto paths = platform_file_selection(
                true, false, "Choose a save export folder");
            if (paths.empty()) continue;
            result = export_persistent_game(short_name, paths.front());
        } else if (selected == 2) {
            const bool folder = game->files.size() > 1;
            const auto paths = platform_file_selection(
                folder, false, folder ?
                    "Choose a folder containing eeprom and backup1" :
                    "Choose an EEPROM file");
            if (paths.empty()) continue;
            result = import_persistent_game(short_name, paths.front());
        } else {
            const std::vector<std::string> confirm_items{
                "Back up and factory reset",
            };
            const int answer = menu.select(
                "Confirm factory reset",
                "The current EEPROM/NVRAM will be backed up before it is "
                "removed. The game will recreate factory data on next boot.",
                confirm_items, "Keep current data", -1);
            if (answer != 0) continue;
            result = reset_persistent_game(short_name);
        }
        show_action_result(menu, "Saved data", result.success, result.message);
    }
}

void show_eeprom_manager(launcher_menu& menu) {
    for (;;) {
        auto games = persistent_games();
        board_counts counts{};
        for (const persistent_game_info& game : games) {
            const int slot = game_board_slot(game.short_name);
            if (slot >= 0) ++counts[static_cast<std::size_t>(slot)];
        }
        const int slot = choose_board(
            menu, "EEPROM / NVRAM Manager",
            "Choose a board, then a game. Back up, restore, export or "
            "factory-reset its operator data.",
            counts, false, "Back to Main Menu");
        if (slot < 0) return;

        for (;;) {
            games = persistent_games();
            std::vector<std::size_t> game_indices;
            std::vector<std::string> labels;
            for (std::size_t index = 0; index < games.size(); ++index) {
                if (game_board_slot(games[index].short_name) != slot) continue;
                game_indices.push_back(index);
                labels.push_back(games[index].display_name + "  [" +
                                 persistent_file_summary(games[index]) + "]");
            }
            const std::string description =
                "Choose a game. Storage root: " + nvram_root_path();
            const int selected = menu.select(
                std::string(arcade_boards()[static_cast<std::size_t>(slot)]
                                .display_name) +
                    " Saved Data",
                description, labels, "Back to Boards");
            if (selected < 0 ||
                selected >= static_cast<int>(game_indices.size()))
                break;
            show_persistent_game_manager(
                menu, games[game_indices[static_cast<std::size_t>(selected)]]
                          .short_name);
        }
    }
}

void show_high_score_viewer(launcher_menu& menu) {
    for (;;) {
        const auto& manifests = supported_rom_sets();
        board_counts counts{};
        for (const auto& manifest : manifests) {
            if (!manifest.working) continue;
            const int slot = board_slot(manifest.board);
            if (slot >= 0) ++counts[static_cast<std::size_t>(slot)];
        }
        const int slot = choose_board(
            menu, "High Scores",
            "Choose a board, then a game. Tables are shown only when their "
            "binary layout and checksum are verified.",
            counts, false, "Back to Main Menu");
        if (slot < 0) return;

        for (;;) {
            std::vector<const rom_set_manifest*> games;
            std::vector<std::string> labels;
            for (const auto& manifest : manifests) {
                if (!manifest.working || board_slot(manifest.board) != slot)
                    continue;
                games.push_back(&manifest);
                labels.emplace_back(manifest.display_name);
            }
            const int selected = menu.select(
                std::string(arcade_boards()[static_cast<std::size_t>(slot)]
                                .display_name) +
                    " High Scores",
                "Choose a game to view its saved score table.", labels,
                "Back to Boards");
            if (selected < 0 || selected >= static_cast<int>(games.size()))
                break;
            const rom_set_manifest& game =
                *games[static_cast<std::size_t>(selected)];
            menu.show_text(game.display_name,
                           high_score_report(game.short_name), "Back to Games");
        }
    }
}

} // namespace

std::vector<rom_choice> discover_rom_choices(const std::string& current_path) {
    return discover_library_roms(current_path);
}

rom_selection_result show_rom_selector(const std::string& current_path,
                                       multiplayer_lobby* lobby) {
    launcher_menu menu;
    std::vector<rom_choice> choices = discover_rom_choices(current_path);
    const auto linked_result = [&](std::string_view short_name, int node) {
        for (const rom_choice& choice : choices) {
            const auto identity = identify_arcade_game(choice.path);
            if (identity && short_name == identity->short_name)
                return rom_selection_result{
                    rom_selection_action::selected, choice.path,
                    cabinet_launch_mode::linked_network, node};
        }
        return rom_selection_result{};
    };
    const auto take_remote_launch = [&]() -> rom_selection_result {
        if (!lobby) return {};
        const std::optional<std::string> game = lobby->take_launch();
        if (!game) return {};
        rom_selection_result result = linked_result(*game, 2);
        if (result.action == rom_selection_action::selected) return result;
        menu.show_text(
            "Multiplayer game unavailable",
            "Player 1 selected " + *game +
                ", but that ROM is not installed in this WhittyArcade "
                "library.", "Back to Main Menu");
        return {};
    };

    for (;;) {
        if (lobby) lobby->set_installed_games(choices);
        std::vector<std::string> main_items{
            "Go Arcade",
            lobby && lobby->connected() ?
                "Multiplayer  [CONNECTED]" :
                "Multiplayer  [SEARCHING]",
            "ROM Folders",
            "Controllers / Keyboard",
            "EEPROM / NVRAM Manager",
            "High Scores",
        };
        const std::string description = choices.empty() ?
            "No games are installed yet. Open ROM Folders to see where to "
            "place MAME ZIP archives and disc images." :
            "Choose where you want to go. Games are organised by their "
            "original arcade hardware.";
        const bool lobby_connected_at_draw =
            lobby && lobby->connected();
        const int selected_page = lobby ?
            menu.select_interruptible(
                "WhittyArcade", description, main_items,
                "Exit WhittyArcade", 0,
                [lobby, lobby_connected_at_draw] {
                    return lobby->launch_pending() ||
                           lobby->connected() != lobby_connected_at_draw;
                }) :
            menu.select("WhittyArcade", description, main_items,
                        "Exit WhittyArcade");
        if (selected_page == launcher_menu::interrupted) {
            rom_selection_result remote = take_remote_launch();
            if (remote.action == rom_selection_action::selected) return remote;
            continue;
        }
        if (selected_page < 0)
            return {rom_selection_action::exit_requested, {}};
        if (selected_page == 1) {
            if (!lobby) {
                menu.show_text(
                    "Multiplayer",
                    "Automatic multiplayer discovery is available when "
                    "WhittyArcade is opened without a ROM on the command "
                    "line.");
                continue;
            }
            for (;;) {
                if (lobby->launch_pending()) {
                    rom_selection_result remote = take_remote_launch();
                    if (remote.action == rom_selection_action::selected)
                        return remote;
                }
                if (!lobby->connected()) {
                    const int waiting = menu.select_interruptible(
                        "Multiplayer - Finding Player 2",
                        "Open WhittyArcade on the second screen or computer. "
                        "The apps detect each other automatically; no cabinet "
                        "role or IP address is required.",
                        {"Searching for another WhittyArcade..."},
                        "Back to Main Menu", 0, [lobby] {
                            return lobby->connected() ||
                                   lobby->launch_pending();
                        });
                    if (waiting == -1) break;
                    continue;
                }
                if (lobby->node() == 2) {
                    const int waiting = menu.select_interruptible(
                        "Multiplayer - Player 2 Connected",
                        "Player 1 is choosing the game. This screen will "
                        "launch automatically.",
                        {"Waiting for Player 1..."},
                        "Back to Main Menu", 0,
                        [lobby] {
                            return lobby->launch_pending() ||
                                   !lobby->connected();
                        });
                    if (waiting == -1) break;
                    continue;
                }

                std::vector<std::size_t> linked_indices;
                std::vector<std::string> linked_labels;
                for (std::size_t index = 0; index < choices.size(); ++index) {
                    const auto identity =
                        identify_arcade_game(choices[index].path);
                    const rom_set_manifest* manifest = identity ?
                        find_supported_rom_set(identity->short_name) : nullptr;
                    if (!identity || !manifest || !manifest->working ||
                        manifest->multiplayer ==
                            arcade_multiplayer_mode::none ||
                        !lobby->peer_has_game(identity->short_name))
                        continue;
                    linked_indices.push_back(index);
                    linked_labels.push_back(choices[index].label);
                }
                const int selected = menu.select_interruptible(
                    "Multiplayer - Player 2 Connected",
                    linked_labels.empty() ?
                        "No supported multiplayer ROM is installed on both "
                        "systems." :
                        "Choose once here. Player 2 will launch the same game "
                        "automatically.",
                    linked_labels, "Back to Main Menu", 0,
                    [lobby] { return !lobby->connected(); });
                if (selected == launcher_menu::interrupted) continue;
                if (selected < 0) break;
                if (selected >= static_cast<int>(linked_indices.size()))
                    continue;
                const rom_choice& choice = choices[
                    linked_indices[static_cast<std::size_t>(selected)]];
                const auto identity = identify_arcade_game(choice.path);
                if (!identity) continue;
                lobby->launch_game(identity->short_name);
                return {rom_selection_action::selected, choice.path,
                        cabinet_launch_mode::linked_network, 1};
            }
            continue;
        }
        if (selected_page == 2) {
            show_rom_library_manager(menu);
            choices = discover_rom_choices(current_path);
            continue;
        }
        if (selected_page == 3) {
            show_input_mapper(menu, choices);
            continue;
        }
        if (selected_page == 4) {
            show_eeprom_manager(menu);
            continue;
        }
        if (selected_page == 5) {
            show_high_score_viewer(menu);
            continue;
        }

        // Go Arcade owns the game selector. Returning from either nested page
        // always lands on this compact main menu rather than a desktop button
        // strip.
        for (;;) {
            board_counts counts{};
            for (const rom_choice& choice : choices) {
                const int slot = board_slot(choice.board);
                if (slot >= 0) ++counts[static_cast<std::size_t>(slot)];
            }
            const int selected_board = choose_board(
                menu, "Go Arcade",
                "Choose the original hardware board. Classic 2D systems are "
                "listed separately, just like the 3D boards.",
                counts, true, "Back to Main Menu");
            if (selected_board < 0) break;

            const arcade_board_type board =
                arcade_boards()[static_cast<std::size_t>(selected_board)].type;
            std::vector<std::size_t> game_indices;
            std::vector<std::string> labels;
            int current_selection = 0;
            for (std::size_t index = 0; index < choices.size(); ++index) {
                if (choices[index].board != board) continue;
                if (choices[index].path == normalized_path(current_path))
                    current_selection = static_cast<int>(game_indices.size());
                game_indices.push_back(index);
                labels.push_back(choices[index].label);
            }
            const int selected_game = menu.select(
                std::string(arcade_boards()[
                                static_cast<std::size_t>(selected_board)]
                                .display_name) + " Games",
                game_indices.empty() ?
                    "No installed ROM archives were found for this board. "
                    "Open ROM Folders from the Main Menu to see where to place "
                    "the required files." :
                    "Choose a game to start.",
                labels, "Back to Boards", current_selection);
            if (selected_game < 0 ||
                selected_game >= static_cast<int>(game_indices.size()))
                continue;

            const rom_choice& choice = choices[
                game_indices[static_cast<std::size_t>(selected_game)]];
            const std::optional<arcade_game_identity> identity =
                identify_arcade_game(choice.path);
            const rom_set_manifest* manifest = identity ?
                find_supported_rom_set(identity->short_name) : nullptr;
            if (manifest && !manifest->working) {
                menu.show_text(
                    std::string(manifest->display_name) + " - Not Working",
                    "This ROM set is recognised but is not supported by the "
                    "current hardware implementation.", "Back to Games");
                continue;
            }
            const bool linked_model2 =
                identity && std::string_view(identity->short_name) == "srallyc";
            const arcade_multiplayer_mode multiplayer = manifest ?
                manifest->multiplayer : arcade_multiplayer_mode::none;
            std::vector<std::string> launch_items{
                "Single Screen  |  one arcade display",
                "Twin Screen Fullscreen  |  two bordered panes",
                "Twin Screen Two Windows  |  same desktop",
                "Twin Screen Two Monitors  |  one window per monitor",
            };
            const char* multiplayer_description =
                multiplayer == arcade_multiplayer_mode::alternating ?
                    "This is an original two-player alternating-turn game. " :
                multiplayer == arcade_multiplayer_mode::simultaneous ?
                    "This game has simultaneous P1 and P2 controls. " :
                multiplayer == arcade_multiplayer_mode::native_link ?
                    "This game uses native linked arcade cabinets. " :
                    "This title has no supported Player 2 input path. ";
            const int launch = menu.select(
                "Start " + choice.label,
                linked_model2 ?
                    "Single Screen runs one Sega cabinet. Twin Screen starts "
                    "two locally linked Sega cabinets, fitted independently "
                    "at the board's native pixel resolution. Choose fullscreen "
                    "panes, two windows or one window on each monitor. "
                    "Use Multiplayer on the "
                    "Main Menu to link two WhittyArcade apps." :
                    std::string(multiplayer_description) +
                    "Single Screen uses one correctly fitted arcade viewport. "
                    "Twin Screen mirrors the same session into two bordered "
                    "native-raster viewports with one integer scale on both "
                    "axes. Choose fullscreen panes, two windows on one desktop "
                    "or one window on each monitor. "
                    "Use Multiplayer on "
                    "the Main Menu for two-app play.",
                launch_items, "Back to Games");
            if (launch < 0) continue;
            return {
                rom_selection_action::selected,
                choice.path,
                launch == 0 ? cabinet_launch_mode::single :
                    (linked_model2 ? cabinet_launch_mode::linked_pair :
                                     cabinet_launch_mode::independent_pair),
                0,
                launch == 0 ? -1 : (launch == 1 ? 1 : 0),
                launch == 3,
            };
        }
    }
}

void show_dip_switches(uint16_t& switches, ridge_racer_rom_set set) {
    for (;;) {
        const std::array<std::string, 4> labels{{
            dip_bank_summary(switches, set, 0),
            dip_bank_summary(switches, set, 1),
            "Reset all switches OFF",
            "Done",
        }};
        const std::array<SDL_MessageBoxButtonData, 4> buttons{{
            {0, 0, labels[0].c_str()},
            {0, 1, labels[1].c_str()},
            {0, 2, labels[2].c_str()},
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT |
             SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 3, labels[3].c_str()},
        }};
        const std::string help = dip_help_text(set);
        const SDL_MessageBoxData box{
            SDL_MESSAGEBOX_INFORMATION,
            nullptr,
            "System 22 DIP switches",
            help.c_str(),
            static_cast<int>(buttons.size()),
            buttons.data(),
            nullptr,
        };
        int selected = 3;
        if (!SDL_ShowMessageBox(&box, &selected) ||
            selected < 0 || selected == 3)
            return;
        if (selected == 2)
            switches = 0xffff;
        else if (selected == 0 || selected == 1)
            edit_dip_bank(switches, set, selected);
    }
}

void show_system16b_dip_switches(uint16_t& switches) {
    struct option {
        uint8_t value;
        const char* name;
    };
    constexpr std::array<option, 11> coinage{{
        {0x0f, "1 coin / 1 credit"},
        {0x09, "2 coins / 1 credit"},
        {0x08, "3 coins / 1 credit"},
        {0x07, "4 coins / 1 credit"},
        {0x06, "2 coins / 3 credits"},
        {0x0e, "1 coin / 2 credits"},
        {0x0d, "1 coin / 3 credits"},
        {0x0c, "1 coin / 4 credits"},
        {0x0b, "1 coin / 5 credits"},
        {0x0a, "1 coin / 6 credits"},
        {0x00, "Free play when A and B are both 0"},
    }};
    const auto coinage_name = [&](uint8_t value) {
        const auto found = std::find_if(
            coinage.begin(), coinage.end(),
            [value](const option& item) { return item.value == value; });
        return found == coinage.end() ? "Custom" : found->name;
    };
    const auto cycle_coinage = [&](uint8_t value) {
        const auto found = std::find_if(
            coinage.begin(), coinage.end(),
            [value](const option& item) { return item.value == value; });
        const std::size_t index = found == coinage.end() ? 0 :
            (static_cast<std::size_t>(found - coinage.begin()) + 1) %
                coinage.size();
        return coinage[index].value;
    };
    const auto cycle_field = [](uint8_t current, uint8_t mask,
                                std::initializer_list<uint8_t> values) {
        auto found = std::find(values.begin(), values.end(), current & mask);
        if (found == values.end() || ++found == values.end())
            return *values.begin();
        return *found;
    };

    for (;;) {
        const uint8_t sw1 = static_cast<uint8_t>(switches);
        const uint8_t sw2 = static_cast<uint8_t>(switches >> 8);
        const char* lives = (sw2 & 0x0c) == 0x08 ? "2" :
                            (sw2 & 0x0c) == 0x0c ? "3" :
                            (sw2 & 0x0c) == 0x04 ? "5" : "Free play";
        const char* difficulty = (sw2 & 0x30) == 0x20 ? "Easy" :
                                 (sw2 & 0x30) == 0x30 ? "Normal" :
                                 (sw2 & 0x30) == 0x10 ? "Hard" : "Hardest";
        const std::array<std::string, 10> labels{{
            std::string("Coin A  [") + coinage_name(sw1 & 0x0f) + "]",
            std::string("Coin B  [") + coinage_name(sw1 >> 4) + "]",
            std::string("Cabinet  [") + ((sw2 & 0x01) ? "Cocktail]" : "Upright]"),
            std::string("Demo sounds  [") + ((sw2 & 0x02) ? "OFF]" : "ON]"),
            std::string("Lives  [") + lives + "]",
            std::string("Difficulty  [") + difficulty + "]",
            std::string("Enemy bullet speed  [") +
                ((sw2 & 0x40) ? "Slow]" : "Fast]"),
            std::string("Language  [") +
                ((sw2 & 0x80) ? "Japanese]" : "English]"),
            "Restore English factory defaults",
            "Done",
        }};
        std::array<SDL_MessageBoxButtonData, 10> buttons{};
        for (int index = 0; index < 10; ++index) {
            buttons[static_cast<std::size_t>(index)] = {
                static_cast<Uint32>(
                    index == 9 ? SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT |
                                     SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0),
                index, labels[static_cast<std::size_t>(index)].c_str()};
        }
        const SDL_MessageBoxData box{
            SDL_MESSAGEBOX_INFORMATION,
            nullptr,
            "Shinobi cabinet DIP switches",
            "These are the named Sega System 16B switches from MAME. "
            "WhittyArcade defaults to English, upright, demo sound on, "
            "three lives and normal difficulty.",
            static_cast<int>(buttons.size()),
            buttons.data(),
            nullptr,
        };
        int selected = 9;
        if (!SDL_ShowMessageBox(&box, &selected) || selected < 0 ||
            selected == 9)
            return;

        uint8_t next_sw1 = sw1;
        uint8_t next_sw2 = sw2;
        switch (selected) {
        case 0:
            next_sw1 = static_cast<uint8_t>(
                (sw1 & 0xf0) | cycle_coinage(sw1 & 0x0f));
            break;
        case 1:
            next_sw1 = static_cast<uint8_t>(
                (sw1 & 0x0f) | (cycle_coinage(sw1 >> 4) << 4));
            break;
        case 2: next_sw2 ^= 0x01; break;
        case 3: next_sw2 ^= 0x02; break;
        case 4:
            next_sw2 = static_cast<uint8_t>(
                (sw2 & ~0x0c) |
                cycle_field(sw2, 0x0c, {0x0c, 0x08, 0x04, 0x00}));
            break;
        case 5:
            next_sw2 = static_cast<uint8_t>(
                (sw2 & ~0x30) |
                cycle_field(sw2, 0x30, {0x30, 0x20, 0x10, 0x00}));
            break;
        case 6: next_sw2 ^= 0x40; break;
        case 7: next_sw2 ^= 0x80; break;
        case 8:
            // SW1 0xff is 1C/1C on both chutes. SW2 0x7c selects
            // upright, demo sounds on, 3 lives, normal, slow bullets,
            // English.
            next_sw1 = 0xff;
            next_sw2 = 0x7c;
            break;
        default: break;
        }
        switches = static_cast<uint16_t>(next_sw1) |
                   (static_cast<uint16_t>(next_sw2) << 8);
    }
}

void show_gng_dip_switches(uint16_t& switches) {
    const auto cycle = [](uint8_t current, uint8_t mask,
                          std::initializer_list<uint8_t> values) {
        auto found = std::find(values.begin(), values.end(), current & mask);
        if (found == values.end() || ++found == values.end())
            return *values.begin();
        return *found;
    };
    const auto field_name = [](uint8_t value, uint8_t mask,
                               std::initializer_list<uint8_t> values,
                               std::initializer_list<const char*> names) {
        auto item = values.begin();
        auto name = names.begin();
        while (item != values.end() && name != names.end()) {
            if ((value & mask) == *item) return *name;
            ++item; ++name;
        }
        return "Custom";
    };
    for (;;) {
        const uint8_t sw1 = static_cast<uint8_t>(switches);
        const uint8_t sw2 = static_cast<uint8_t>(switches >> 8);
        const std::array<std::string, 11> labels{{
            std::string("Coinage  [") + field_name(
                sw1, 0x0f, {0x0f,0x08,0x0e,0x0d,0x00},
                {"1C/1C","2C/1C","1C/2C","1C/3C","Free play"}) + "]",
            std::string("Coinage affects  [") +
                ((sw1 & 0x10) ? "Coin A]" : "Coin B]"),
            std::string("Demo sounds  [") +
                ((sw1 & 0x20) ? "OFF]" : "ON]"),
            std::string("Service mode  [") +
                ((sw1 & 0x40) ? "OFF]" : "ON]"),
            std::string("Flip screen  [") +
                ((sw1 & 0x80) ? "OFF]" : "ON]"),
            std::string("Lives  [") + field_name(
                sw2, 0x03, {0x03,0x02,0x01,0x00}, {"3","4","5","7"}) + "]",
            std::string("Cabinet  [") +
                ((sw2 & 0x04) ? "Cocktail]" : "Upright]"),
            std::string("Bonus life  [") + field_name(
                sw2, 0x18, {0x18,0x10,0x08,0x00},
                {"20K 70K every 70K","30K 80K every 80K",
                 "20K and 80K only","30K and 80K only"}) + "]",
            std::string("Difficulty  [") + field_name(
                sw2, 0x60, {0x40,0x60,0x20,0x00},
                {"Easy","Normal","Difficult","Very difficult"}) + "]",
            "Restore factory defaults",
            "Done",
        }};
        std::array<SDL_MessageBoxButtonData, 11> buttons{};
        for (int index = 0; index < 11; ++index) {
            buttons[index] = {
                static_cast<Uint32>(index == 10 ?
                    SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT |
                    SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0),
                index, labels[index].c_str()};
        }
        const SDL_MessageBoxData box{
            SDL_MESSAGEBOX_INFORMATION, nullptr,
            "Ghosts'n Goblins cabinet DIP switches",
            "Capcom factory defaults are upright, demo sound on, three lives, "
            "normal difficulty and one coin per credit.",
            static_cast<int>(buttons.size()), buttons.data(), nullptr};
        int selected = 10;
        if (!SDL_ShowMessageBox(&box, &selected) || selected < 0 ||
            selected == 10)
            return;
        uint8_t next1 = sw1, next2 = sw2;
        switch (selected) {
        case 0: next1 = static_cast<uint8_t>((sw1 & 0xf0) |
            cycle(sw1, 0x0f, {0x0f,0x08,0x0e,0x0d,0x00})); break;
        case 1: next1 ^= 0x10; break;
        case 2: next1 ^= 0x20; break;
        case 3: next1 ^= 0x40; break;
        case 4: next1 ^= 0x80; break;
        case 5: next2 = static_cast<uint8_t>((sw2 & ~0x03) |
            cycle(sw2, 0x03, {0x03,0x02,0x01,0x00})); break;
        case 6: next2 ^= 0x04; break;
        case 7: next2 = static_cast<uint8_t>((sw2 & ~0x18) |
            cycle(sw2, 0x18, {0x18,0x10,0x08,0x00})); break;
        case 8: next2 = static_cast<uint8_t>((sw2 & ~0x60) |
            cycle(sw2, 0x60, {0x40,0x60,0x20,0x00})); break;
        case 9: next1 = 0xdf; next2 = 0xfb; break;
        default: break;
        }
        switches = static_cast<uint16_t>(next1 | (next2 << 8));
    }
}

void show_model1_cabinet_settings(bool& attract_sound_enabled) {
    for (;;) {
        const std::array<std::string, 3> labels{{
            std::string("Advertise / attract sound  [") +
                (attract_sound_enabled ? "ON]" : "OFF]"),
            "Physical DSW1-DSW3 status",
            "Done",
        }};
        const std::array<SDL_MessageBoxButtonData, 3> buttons{{
            {0, 0, labels[0].c_str()},
            {0, 1, labels[1].c_str()},
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT |
             SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 2, labels[2].c_str()},
        }};
        const SDL_MessageBoxData box{
            SDL_MESSAGEBOX_INFORMATION,
            nullptr,
            "Sega Model 1 cabinet settings",
            "Virtua Racing / Formula stores game options in its operator "
            "EEPROM. Its three physical 8-way I/O DIP banks are unused and "
            "should remain all OFF. F2 opens the original game service menu.",
            static_cast<int>(buttons.size()),
            buttons.data(),
            nullptr,
        };
        int selected = 2;
        if (!SDL_ShowMessageBox(&box, &selected) ||
            selected < 0 || selected == 2)
            return;
        if (selected == 0) {
            attract_sound_enabled = !attract_sound_enabled;
        } else {
            SDL_ShowSimpleMessageBox(
                SDL_MESSAGEBOX_INFORMATION,
                "Model 1 physical DIP banks",
                "DSW1: 1-8 OFF (unused)\n"
                "DSW2: 1-8 OFF (unused)\n"
                "DSW3: 1-8 OFF (unused)\n\n"
                "Coinage, difficulty, link/cabinet options and advertise "
                "sound belong to the EEPROM-backed service settings.",
                nullptr);
        }
    }
}

operator_menu_definition make_system22_operator_menu(
    uint16_t switches, ridge_racer_rom_set set) {
    operator_menu_definition menu;
    menu.title = "SYSTEM 22 DIP SWITCHES";
    menu.description =
        "Physical switches only; game options are in the service menu.";
    const int first_bank = is_super_system22_set(set) ? 4 : 2;
    for (int bit = 0; bit < 16; ++bit) {
        const int bank = bit / 8;
        menu.rows.push_back({
            bit,
            "SW" + std::to_string(first_bank + bank) + ":" +
                std::to_string(bit % 8 + 1) + "  " +
                dip_switch_name(set, bank, bit % 8),
            {"OFF", "ON"},
            (switches & (uint16_t{1} << bit)) == 0 ? 1 : 0,
        });
    }
    menu.rows.push_back({100, "Reset all switches OFF", {}, 0, true});
    return menu;
}

void apply_system22_operator_action(uint16_t& switches,
                                    const operator_menu_action& action) {
    if (action.row_id == 100) {
        switches = 0xffff;
    } else if (action.row_id >= 0 && action.row_id < 16) {
        const uint16_t mask = static_cast<uint16_t>(
            uint16_t{1} << action.row_id);
        if (action.selected == 0) switches |= mask;
        else switches &= static_cast<uint16_t>(~mask);
    }
}

operator_menu_definition make_system16b_operator_menu(uint16_t switches) {
    constexpr std::array<const char*, 11> coinage{{
        "1 coin / 1 credit", "2 coins / 1 credit", "3 coins / 1 credit",
        "4 coins / 1 credit", "2 coins / 3 credits", "1 coin / 2 credits",
        "1 coin / 3 credits", "1 coin / 4 credits", "1 coin / 5 credits",
        "1 coin / 6 credits", "Free play",
    }};
    constexpr std::array<uint8_t, 11> coin_values{{
        0x0f, 0x09, 0x08, 0x07, 0x06, 0x0e, 0x0d, 0x0c, 0x0b, 0x0a, 0x00,
    }};
    const auto index_of = [](uint8_t value, const auto& values) {
        const auto found = std::find(values.begin(), values.end(), value);
        return found == values.end() ? 0 :
            static_cast<int>(found - values.begin());
    };
    const uint8_t sw1 = static_cast<uint8_t>(switches);
    const uint8_t sw2 = static_cast<uint8_t>(switches >> 8);
    operator_menu_definition menu;
    menu.title = "SHINOBI DIP SWITCHES";
    menu.description = "Sega System 16B cabinet options.";
    const std::vector<std::string> coins(coinage.begin(), coinage.end());
    menu.rows = {
        {0, "Coin A", coins, index_of(sw1 & 0x0f, coin_values)},
        {1, "Coin B", coins, index_of(sw1 >> 4, coin_values)},
        {2, "Cabinet", {"Upright", "Cocktail"}, (sw2 & 0x01) ? 1 : 0},
        {3, "Demo sounds", {"ON", "OFF"}, (sw2 & 0x02) ? 1 : 0},
        {4, "Lives", {"3", "2", "5", "Free play"},
             index_of(static_cast<uint8_t>(sw2 & 0x0c),
                      std::array<uint8_t, 4>{{0x0c, 0x08, 0x04, 0x00}})},
        {5, "Difficulty", {"Normal", "Easy", "Hard", "Hardest"},
             index_of(static_cast<uint8_t>(sw2 & 0x30),
                      std::array<uint8_t, 4>{{0x30, 0x20, 0x10, 0x00}})},
        {6, "Enemy bullet speed", {"Fast", "Slow"}, (sw2 & 0x40) ? 1 : 0},
        {7, "Language", {"English", "Japanese"}, (sw2 & 0x80) ? 1 : 0},
        {8, "Restore English factory defaults", {}, 0, true},
    };
    return menu;
}

void apply_system16b_operator_action(uint16_t& switches,
                                   const operator_menu_action& action) {
    constexpr std::array<uint8_t, 11> coins{{
        0x0f, 0x09, 0x08, 0x07, 0x06, 0x0e, 0x0d, 0x0c, 0x0b, 0x0a, 0x00,
    }};
    constexpr std::array<uint8_t, 4> lives{{0x0c, 0x08, 0x04, 0x00}};
    constexpr std::array<uint8_t, 4> difficulty{{0x30, 0x20, 0x10, 0x00}};
    uint8_t sw1 = static_cast<uint8_t>(switches);
    uint8_t sw2 = static_cast<uint8_t>(switches >> 8);
    const int selected = std::max(action.selected, 0);
    switch (action.row_id) {
    case 0: sw1 = static_cast<uint8_t>((sw1 & 0xf0) | coins[selected % coins.size()]); break;
    case 1: sw1 = static_cast<uint8_t>((sw1 & 0x0f) | (coins[selected % coins.size()] << 4)); break;
    case 2: sw2 = static_cast<uint8_t>((sw2 & ~0x01) | (selected ? 0x01 : 0)); break;
    case 3: sw2 = static_cast<uint8_t>((sw2 & ~0x02) | (selected ? 0x02 : 0)); break;
    case 4: sw2 = static_cast<uint8_t>((sw2 & ~0x0c) | lives[selected % lives.size()]); break;
    case 5: sw2 = static_cast<uint8_t>((sw2 & ~0x30) | difficulty[selected % difficulty.size()]); break;
    case 6: sw2 = static_cast<uint8_t>((sw2 & ~0x40) | (selected ? 0x40 : 0)); break;
    case 7: sw2 = static_cast<uint8_t>((sw2 & ~0x80) | (selected ? 0x80 : 0)); break;
    case 8: sw1 = 0xff; sw2 = 0x7c; break;
    default: return;
    }
    switches = static_cast<uint16_t>(sw1) |
               (static_cast<uint16_t>(sw2) << 8);
}

operator_menu_definition make_gng_operator_menu(uint16_t switches) {
    const auto index_of = [](uint8_t value, const auto& values) {
        const auto found = std::find(values.begin(), values.end(), value);
        return found == values.end() ? 0 :
            static_cast<int>(found - values.begin());
    };
    constexpr std::array<uint8_t, 5> coinage{{0x0f,0x08,0x0e,0x0d,0x00}};
    constexpr std::array<uint8_t, 4> lives{{0x03,0x02,0x01,0x00}};
    constexpr std::array<uint8_t, 4> bonus{{0x18,0x10,0x08,0x00}};
    constexpr std::array<uint8_t, 4> difficulty{{0x40,0x60,0x20,0x00}};
    const uint8_t sw1 = static_cast<uint8_t>(switches);
    const uint8_t sw2 = static_cast<uint8_t>(switches >> 8);
    operator_menu_definition menu;
    menu.title = "GHOSTS'N GOBLINS DIP SWITCHES";
    menu.description = "Capcom cabinet options; changes apply immediately.";
    menu.rows = {
        {0, "Coinage", {"1C/1C","2C/1C","1C/2C","1C/3C","Free play"}, index_of(sw1 & 0x0f, coinage)},
        {1, "Coinage affects", {"Coin B", "Coin A"}, (sw1 & 0x10) ? 1 : 0},
        {2, "Demo sounds", {"ON", "OFF"}, (sw1 & 0x20) ? 1 : 0},
        {3, "Service mode", {"ON", "OFF"}, (sw1 & 0x40) ? 1 : 0},
        {4, "Flip screen", {"ON", "OFF"}, (sw1 & 0x80) ? 1 : 0},
        {5, "Lives", {"3","4","5","7"}, index_of(sw2 & 0x03, lives)},
        {6, "Cabinet", {"Upright", "Cocktail"}, (sw2 & 0x04) ? 1 : 0},
        {7, "Bonus life", {"20K 70K every 70K","30K 80K every 80K","20K and 80K only","30K and 80K only"}, index_of(sw2 & 0x18, bonus)},
        {8, "Difficulty", {"Easy","Normal","Difficult","Very difficult"}, index_of(sw2 & 0x60, difficulty)},
        {9, "Restore factory defaults", {}, 0, true},
    };
    return menu;
}

void apply_gng_operator_action(uint16_t& switches,
                               const operator_menu_action& action) {
    constexpr std::array<uint8_t, 5> coinage{{0x0f,0x08,0x0e,0x0d,0x00}};
    constexpr std::array<uint8_t, 4> lives{{0x03,0x02,0x01,0x00}};
    constexpr std::array<uint8_t, 4> bonus{{0x18,0x10,0x08,0x00}};
    constexpr std::array<uint8_t, 4> difficulty{{0x40,0x60,0x20,0x00}};
    uint8_t sw1 = static_cast<uint8_t>(switches);
    uint8_t sw2 = static_cast<uint8_t>(switches >> 8);
    const int selected = std::max(action.selected, 0);
    switch (action.row_id) {
    case 0: sw1 = static_cast<uint8_t>((sw1 & 0xf0) | coinage[selected % coinage.size()]); break;
    case 1: sw1 = static_cast<uint8_t>((sw1 & ~0x10) | (selected ? 0x10 : 0)); break;
    case 2: sw1 = static_cast<uint8_t>((sw1 & ~0x20) | (selected ? 0x20 : 0)); break;
    case 3: sw1 = static_cast<uint8_t>((sw1 & ~0x40) | (selected ? 0x40 : 0)); break;
    case 4: sw1 = static_cast<uint8_t>((sw1 & ~0x80) | (selected ? 0x80 : 0)); break;
    case 5: sw2 = static_cast<uint8_t>((sw2 & ~0x03) | lives[selected % lives.size()]); break;
    case 6: sw2 = static_cast<uint8_t>((sw2 & ~0x04) | (selected ? 0x04 : 0)); break;
    case 7: sw2 = static_cast<uint8_t>((sw2 & ~0x18) | bonus[selected % bonus.size()]); break;
    case 8: sw2 = static_cast<uint8_t>((sw2 & ~0x60) | difficulty[selected % difficulty.size()]); break;
    case 9: sw1 = 0xdf; sw2 = 0xfb; break;
    default: return;
    }
    switches = static_cast<uint16_t>(sw1) |
               (static_cast<uint16_t>(sw2) << 8);
}

operator_menu_definition make_model1_operator_menu(bool attract_sound_enabled) {
    operator_menu_definition menu;
    menu.title = "SEGA MODEL 1 OPERATOR SETTINGS";
    menu.description = "Game options are stored in operator EEPROM.";
    menu.rows = {
        {0, "Advertise / attract sound", {"OFF", "ON"},
             attract_sound_enabled ? 1 : 0},
        {1, "Physical DSW1-DSW3: all OFF (unused)", {}, 0, false, false},
        {2, "Use F2 for the original service menu", {}, 0, false, false},
    };
    return menu;
}

operator_menu_definition make_service_operator_menu(
    const std::string& title, const std::string& description,
    const std::string& action_label) {
    operator_menu_definition menu;
    menu.title = title;
    menu.description = description;
    menu.rows.push_back({0, action_label, {}, 0, true});
    menu.rows.push_back({1, "F2 is TEST/select inside the service menu",
                         {}, 0, false, false});
    return menu;
}

operator_menu_definition make_unavailable_operator_menu() {
    operator_menu_definition menu;
    menu.description = "No verified host-side DIP editor for this game.";
    menu.rows.push_back({0, "Use F2 for cabinet TEST/select where supported",
                         {}, 0, false, false});
    return menu;
}
