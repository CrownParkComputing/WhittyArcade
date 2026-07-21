#include "arcade_frontend.h"
#include "arcade_catalog.h"
#include "high_scores.h"
#include "input_mapper.h"
#include "launcher_menu.h"
#include "persistent_data.h"
#include "rom_library.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

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
           set == ridge_racer_rom_set::dirt_dash;
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
            (set == ridge_racer_rom_set::time_crisis ?
                "SW4 - test mode and reserved switches" :
                "SW4 - reserved switches") :
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
        if (SDL_ShowMessageBox(&box, &selected) < 0 ||
            selected < 0 || selected >= 8)
            return;
        switches ^= static_cast<uint16_t>(uint16_t{1} <<
                                           (bank * 8 + selected));
    }
}

std::vector<std::string> zenity_selection(bool directory, bool multiple,
                                          const char* title) {
    int output_pipe[2]{};
    if (pipe(output_pipe) != 0) return {};
    const pid_t child = fork();
    if (child < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        return {};
    }
    if (child == 0) {
        dup2(output_pipe[1], STDOUT_FILENO);
        close(output_pipe[0]);
        close(output_pipe[1]);
        if (directory) {
            execlp("zenity", "zenity", "--file-selection", "--directory",
                   "--modal", title, static_cast<char*>(nullptr));
        } else if (multiple) {
            execlp("zenity", "zenity", "--file-selection", "--multiple",
                   "--separator=\n", "--modal",
                   "--file-filter=MAME ROM archives | *.zip *.ZIP", title,
                   static_cast<char*>(nullptr));
        } else {
            execlp("zenity", "zenity", "--file-selection", "--modal", title,
                   static_cast<char*>(nullptr));
        }
        _exit(127);
    }
    close(output_pipe[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    for (;;) {
        const ssize_t count = read(output_pipe[0], buffer.data(), buffer.size());
        if (count <= 0) break;
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    close(output_pipe[0]);
    int status = 0;
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return {};

    std::vector<std::string> paths;
    std::stringstream lines(output);
    std::string path;
    while (std::getline(lines, path)) {
        if (!path.empty() && path.back() == '\r') path.pop_back();
        if (!path.empty()) paths.push_back(std::move(path));
    }
    return paths;
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
            "Import a MAME ROM folder...",
            "Import one or more ZIPs...",
            "Audit installed ROMs",
            "Required MAME sets",
            "ROM storage location",
            "How merged / split sets work",
        };
        const int selected = menu.select(
            "ROM Library / Import",
            "Importing copies supported ZIP archives unchanged into the "
            "WhittyArcade user library. Nothing is extracted or repacked.",
            items, "Back to Main Menu");
        if (selected < 0) return;
        if (selected == 0 || selected == 1) {
            const std::vector<std::string> paths = zenity_selection(
                selected == 0, selected == 1,
                selected == 0 ? "--title=Import a MAME ROM folder" :
                                "--title=Import MAME ROM ZIPs");
            if (paths.empty()) continue;
            const rom_import_result imported = import_rom_paths(paths);
            show_action_result(menu, "ROM import", imported.error.empty(),
                               imported.summary());
        } else if (selected == 2) {
            audit_installed_roms(menu);
        } else if (selected == 3) {
            const std::string requirements = required_rom_sets_text();
            menu.show_text("Required MAME sets", requirements);
        } else if (selected == 4) {
            const std::string message =
                "Imported archives are stored under:\n" + rom_library_path() +
                "\n\nThey are grouped by board and remain ordinary MAME ZIP "
                "files. Removing the original download will not break an "
                "imported game.";
            menu.show_text("ROM storage location", message);
        } else if (selected == 5) {
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
            const auto paths = zenity_selection(
                true, false, "--title=Choose a save export folder");
            if (paths.empty()) continue;
            result = export_persistent_game(short_name, paths.front());
        } else if (selected == 2) {
            const bool folder = game->files.size() > 1;
            const auto paths = zenity_selection(
                folder, false, folder ?
                    "--title=Choose a folder containing eeprom and backup1" :
                    "--title=Choose an EEPROM file");
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

rom_selection_result show_rom_selector(const std::string& current_path) {
    launcher_menu menu;
    std::vector<rom_choice> choices = discover_rom_choices(current_path);
    for (;;) {
        const std::vector<std::string> main_items{
            "Go Arcade",
            "ROM Library / Import",
            "Controllers / Keyboard",
            "EEPROM / NVRAM Manager",
            "High Scores",
        };
        const std::string description = choices.empty() ?
            "No games are installed yet. Open ROM Library / Import to add "
            "MAME ZIP archives, or manage saved data and scores." :
            "Choose where you want to go. Games are organised by their "
            "original arcade hardware.";
        const int selected_page = menu.select(
            "WhittyArcade", description, main_items, "Exit WhittyArcade");
        if (selected_page < 0)
            return {rom_selection_action::exit_requested, {}};
        if (selected_page == 1) {
            show_rom_library_manager(menu);
            choices = discover_rom_choices(current_path);
            continue;
        }
        if (selected_page == 2) {
            show_input_mapper(menu, choices);
            continue;
        }
        if (selected_page == 3) {
            show_eeprom_manager(menu);
            continue;
        }
        if (selected_page == 4) {
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
                    "Use ROM Library / Import from the Main Menu to add one." :
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
            return {rom_selection_action::selected, choice.path};
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
        if (SDL_ShowMessageBox(&box, &selected) < 0 ||
            selected < 0 || selected == 3)
            return;
        if (selected == 2)
            switches = 0xffff;
        else if (selected == 0 || selected == 1)
            edit_dip_bank(switches, set, selected);
    }
}

void show_shinobi_dip_switches(uint16_t& switches) {
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
        if (SDL_ShowMessageBox(&box, &selected) < 0 || selected < 0 ||
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
        if (SDL_ShowMessageBox(&box, &selected) < 0 ||
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
