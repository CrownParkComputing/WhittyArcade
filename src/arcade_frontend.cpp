#include "arcade_frontend.h"
#include "system246_rom.h"
#include "arcade_catalog.h"
#include "arcade_settings.h"
#include "high_scores.h"
#include "banner_library.h"
#include "wall_capacity.h"
#include "system16_data.h"
#include "play_stats.h"
#include "input_mapper.h"
#include "launcher_menu.h"
#include "media_library.h"
#include "multiplayer_lobby.h"
#include "persistent_data.h"
#include "platform_file_dialog.h"
#include "rom_library.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include "stb_image.h"
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {
using board_counts = std::array<int, arcade_board_count>;

bool hidden_from_launcher(arcade_board_type board) {
    return board == arcade_board_type::game_plugin;
}

fs::path source_media_root();

std::vector<std::string> media_names_for_choice(const rom_choice& choice) {
    std::vector<std::string> names;
    const fs::path path(choice.path);
    const std::string archive_name = path.stem().string();
    if (!archive_name.empty()) names.push_back(archive_name);
    if (const auto identity = identify_arcade_game(choice.path)) {
        if (std::find(names.begin(), names.end(), identity->short_name) ==
            names.end())
            names.push_back(identity->short_name);
    }
    return names;
}

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
        if (hidden_from_launcher(boards[slot].type))
            continue;
        if (!include_empty && counts[slot] == 0) continue;
        slots.push_back(static_cast<int>(slot));
        labels.emplace_back(std::string(boards[slot].display_name) + "  (" +
                            std::to_string(counts[slot]) + ")");
    }
    const int selected = menu.select(title, description, labels, back_label);
    return selected >= 0 && selected < static_cast<int>(slots.size()) ?
        slots[static_cast<std::size_t>(selected)] : -1;
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

bool save_library_folders(launcher_menu& menu, emulator_settings& settings) {
    settings.library_setup_complete = true;
    if (!save_settings(settings)) {
        menu.show_text(
            "Folder settings - Error",
            "MANX could not save the selected folders to:\n\n" +
                settings_path());
        return false;
    }
    // Discovery creates missing default/custom directories and immediately
    // verifies that the new setting can be read back.
    const std::vector<rom_choice> games = discover_library_roms({});
    menu.show_text(
        "Folders saved",
        "ROM folder:\n" + rom_library_path() +
            "\n\nCHD folder:\n" + chd_library_path() +
            "\n\nNAS media source:\n" +
            (settings.media_directory.empty() ?
                std::string("Not configured") : settings.media_directory) +
            "\n\nLocal curated media:\n" +
            manx_media::local_root().string() +
            "\n\nInstalled supported games found: " +
            std::to_string(games.size()));
    return true;
}

void import_installed_game_media(launcher_menu& menu) {
    const fs::path source = source_media_root();
    if (source.empty()) {
        menu.show_text(
            "Import private media",
            "Choose the mounted NAS media source first. For our pack, select "
            "the /Roms/v3_0.285/media folder.", "Back to Folders");
        return;
    }

    std::vector<std::string> short_names;
    for (const rom_choice& choice : discover_library_roms({})) {
        if (hidden_from_launcher(choice.board))
            continue;
        const std::vector<std::string> names = media_names_for_choice(choice);
        short_names.insert(short_names.end(), names.begin(), names.end());
    }
    std::sort(short_names.begin(), short_names.end());
    short_names.erase(std::unique(short_names.begin(), short_names.end()),
                      short_names.end());

    menu.show_splash("Copying artwork and game descriptions", 0.5f);
    const manx_media::import_result imported =
        manx_media::import_installed_games(
            source, manx_media::local_root(), short_names);
    std::ostringstream report;
    report << imported.message
           << "\n\nSource:\n" << source.string()
           << "\n\nLocal mirror:\n" << manx_media::local_root().string()
           << "\n\nNew folders created: " << imported.directories_created
           << "\nFiles copied: " << imported.files_copied
           << "\nDescriptions imported: "
           << imported.descriptions_imported
           << "\nData copied: "
           << (imported.bytes_copied / (1024 * 1024)) << " MB";
    menu.show_text(imported.success ? "Media import complete" :
                                      "Media import failed",
                   report.str(), "Back to Folders");
}

bool choose_library_folders(launcher_menu& menu, bool first_run) {
    emulator_settings settings = load_settings();
    for (;;) {
        const int selected = menu.select_modes(
            first_run ? "Welcome to MANX" : "ROM and CHD folders",
            first_run ?
                "Choose where MANX should look for your existing MAME "
                "ROM archives and disc images. Files stay in place and are "
                "never imported, copied or repacked." :
                "Choose both library locations again, or use MANX's "
                "recommended per-user folders.",
            {{"Choose Library", "Select existing ROM and CHD folders",
              launcher_menu::mode_icon::folder},
             {"Recommended Folders", "Use MANX's per-user library",
              launcher_menu::mode_icon::settings}},
            first_run ? "Exit MANX" : "Cancel");
        if (selected < 0) return false;
        if (selected == 1) {
            settings.rom_directory.clear();
            settings.chd_directory.clear();
            return save_library_folders(menu, settings);
        }

        const std::vector<std::string> rom_folder =
            platform_file_selection(true, false, "Choose your MAME ROM folder");
        if (rom_folder.empty()) continue;
        settings.rom_directory = rom_folder.front();

        const int chd_choice = menu.select(
            "Disc image folder",
            "CHD files can live beside your ROM ZIPs, or in their own folder.",
            {"Choose a separate CHD folder",
             "Use the ROM folder for CHDs"},
            "Back");
        if (chd_choice < 0) continue;
        if (chd_choice == 0) {
            const std::vector<std::string> chd_folder =
                platform_file_selection(
                    true, false, "Choose your arcade CHD folder");
            if (chd_folder.empty()) continue;
            settings.chd_directory = chd_folder.front();
        } else {
            settings.chd_directory = settings.rom_directory;
        }
        return save_library_folders(menu, settings);
    }
}

void show_library_folder_settings(launcher_menu& menu) {
    for (;;) {
        const emulator_settings current = load_settings();
        const std::string description =
            "ROM folder:\n" + rom_library_path() +
            "\n\nCHD folder:\n" + chd_library_path() +
            "\n\nNAS media source:\n" +
            (current.media_directory.empty() ?
                std::string("Not configured") :
                current.media_directory) +
            "\n\nLocal curated media:\n" +
            manx_media::local_root().string();
        const int selected = menu.select_modes(
            "Library and media folders", description,
            {{"ROM + CHD", "Choose both library folders",
              launcher_menu::mode_icon::folder},
             {"ROM Folder", "Change arcade archive location",
              launcher_menu::mode_icon::storage},
             {"CHD Folder", "Change disc-image location",
              launcher_menu::mode_icon::storage},
             {"Shared Folder", "Keep CHDs beside ROM archives",
              launcher_menu::mode_icon::linked_cabinets},
             {"NAS Media", "Set /Roms/v3_0.285/media source",
              launcher_menu::mode_icon::network},
             {"Import Media", "Copy artwork and game descriptions",
              launcher_menu::mode_icon::refresh},
             {"Disconnect NAS", "Keep the local copied media",
              launcher_menu::mode_icon::exit},
             {"Restore Defaults", "Use MANX's recommended folders",
              launcher_menu::mode_icon::settings}},
            "Back to Settings");
        if (selected < 0) return;
        if (selected == 0) {
            choose_library_folders(menu, false);
            continue;
        }

        emulator_settings settings = load_settings();
        if (selected == 1 || selected == 2) {
            const bool rom = selected == 1;
            const std::vector<std::string> folder = platform_file_selection(
                true, false, rom ? "Choose your MAME ROM folder" :
                                   "Choose your arcade CHD folder");
            if (folder.empty()) continue;
            if (rom) settings.rom_directory = folder.front();
            else settings.chd_directory = folder.front();
        } else if (selected == 3) {
            settings.chd_directory = rom_library_path();
        } else if (selected == 4) {
            const std::vector<std::string> folder = platform_file_selection(
                true, false,
                "Choose the mounted Roms/v3_0.285/media folder");
            if (folder.empty()) continue;
            settings.media_directory = folder.front();
        } else if (selected == 5) {
            import_installed_game_media(menu);
            continue;
        } else if (selected == 6) {
            settings.media_directory.clear();
        } else if (selected == 7) {
            settings.rom_directory.clear();
            settings.chd_directory.clear();
        }
        save_library_folders(menu, settings);
    }
}

void show_rom_library_manager(launcher_menu& menu) {
    for (;;) {
        const std::vector<launcher_menu::mode_card> items{
            {"Library + Media", "ROM, CHD and NAS artwork folders",
             launcher_menu::mode_icon::folder},
            {"Audit ROMs", "Verify every installed game",
             launcher_menu::mode_icon::audit},
            {"Required Sets", "See the supported MAME archives",
             launcher_menu::mode_icon::storage},
            {"Archive Help", "Merged and split-set guidance",
             launcher_menu::mode_icon::information},
        };
        const int selected = menu.select_modes(
            "Settings",
            "Choose a settings area.",
            items, "Back to Main Menu");
        if (selected < 0) return;
        if (selected == 0) {
            show_library_folder_settings(menu);
        } else if (selected == 1) {
            audit_installed_roms(menu);
        } else if (selected == 2) {
            menu.show_text("Required MAME sets", required_rom_sets_text());
        } else if (selected == 3) {
            menu.show_text(
                "MAME archive layouts",
                "Non-merged: each game ZIP contains every ROM it needs.\n\n"
                "Split: clone ZIPs omit shared parent ROMs. Keep the named "
                "parent ZIP beside the game; MANX searches both.\n\n"
                "Merged: a parent ZIP contains parent and clone subfolders. "
                "MANX searches entries by basename, so the merged "
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
    const auto& manifests = supported_rom_sets();
    std::vector<const rom_set_manifest*> score_games;
    for (const auto& manifest : manifests) {
        if (manifest.working && has_high_score_decoder(manifest.short_name))
            score_games.push_back(&manifest);
    }
    if (score_games.empty()) {
        menu.show_text(
            "High Scores",
            "No verified high-score decoders are available yet.\n\n"
            "Each game's binary memory layout and checksums must be\n"
            "independently verified before MANX will decode its\n"
            "score table.",
            "Back to Main Menu");
        return;
    }
    const int total = static_cast<int>(score_games.size());
    menu.show_scoreboard(
        "Back to Main Menu", "Hall of Fame", total,
        [&](int index) -> launcher_menu::scoreboard_page {
            const rom_set_manifest& game =
                *score_games[static_cast<std::size_t>(index)];
            launcher_menu::scoreboard_page page;
            page.title = game.display_name;
            const int slot = board_slot(game.board);
            page.subtitle = slot >= 0 ?
                std::string(arcade_boards()[
                    static_cast<std::size_t>(slot)].display_name) :
                std::string();
            high_score_table table;
            std::string message;
            if (!high_score_view(game.short_name, table, message)) {
                page.message = message;
                return page;
            }
            const std::size_t shown =
                std::min<std::size_t>(table.entries.size(), 10);
            for (std::size_t rank = 0; rank < shown; ++rank) {
                launcher_menu::scoreboard_row row;
                row.rank = static_cast<int>(rank) + 1;
                row.name = table.entries[rank].name;
                row.score = format_high_score(table.entries[rank].score);
                page.rows.push_back(std::move(row));
            }
            for (const auto& extra : table.extra_scores)
                page.extras.emplace_back(extra.first,
                                         format_high_score(extra.second));
            return page;
        },
        0);
}

} // namespace

std::vector<rom_choice> discover_rom_choices(const std::string& current_path) {
    return discover_library_roms(current_path);
}

namespace {

// How many displays this machine has. Asked at the moment the question is
// put, not cached: a monitor can be plugged in while the front end is open.
int display_count() {
    int count = 0;
    if (SDL_DisplayID* displays = SDL_GetDisplays(&count)) SDL_free(displays);
    return count;
}

struct launch_capabilities {
    const rom_set_manifest* manifest{};
    arcade_multiplayer_mode multiplayer{arcade_multiplayer_mode::none};
    bool system_link{};
    bool network_two_player{};
};

launch_capabilities probe_choice(const rom_choice& choice) {
    launch_capabilities caps;
    const std::optional<arcade_game_identity> identity =
        identify_arcade_game(choice.path);
    caps.manifest = identity ?
        find_supported_rom_set(identity->short_name) : nullptr;
    // System 246/256 collection games have no catalog entry - the board
    // boots any ".acgame" manifest - so their two-player support is
    // answered by the board's loader instead.
    const bool acgame_two_player =
        !caps.manifest &&
        choice.board == arcade_board_type::system246 &&
        system246_rom_loader::acgame_two_player(
            system246_rom_loader::acgame_short_name(choice.path));
    caps.multiplayer = caps.manifest ?
        caps.manifest->multiplayer :
        (acgame_two_player ? arcade_multiplayer_mode::simultaneous
                           : arcade_multiplayer_mode::none);
    caps.system_link =
        caps.manifest && supports_native_system_link(*caps.manifest);
    caps.network_two_player = caps.manifest ?
        supports_network_two_player(*caps.manifest) : acgame_two_player;
    return caps;
}

// The names the player sees for each play style, and the filter each one
// applies. Style 0 is every game; the rest match the two-player paths.
constexpr std::array<const char*, 5> play_style_names{
    "All Games", "2P Alternating", "2P Simultaneous", "2P Twin Screens",
    "2P Linked Cabinets"};

// The Wikipedia article whose lead image stands for a board - a PCB photo
// for most of them. Publishers use their Commons "<name> logo.svg" instead.
const char* board_wiki_article(arcade_board_type type) {
    switch (type) {
    case arcade_board_type::system22: return "Namco System 22";
    case arcade_board_type::system246: return "Namco System 246/256";
    case arcade_board_type::model1: return "Sega Model 1";
    case arcade_board_type::model2: return "Sega Model 2";
    case arcade_board_type::phoenix: return "Phoenix (1980 video game)";
    case arcade_board_type::galaxian: return "Galaxian";
    case arcade_board_type::system16b: return "Sega System 16";
    case arcade_board_type::capcom_gng: return "Ghosts 'n Goblins";
    case arcade_board_type::namco_galaga: return "Galaga";
    case arcade_board_type::namco_system1: return "Namco System 1";
    case arcade_board_type::midway: return "Midway Wolf Unit";
    }
}

// Decodes one harvested image. Uses the banner image type because it is the
// same shape the grid already draws from.
banner::banner_image load_local_art(const std::string& path) {
    banner::banner_image out;
    if (path.empty()) return out;
    std::ifstream input(path, std::ios::binary);
    if (!input) return out;
    const std::string bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(bytes.data()),
        static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (!pixels) return out;
    out.width = width;
    out.height = height;
    out.rgba.assign(pixels,
                    pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return out;
}

// Private/local MAME media pack. The configured folder may itself be the
// `media` directory on a mounted NAS, or MANX can infer a sibling `media`
// directory when ROMs live in `<version>/roms`. No URL is embedded and no
// media is copied into a release.
fs::path source_media_root() {
    const emulator_settings settings = load_settings();
    if (!settings.media_directory.empty())
        return fs::path(settings.media_directory);
    if (const char* environment = std::getenv("MANX_MEDIA_ROOT"))
        if (*environment) return fs::path(environment);
    if (!settings.rom_directory.empty()) {
        const fs::path beside_roms =
            fs::path(settings.rom_directory).parent_path() / "media";
        std::error_code error;
        if (fs::is_directory(beside_roms, error)) return beside_roms;
    }
    return {};
}

bool matches_play_style(const rom_choice& choice, int style) {
    if (style <= 0) return true;
    const launch_capabilities caps = probe_choice(choice);
    switch (style) {
    case 1: return caps.multiplayer == arcade_multiplayer_mode::alternating;
    case 2: return caps.multiplayer == arcade_multiplayer_mode::simultaneous;
    case 3: return caps.network_two_player;
    default: return caps.system_link;
    }
}

// Returns the chosen index into choices, -1 for back, or
// launcher_menu::interrupted when the interrupt callback fired.
int browse_library_grid(
    launcher_menu& menu, const std::vector<rom_choice>& choices,
    banner::library& banners,
    std::map<std::string, banner::banner_image>& banner_pixels,
    const std::string& menu_title,
    const std::function<bool(const rom_choice&)>& filter,
    int* play_style = nullptr,
    const std::function<bool()>& interrupt = {},
    const std::string& back_label = "Back") {

        // The whole (filtered) library, straight onto the cover grid. The
        // view - sort order or a board/publisher narrowing - is changed from
        // the grid itself with TAB, not chosen up front: seeing the games
        // comes first, organising them second.
        struct browse_entry {
            std::size_t index;
            int plays;
            long long last_played;
            long long added;
        };
        std::vector<browse_entry> entries;
        {
            const std::map<std::string, play_stat> stats = load_play_stats();
            for (std::size_t index = 0; index < choices.size(); ++index) {
                if (filter && !filter(choices[index])) continue;
                browse_entry entry{index, 0, 0, 0};
                const std::optional<arcade_game_identity> identity =
                    identify_arcade_game(choices[index].path);
                if (identity) {
                    const auto found = stats.find(identity->short_name);
                    if (found != stats.end()) {
                        entry.plays = found->second.count;
                        entry.last_played = found->second.last_played;
                    }
                }
                std::error_code error;
                const auto written = std::filesystem::last_write_time(
                    choices[index].path, error);
                if (!error)
                    entry.added =
                        written.time_since_epoch().count();
                entries.push_back(entry);
            }
        }
        // Decoded local artwork, keyed by index into choices.
    std::map<std::size_t, banner::banner_image> local_art;
    std::map<std::size_t, std::string> local_descriptions;
    // Cover-flow media, decoded once per game and kept for the life of the
    // browser. The launcher reads these pixels while drawing, so they must
    // outlive every media_for call - a per-call buffer would dangle.
    std::map<std::size_t, banner::banner_image> flow_box;
    std::map<std::size_t, banner::banner_image> flow_marquee;
    std::map<std::size_t, banner::banner_image> flow_snap;
    // Media names resolved once per game: media_names_for_choice hashes the
    // ROM archive to identify it, far too expensive for a per-frame call.
    std::map<std::size_t, std::vector<std::string>> flow_names;
    const auto names_for_entry =
        [&](std::size_t entry) -> const std::vector<std::string>& {
        auto found = flow_names.find(entry);
        if (found == flow_names.end())
            found = flow_names
                        .emplace(entry, media_names_for_choice(choices[entry]))
                        .first;
        return found->second;
    };
    const fs::path media_source = source_media_root();
    const fs::path media_local = manx_media::local_root();
    std::string media_category = load_settings().media_artwork_category;
    // 0 A-Z, 1 most played, 2 recently added, 3 last played, 4 single-screen,
    // 5 multi-screen. The browser opens A-Z, the order every established
    // front end (EmulationStation, Retrobat, LaunchBox) defaults to; the
    // other orders are one TAB away. 4 and 5 are the MANX cabinet-form
    // filters - the launcher's signature feature.
    int sort_mode = 0;
        // Paging: By Board / By Publisher turn the grid into a carousel - one
        // page per board or publisher, flipped with PgUp/PgDn or the shoulder
        // buttons, each page showing every game that belongs to it.
        int page_mode = 0;         // 0 none, 1 boards, 2 publishers
        int page_index = 0;
        int remembered = 0;
        for (;;) {
            // The pages that exist under the current style filter, rebuilt every
            // lap so a style change never leaves an empty page behind.
            std::vector<int> board_pages;
            std::vector<std::string> publisher_pages;
            if (page_mode == 1) {
                board_counts page_counts{};
                for (const browse_entry& entry : entries) {
                    const rom_choice& choice = choices[entry.index];
                    if (play_style && !matches_play_style(choice, *play_style))
                        continue;
                    const int slot = board_slot(choice.board);
                    if (slot >= 0)
                        ++page_counts[static_cast<std::size_t>(slot)];
                }
                for (std::size_t slot = 0; slot < page_counts.size(); ++slot)
                    if (page_counts[slot] > 0)
                        board_pages.push_back(static_cast<int>(slot));
                if (board_pages.empty()) page_mode = 0;
            } else if (page_mode == 2) {
                std::map<std::string, int> counted;
                for (const browse_entry& entry : entries) {
                    const rom_choice& choice = choices[entry.index];
                    if (play_style && !matches_play_style(choice, *play_style))
                        continue;
                    if (!choice.publisher.empty()) ++counted[choice.publisher];
                }
                for (const auto& publisher : counted)
                    publisher_pages.push_back(publisher.first);
                if (publisher_pages.empty()) page_mode = 0;
            }
            const int page_count = page_mode == 1 ?
                static_cast<int>(board_pages.size()) :
                page_mode == 2 ? static_cast<int>(publisher_pages.size()) : 0;
            if (page_count > 0)
                page_index = ((page_index % page_count) + page_count) %
                             page_count;
            const int board_narrow = page_mode == 1 ?
                board_pages[static_cast<std::size_t>(page_index)] : -1;
            const std::string publisher_narrow = page_mode == 2 ?
                publisher_pages[static_cast<std::size_t>(page_index)] :
                std::string();
            std::vector<browse_entry> visible;
            for (const browse_entry& entry : entries) {
                const rom_choice& choice = choices[entry.index];
                if (play_style &&
                    !matches_play_style(choice, *play_style))
                    continue;
                if (sort_mode == 4 || sort_mode == 5) {
                    // Cabinet-form filter. Identify the game by path and
                    // look up its catalog manifest; if we cannot find a
                    // manifest (an unpacked Xbox disc, say), the game is
                    // treated as single-screen by default.
                    const auto identity =
                        identify_arcade_game(choice.path);
                    if (!identity) continue;
                    const rom_set_manifest* manifest =
                        find_supported_rom_set(identity->short_name);
                    if (!manifest) continue;
                    const arcade_cabinet_form form =
                        classify_cabinet_form(*manifest);
                    const bool wants_multi =
                        form == arcade_cabinet_form::twin_cabinet ||
                        form == arcade_cabinet_form::linked_network;
                    if (sort_mode == 4 && wants_multi) continue;
                    if (sort_mode == 5 && !wants_multi) continue;
                }
                if (board_narrow >= 0 &&
                    board_slot(choice.board) != board_narrow)
                    continue;
                if (!publisher_narrow.empty() &&
                    choice.publisher != publisher_narrow)
                    continue;
                visible.push_back(entry);
            }
            std::stable_sort(
                visible.begin(), visible.end(),
                [&](const browse_entry& a, const browse_entry& b) {
                    if (sort_mode == 1) {
                        if (a.plays != b.plays) return a.plays > b.plays;
                        if (a.last_played != b.last_played)
                            return a.last_played > b.last_played;
                    } else if (sort_mode == 2) {
                        if (a.added != b.added) return a.added > b.added;
                    } else if (sort_mode == 3) {
                        if (a.last_played != b.last_played)
                            return a.last_played > b.last_played;
                    }
                    return choices[a.index].label < choices[b.index].label;
                });
            std::vector<std::size_t> game_indices;
            std::vector<std::string> labels;
            std::vector<std::string> descriptions;
            for (const browse_entry& entry : visible) {
                game_indices.push_back(entry.index);
                labels.push_back(choices[entry.index].label);
                auto cached = local_descriptions.find(entry.index);
                if (cached == local_descriptions.end()) {
                    cached = local_descriptions.emplace(
                        entry.index,
                        manx_media::description(
                            media_local,
                            media_names_for_choice(choices[entry.index])))
                                 .first;
                }
                descriptions.push_back(cached->second);
            }
            // Local artwork for whatever this page shows, decoded once and
            // kept for the life of the browser.
            for (std::size_t slot = 0; slot < game_indices.size(); ++slot) {
                const std::size_t entry = game_indices[slot];
                if (local_art.count(entry)) continue;
                const std::optional<arcade_game_identity> identity =
                    identify_arcade_game(choices[entry].path);
                if (!identity) {
                    local_art[entry] = {};
                    continue;
                }
                banner::banner_image image = load_local_art(
                    manx_media::artwork_path(
                        media_local, media_names_for_choice(choices[entry]),
                        media_category).string());
                if (!image.valid())
                    image = load_local_art(
                        manx_media::artwork_path(
                            media_source,
                            media_names_for_choice(choices[entry]),
                            media_category).string());
                local_art[entry] = std::move(image);
            }
            // No downloaded artwork: a game whose icon has not been
            // captured yet shows its name on the plain plate until it has
            // been played once.
            // The page's identity artwork: a board's PCB photo from its
            // Wikipedia article, a publisher's logo from Wikimedia Commons.
            std::string page_title = menu_title;
            std::string board_summary;
            std::string board_info;
            launcher_menu::cover banner_cover;
            const launcher_menu::cover* banner_ptr = nullptr;
            std::string banner_key;
            if (page_mode != 0) {
                if (page_mode == 1) {
                    const auto& descriptor =
                        arcade_boards()[static_cast<std::size_t>(
                            board_narrow)];
                    page_title = descriptor.display_name;
                    banner_key = std::string("board_") + descriptor.id;
                    banners.request(banner_key,
                                    banner::library::kind::article,
                                    board_wiki_article(descriptor.type));
                    // The board's real hardware description, harvested from
                    // system16.com: the CPUs, sound hardware and features
                    // that make this board what it is.
                    // Copied, not referenced: the argument is a temporary
                    // std::string built from the descriptor's char pointer,
                    // and a reference through it is what GCC rightly warns
                    // about.
                    const system16::spec_list specs =
                        system16::board_specs(descriptor.id);
                    for (const auto& spec : specs) {
                        if (spec.first.rfind('-', 0) == 0) continue;
                        if (!board_info.empty()) board_info += "\n";
                        board_info += spec.first + ":  " + spec.second;
                    }
                    // The header stays one short line - the CPU, which is
                    // what identifies a board at a glance. Everything else
                    // is behind the "i".
                    // "Main CPU" first, then a bare "CPU", then anything
                    // CPU-ish: a board is identified by its main processor,
                    // not by whichever coprocessor happens to be listed
                    // first.
                    for (const char* wanted : {"Main CPU", "CPU", "CPU"}) {
                        for (const auto& spec : specs) {
                            const bool match =
                                std::string(wanted) == "Main CPU"
                                    ? spec.first == "Main CPU"
                                    : (spec.first == "CPU" ||
                                       spec.first.find("CPU") !=
                                           std::string::npos);
                            if (!match) continue;
                            board_summary = spec.first + ": " + spec.second;
                            break;
                        }
                        if (!board_summary.empty()) break;
                    }
                } else {
                    page_title = publisher_narrow;
                    banner_key = "pub_" + publisher_narrow;
                    banners.request(banner_key,
                                    banner::library::kind::commons_logo,
                                    publisher_narrow);
                }
                const auto found = banner_pixels.find(banner_key);
                if (found != banner_pixels.end() && found->second.valid()) {
                    banner_cover.pixels = found->second.rgba.data();
                    banner_cover.width = found->second.width;
                    banner_cover.height = found->second.height;
                }
                banner_ptr = &banner_cover;
            }
            const int selected_game = menu.select_grid(
                page_title,
                labels.empty() ?
                    "No games match this view. TAB changes the view; open "
                    "Settings from the System Menu to choose ROM folders." :
                    "Multiscreen Arcade Nexus  //  Multi-screen. "
                    "Multi-player. Networked arcade cohesion.",
                labels, back_label, std::min(remembered,
                    std::max(0, static_cast<int>(labels.size()) - 1)),
                [&](int index) {
                    launcher_menu::cover art;
                    if (index < 0 ||
                        index >= static_cast<int>(labels.size()))
                        return art;
                    // Only the explicitly selected NAS media category is
                    // shown. Missing media stays a named placeholder.
                    const std::size_t entry =
                        game_indices[static_cast<std::size_t>(index)];
                    const auto local = local_art.find(entry);
                    if (local != local_art.end() && local->second.valid()) {
                        art.pixels = local->second.rgba.data();
                        art.width = local->second.width;
                        art.height = local->second.height;
                        return art;
                    }
                    return art;
                },
                [&] {
                    bool arrived = false;
                    for (banner::ready_banner& ready : banners.take_ready()) {
                        if (!ready.image.valid()) continue;
                        banner_pixels[ready.key] = std::move(ready.image);
                        arrived = true;
                    }
                    // A banner landing while its page is on screen appears
                    // immediately; without this it only showed on the NEXT
                    // visit to the page, which read as "no artwork at all".
                    if (!banner_key.empty()) {
                        const auto found = banner_pixels.find(banner_key);
                        if (found != banner_pixels.end() &&
                            found->second.valid() && !banner_cover.pixels) {
                            banner_cover.pixels = found->second.rgba.data();
                            banner_cover.width = found->second.width;
                            banner_cover.height = found->second.height;
                            arrived = true;
                        }
                    }
                    return arrived;
                },
                interrupt, page_mode != 0, banner_ptr,
                !board_info.empty(),
                /*list_view=*/false,
                /*marquee_view=*/media_category == "marquee",
                /*coverflow_view=*/media_category == "coverflow",
                /*video_for=*/[&](int index) -> std::string {
                    if (index < 0 ||
                        index >= static_cast<int>(game_indices.size()))
                        return {};
                    const std::size_t entry =
                        game_indices[static_cast<std::size_t>(index)];
                    const auto& names = names_for_entry(entry);
                    fs::path found = manx_media::video_path(media_local, names);
                    if (found.empty())
                        found = manx_media::video_path(media_source, names);
                    return found.string();
                },
                /*media_for=*/[&](int index) -> launcher_menu::game_media {
                    launcher_menu::game_media media;
                    if (index < 0 ||
                        index >= static_cast<int>(game_indices.size()))
                        return media;

                    const std::size_t entry =
                        game_indices[static_cast<std::size_t>(index)];
                    const auto& names = names_for_entry(entry);

                    // One decoded image per media slot, found through the
                    // media library's category search - the same lookup the
                    // grid uses, so the pack's real folder names (box2d,
                    // images, titles...) all resolve. The first category
                    // that has an image for this game wins.
                    auto flow_art =
                        [&](std::map<std::size_t, banner::banner_image>& cache,
                            std::initializer_list<const char*> categories)
                            -> const banner::banner_image& {
                        auto found = cache.find(entry);
                        if (found == cache.end()) {
                            banner::banner_image image;
                            for (const char* category : categories) {
                                image = load_local_art(
                                    manx_media::artwork_path(
                                        media_local, names, category)
                                        .string());
                                if (!image.valid())
                                    image = load_local_art(
                                        manx_media::artwork_path(
                                            media_source, names, category)
                                            .string());
                                if (image.valid()) break;
                            }
                            found = cache.emplace(entry, std::move(image))
                                        .first;
                        }
                        return found->second;
                    };

                    const banner::banner_image& box = flow_art(
                        flow_box,
                        {"box2d", "box3d", "covers", "cover", "flyers"});
                    if (box.valid()) {
                        media.box_pixels = box.rgba.data();
                        media.box_w = box.width;
                        media.box_h = box.height;
                    }
                    const banner::banner_image& marquee = flow_art(
                        flow_marquee, {"marquee", "marquees"});
                    if (marquee.valid()) {
                        media.marquee_pixels = marquee.rgba.data();
                        media.marquee_w = marquee.width;
                        media.marquee_h = marquee.height;
                    }
                    const banner::banner_image& snap = flow_art(
                        flow_snap,
                        {"images", "titles", "title", "snaps", "snap",
                         "thumbnails", "fanarts"});
                    if (snap.valid()) {
                        media.snap_pixels = snap.rgba.data();
                        media.snap_w = snap.width;
                        media.snap_h = snap.height;
                    }

                    const rom_choice& choice = choices[entry];
                    media.publisher = choice.publisher;
                    const int slot = board_slot(choice.board);
                    if (slot >= 0)
                        media.board_name =
                            arcade_boards()[static_cast<std::size_t>(slot)]
                                .display_name;
                    return media;
                },
                &descriptions);
            if (selected_game == launcher_menu::exit_requested)
                return launcher_menu::exit_requested;
            if (selected_game == launcher_menu::interrupted)
                return launcher_menu::interrupted;
            if (selected_game == launcher_menu::info_request) {
                menu.show_text(page_title, board_info, "Back to Games");
                continue;
            }
            if (selected_game == launcher_menu::page_back ||
                selected_game == launcher_menu::page_forward) {
                page_index +=
                    selected_game == launcher_menu::page_back ? -1 : 1;
                remembered = 0;
                continue;
            }
            if (selected_game == launcher_menu::view_change) {
                // One page for the whole view: what the cards look like,
                // what order they come in, and whether the library is paged
                // by board or publisher - the sort/group menu every
                // established front end hangs off its select button.
                static constexpr std::array<const char*, 4> categories{{
                    "box2d", "box3d", "marquee", "coverflow",
                }};
                constexpr int first_sort = 4;   // cards 4..7 pick the order
                constexpr int first_group = 8;  // cards 8..9 pick the paging
                constexpr int arrange = 10;     // card 10 opens the designer
                int current = 0;
                for (std::size_t index = 0; index < categories.size(); ++index)
                    if (media_category == categories[index])
                        current = static_cast<int>(index);
                const int picked = menu.select_modes(
                    "View", "",
                    {{"2D", "Flat cover",
                      launcher_menu::mode_icon::cover_front},
                     {"3D", "Box render",
                      launcher_menu::mode_icon::cover_3d},
                     {"Marquee", "Cabinet header",
                      launcher_menu::mode_icon::marquee_art},
                     {"Cover Flow", "Full-screen 3D carousel",
                      launcher_menu::mode_icon::fan_art},
                     {"A-Z", "Alphabetical order",
                      launcher_menu::mode_icon::audit},
                     {"Most Played", "Favourites first",
                      launcher_menu::mode_icon::scores},
                     {"Recently Added", "Newest sets first",
                      launcher_menu::mode_icon::refresh},
                     {"Last Played", "Pick up where you left off",
                      launcher_menu::mode_icon::solo},
                     {"By Board", "One page per board",
                      launcher_menu::mode_icon::storage},
                     {"By Publisher", "One page per publisher",
                      launcher_menu::mode_icon::folder},
                     {"Arrange Page", "Move and resize the cover-flow art",
                      launcher_menu::mode_icon::cover_3d}},
                    "Back to Games", current);
                if (picked == launcher_menu::exit_requested)
                    return launcher_menu::exit_requested;
                if (picked >= 0 &&
                    picked < static_cast<int>(categories.size())) {
                    media_category =
                        categories[static_cast<std::size_t>(picked)];
                    emulator_settings settings = load_settings();
                    settings.media_artwork_category = media_category;
                    save_settings(settings);
                    local_art.clear();
                } else if (picked >= first_sort && picked < first_group) {
                    sort_mode = picked - first_sort;
                    page_mode = 0;
                } else if (picked == first_group || picked == first_group + 1) {
                    page_mode = picked - first_group + 1;
                    page_index = 0;
                } else if (picked == arrange) {
                    // Arranging is only meaningful over the cover-flow page,
                    // so asking for it from any other view switches to it
                    // rather than doing nothing visible.
                    if (media_category != "coverflow") {
                        media_category = "coverflow";
                        emulator_settings settings = load_settings();
                        settings.media_artwork_category = media_category;
                        save_settings(settings);
                        local_art.clear();
                    }
                    menu.arrange_coverflow_next();
                }
                remembered = 0;
                continue;
            }
            if (selected_game < 0 ||
                selected_game >= static_cast<int>(game_indices.size()))
                return -1;
            remembered = selected_game;
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
            return static_cast<int>(
                game_indices[static_cast<std::size_t>(selected_game)]);
        }
}

} // namespace

int linked_cabinet_maximum(const std::string& short_name) {
    // Daytona's ring hardware takes 8 cabinets; per-car operator records
    // for all 8 are in place and 3- and 4-cabinet rings race on host (the
    // ring ids number DOWNWARDS from the master - the fact that unblocked
    // everything past a twin). Manx TT's bike numbers likewise run No.1
    // to No.8 (Master, Relay middles, Slave tail, records synthesized
    // with the Sega record CRC). Sega Rally's own link menu stops at
    // four cars.
    if (short_name == "daytona") return 8;
    if (short_name == "manxttc") return 8;
    if (short_name == "srallyc") return 4;
    if (short_name == "motoraid") return 4;
    return 2;
}

std::string show_in_game_game_browser(const std::vector<rom_choice>& choices) {
    // A software launcher avoids stealing the running cabinet's GL context,
    // exactly as the in-game controls mapper does.
    launcher_menu menu(true);
    banner::library banners;
    std::map<std::string, banner::banner_image> banner_pixels;
    const int picked = browse_library_grid(
        menu, choices, banners, banner_pixels,
        "Switch Game", {});
    return picked >= 0 ? choices[static_cast<std::size_t>(picked)].path
                       : std::string();
}

rom_selection_result show_rom_selector(const std::string& current_path,
                                       multiplayer_lobby* lobby) {
    launcher_menu menu;
    // The boot screen goes up before anything slow happens, so switching the
    // cabinet on shows the cabinet rather than an empty desktop.
    menu.show_splash("Starting up", 0.05f);

    // Local helper: render the per-game loading screen between launcher
    // pick and emulator start. The screen shows the game title, cabinet
    // form, and a fading-in spinner. Driven by a callback that updates the
    // progress fraction; the callback is called repeatedly while the
    // loader runs.
    //
    // The screen is intentionally short (3-4 spinner ticks) so it does
    // not visibly block the emulator from appearing; main() handles the
    // long asset warm in the background.
    auto run_loading_screen = [](launcher_menu& menu,
                                   const rom_choice& choice) {
        // Build the cabinet-form subtitle from the catalog manifest so the
        // player can see what they just chose.
        std::string subtitle = "Cabinet: ";
        if (const auto identity = identify_arcade_game(choice.path)) {
            if (const rom_set_manifest* manifest =
                    find_supported_rom_set(identity->short_name)) {
                subtitle += cabinet_form_label(classify_cabinet_form(*manifest));
            } else {
                subtitle += "Single-screen cabinet";
            }
        } else {
            subtitle += "Single-screen cabinet";
        }
        // Brief, friendly messages — the spinner and progress bar carry
        // the visual load.
        const std::vector<std::string> steps = {
            "Reserving cabinet hardware...",
            "Loading cabinet artwork...",
            "Warming emulated components...",
        };
        std::size_t step_index = 0;
        float progress = 0.0f;
        menu.show_loading_screen(
            choice.label, subtitle,
            [&steps, &step_index, &progress](float& out) -> bool {
                if (step_index >= steps.size()) {
                    out = 1.0f;
                    return false;  // finished -> fade out
                }
                out = (step_index + 1.0f) / steps.size();
                progress = out;
                ++step_index;
                return true;
            });
    };
    // Board and publisher banners are collected in the background and kept
    // for the life of the selector so moving between lists does not refetch.
    banner::library banners;
    std::map<std::string, banner::banner_image> banner_pixels;
    if (!load_settings().library_setup_complete &&
        !choose_library_folders(menu, true)) {
        rom_selection_result exit_result;
        exit_result.action = rom_selection_action::exit_requested;
        return exit_result;
    }
    menu.show_splash("Reading the game library", 0.35f);
    std::vector<rom_choice> choices = discover_rom_choices(current_path);
    choices.erase(
        std::remove_if(choices.begin(), choices.end(),
            [](const rom_choice& choice) {
                return hidden_from_launcher(choice.board);
            }),
        choices.end());
    menu.show_splash("Gathering cabinet artwork", 0.8f);
    const auto linked_result = [&](std::string_view short_name, int node) {
        for (const rom_choice& choice : choices) {
            const auto identity = identify_arcade_game(choice.path);
            if (identity && short_name == identity->short_name)
                return rom_selection_result{
                    rom_selection_action::selected, choice.path,
                    cabinet_launch_mode::linked_network, node,
                    false, false, true, {}};
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
                ", but that ROM is not installed in this MANX "
                "library.", "Back to Main Menu");
        return {};
    };

    // The board/publisher browser with the cover grid, shared by every menu
    // that ends in "pick a game". filter narrows the library - the same-
    // machine menu offers only games with a second-player path - and a game
    // that fails its manifest check reports itself and keeps browsing.
    bool exit_from_nested_browser = false;
    const auto browse_for_game =
        [&](const std::string& menu_title,
            const std::function<bool(const rom_choice&)>& filter)
        -> std::optional<std::size_t> {
        const int picked = browse_library_grid(
            menu, choices, banners, banner_pixels,
            menu_title, filter);
        if (picked == launcher_menu::exit_requested)
            exit_from_nested_browser = true;
        if (picked < 0) return std::nullopt;
        return static_cast<std::size_t>(picked);
    };

    struct platform_entry {
        arcade_board_type board;
        std::string name;
        std::string logo;
        int games{};
        launcher_menu::mode_icon icon{launcher_menu::mode_icon::storage};
    };
    std::vector<platform_entry> platforms;
    const auto platform_name = [](arcade_board_type board) {
        const int slot = board_slot(board);
        if (slot >= 0)
            return std::string(arcade_boards()[
                static_cast<std::size_t>(slot)].display_name);
        if (board == arcade_board_type::game_plugin)
            return std::string("Native Arcade");
        return std::string("Other Arcade");
    };
    const auto platform_icon = [](arcade_board_type board) {
        switch (board) {
        case arcade_board_type::system22:
        case arcade_board_type::system246:
        case arcade_board_type::model1:
        case arcade_board_type::model2:
            return launcher_menu::mode_icon::linked_cabinets;
        case arcade_board_type::system16b:
        case arcade_board_type::capcom_gng:
            return launcher_menu::mode_icon::local_players;
        case arcade_board_type::game_plugin:
            return launcher_menu::mode_icon::display_wall;
        default:
            return launcher_menu::mode_icon::solo;
        }
    };
    const auto platform_logo = [](arcade_board_type board) {
        switch (board) {
        case arcade_board_type::system22:
        case arcade_board_type::namco_galaga:
        case arcade_board_type::namco_system1:
            return std::string("Namco");
        case arcade_board_type::system246:
            return std::string("Sony");
        case arcade_board_type::model1:
        case arcade_board_type::model2:
        case arcade_board_type::system16b:
            return std::string("Sega");
        case arcade_board_type::capcom_gng:
            return std::string("Capcom");
        case arcade_board_type::galaxian:
            return std::string("Namco");
        case arcade_board_type::phoenix:
            return std::string("Amstar");
        case arcade_board_type::midway:
            return std::string("Midway");
        default:
            return std::string();
        }
    };
    for (const rom_choice& choice : choices) {
        const auto found = std::find_if(
            platforms.begin(), platforms.end(),
            [&](const platform_entry& entry) {
                return entry.board == choice.board;
            });
        if (found != platforms.end()) {
            ++found->games;
        } else {
            platforms.push_back({choice.board, platform_name(choice.board),
                                 platform_logo(choice.board), 1,
                                 platform_icon(choice.board)});
        }
    }
    std::stable_sort(platforms.begin(), platforms.end(),
                     [](const platform_entry& left,
                        const platform_entry& right) {
                         return left.name < right.name;
                     });

    // Platform is the first selection. Once inside one, Back returns to the
    // platform tiles; Back there opens the system menu. This keeps a marquee
    // carousel short and coherent instead of mixing every board generation.
    int play_style = 0;
    std::optional<arcade_board_type> selected_platform;
    int platform_cursor = 0;
    for (;;) {
        if (lobby) lobby->set_installed_games(choices);
        const bool lobby_connected_at_draw = lobby && lobby->connected();
        // The grid re-enters this loop whenever the connection changes, so
        // setting it here is enough to keep it honest.
        menu.set_status(
            lobby_connected_at_draw ?
                "2 machines connected - network play ready" :
                "Waiting for another machine - no network play yet",
            lobby_connected_at_draw);
        const std::function<bool()> interrupt = lobby ?
            std::function<bool()>([lobby, lobby_connected_at_draw] {
                return lobby->launch_pending() ||
                       lobby->connected() != lobby_connected_at_draw;
            }) :
            std::function<bool()>{};
        bool system_menu_requested = false;
        if (!selected_platform) {
            std::vector<launcher_menu::mode_card> platform_cards;
            std::vector<std::string> platform_logo_keys;
            platform_cards.reserve(platforms.size());
            platform_logo_keys.reserve(platforms.size());
            for (const platform_entry& platform : platforms) {
                platform_cards.push_back({
                    platform.name,
                    std::to_string(platform.games) +
                    (platform.games == 1 ? " game" : " games"),
                    platform.icon, platform.games});
                platform_cards.back().artwork_fallback = platform.logo;
                const std::string key = platform.logo.empty() ? std::string{} :
                    "brand_" + platform.logo;
                platform_logo_keys.push_back(key);
                if (!key.empty())
                    banners.request(key, banner::library::kind::commons_logo,
                                    platform.logo);
            }
            const auto collect_platform_logos = [&] {
                bool arrived = false;
                for (banner::ready_banner& ready : banners.take_ready()) {
                    if (!ready.image.valid()) continue;
                    banner_pixels[ready.key] = std::move(ready.image);
                    arrived = true;
                }
                for (std::size_t index = 0;
                     index < platform_logo_keys.size(); ++index) {
                    const auto found = banner_pixels.find(
                        platform_logo_keys[index]);
                    if (found == banner_pixels.end() ||
                        !found->second.valid())
                        continue;
                    platform_cards[index].artwork =
                        found->second.rgba.data();
                    platform_cards[index].artwork_width =
                        found->second.width;
                    platform_cards[index].artwork_height =
                        found->second.height;
                }
                return arrived;
            };
            const int platform_choice = menu.select_modes(
                "Choose Platform",
                "Multiscreen Arcade Nexus",
                platform_cards, "System Menu", platform_cursor, interrupt,
                collect_platform_logos);
            if (platform_choice == launcher_menu::exit_requested) {
                rom_selection_result exit_result;
                exit_result.action = rom_selection_action::exit_requested;
                return exit_result;
            }
            if (platform_choice == launcher_menu::interrupted) {
                rom_selection_result remote = take_remote_launch();
                if (remote.action == rom_selection_action::selected)
                    return remote;
                continue;
            }
            if (platform_choice < 0) {
                system_menu_requested = true;
            } else {
                platform_cursor = platform_choice;
                selected_platform = platforms[
                    static_cast<std::size_t>(platform_choice)].board;
            }
        }

        int picked = -1;
        if (!system_menu_requested) {
            const std::string title = platform_name(*selected_platform);
            picked = browse_library_grid(
                menu, choices, banners, banner_pixels, title,
                [&](const rom_choice& choice) {
                    return choice.board == *selected_platform;
                }, &play_style, interrupt, "Platforms");
        }
        if (picked == launcher_menu::exit_requested) {
            rom_selection_result exit_result;
            exit_result.action = rom_selection_action::exit_requested;
            return exit_result;
        }
        if (picked == launcher_menu::interrupted) {
            rom_selection_result remote = take_remote_launch();
            if (remote.action == rom_selection_action::selected) return remote;
            continue;
        }
        if (!system_menu_requested && picked < 0) {
            selected_platform.reset();
            play_style = 0;
            continue;
        }
        if (picked >= 0) {
            const rom_choice& choice =
                choices[static_cast<std::size_t>(picked)];
            if (play_style == 0) {
                // Always ask how to launch. Multi-instance is useful even
                // when an older catalogue entry has no multiplayer metadata,
                // and must never disappear merely because that metadata is
                // incomplete.
                const launch_capabilities caps = probe_choice(choice);
                int wanted = -1;
                while (wanted < 0) {
                    const auto identity = identify_arcade_game(choice.path);
                    const bool peer_connected = lobby && lobby->connected();
                    const bool peer_ready = peer_connected && identity &&
                        lobby->peer_has_game(identity->short_name);
                    std::vector<launcher_menu::mode_card> how;
                    std::vector<int> how_style;
                    how.push_back({"Single Player", "One player, one cabinet",
                                   launcher_menu::mode_icon::solo});
                    how_style.push_back(0);
                    how.push_back({
                        "Another Instance",
                        caps.system_link || caps.network_two_player ?
                            "Start Player 2 here and connect immediately" :
                            "Start a second connected cabinet here",
                        launcher_menu::mode_icon::linked_cabinets, 2});
                    how_style.push_back(6);
                    if (caps.multiplayer ==
                            arcade_multiplayer_mode::alternating) {
                        how.push_back({"Local Multiplayer",
                                       "Take turns on this cabinet",
                                       launcher_menu::mode_icon::local_players});
                        how_style.push_back(1);
                    } else if (caps.multiplayer ==
                                   arcade_multiplayer_mode::simultaneous) {
                        how.push_back({"Local Multiplayer",
                                       "Play together on this cabinet",
                                       launcher_menu::mode_icon::local_players});
                        how_style.push_back(2);
                    }
                    if (caps.network_two_player) {
                        how.push_back({"Split Screen",
                                       "A separate view for each player",
                                       launcher_menu::mode_icon::split_screen});
                        how_style.push_back(3);
                    }
                    if (caps.system_link) {
                        const int maximum = identity ?
                            linked_cabinet_maximum(identity->short_name) : 2;
                        if (maximum > 2) {
                            how.push_back({
                                "Linked Cabinets",
                                "Launch 2-" + std::to_string(maximum) +
                                    " connected instances here",
                                launcher_menu::mode_icon::linked_cabinets,
                                maximum});
                            how_style.push_back(4);
                        }
                    }
                    if (lobby) {
                        how.push_back({"Network Play",
                                       "Play across two computers",
                                       launcher_menu::mode_icon::network, 0,
                                       !peer_ready,
                                       peer_ready ? "PEER READY" :
                                       peer_connected ? "GAME NOT ON PEER" :
                                                        "SEARCHING"});
                        how_style.push_back(5);
                    }
                    const int how_chosen = menu.select_modes(
                        choice.label,
                        "Choose how this game should launch.", how,
                        "Back to Games", 0,
                        [lobby, peer_connected] {
                            return lobby &&
                                lobby->connected() != peer_connected;
                        });
                    if (how_chosen == launcher_menu::exit_requested) {
                        rom_selection_result exit_result;
                        exit_result.action =
                            rom_selection_action::exit_requested;
                        return exit_result;
                    }
                    if (how_chosen == launcher_menu::interrupted)
                        continue;   // a machine arrived, or left
                    if (how_chosen < 0 ||
                        how_chosen >= static_cast<int>(how_style.size()))
                        break;
                    wanted = how_style[static_cast<std::size_t>(how_chosen)];
                }
                if (wanted < 0) continue;   // backed out
                if (wanted == 5) {
                    // Across two computers: the partner launches itself.
                    const auto identity = identify_arcade_game(choice.path);
                    if (identity) lobby->launch_game(identity->short_name);
                    run_loading_screen(menu, choice);

                    return {rom_selection_action::selected, choice.path, cabinet_launch_mode::linked_network, 1, false,
                            false, true, {}};
                }
                if (wanted == 6) {
                    // A second process on this computer. Main allocates a
                    // private loopback port pair before spawning it, so this
                    // is already connected when both boards finish booting.
                    run_loading_screen(menu, choice);
                    return {rom_selection_action::selected, choice.path,
                            cabinet_launch_mode::linked_pair, 0, false, false,
                            true, {}};
                }
                if (wanted == 0 || wanted == 1 || wanted == 2) {
                    run_loading_screen(menu, choice);

                    return {rom_selection_action::selected, choice.path, cabinet_launch_mode::single, 0, false, false,
                            wanted != 0, {}};
                }
                play_style = wanted;   // twin screens or linked cabinets
            }
            if (play_style == 1 || play_style == 2) {
                // One cabinet, both players sharing it exactly as the
                // original game was played.
                run_loading_screen(menu, choice);

                return {rom_selection_action::selected, choice.path, cabinet_launch_mode::single, 0, false, false, true,
                        {}};
            }
            if (play_style == 3) {
                // A machine with one display cannot put a player on a
                // second one, but the option still shows - greyed out, so it
                // reads as "this machine cannot", not "this cannot be done".
                const bool second_display_missing =
                    display_count() < 2;
                const int mode = menu.select_modes(
                    "Start " + choice.label + " - Twin Screens",
                    "Choose where the two player views should appear.",
                    {{"Split This Screen", "Two views, side by side",
                      launcher_menu::mode_icon::split_screen},
                     {"Two Monitors", "One fullscreen view per display",
                      launcher_menu::mode_icon::display_wall, 2,
                      second_display_missing,
                      second_display_missing ? "CONNECT A SECOND DISPLAY" :
                                               "2 DISPLAYS READY"}},
                    "Back to Modes");
                if (mode == launcher_menu::exit_requested) {
                    rom_selection_result exit_result;
                    exit_result.action = rom_selection_action::exit_requested;
                    return exit_result;
                }
                if (mode < 0) continue;
                run_loading_screen(menu, choice);

                return {rom_selection_action::selected, choice.path, cabinet_launch_mode::independent_pair, 0, mode == 1,
                        false, true, {}};
            }
            // Games whose comm ring runs more than a twin offer the count
            // first; everything else launches the classic pair directly.
            int cabinet_count = 2;
            const auto link_identity = identify_arcade_game(choice.path);
            const int cabinet_maximum = link_identity ?
                linked_cabinet_maximum(link_identity->short_name) : 2;
            if (cabinet_maximum > 2) {
                std::vector<launcher_menu::mode_card> counts;
                for (int count = 2; count <= cabinet_maximum; ++count) {
                    counts.push_back({std::to_string(count) + " Cabinets",
                                      count == 2 ? "Classic linked twin" :
                                                   "One instance per cabinet",
                                      launcher_menu::mode_icon::cabinet_count,
                                      count});
                }
                const int picked = menu.select_modes(
                    "Start " + choice.label + " - Cabinets In The Link",
                    "Choose how many linked game instances to start on this "
                    "machine.", counts, "Back to Modes", 0);
                if (picked == launcher_menu::exit_requested) {
                    rom_selection_result exit_result;
                    exit_result.action = rom_selection_action::exit_requested;
                    return exit_result;
                }
                if (picked < 0) continue;
                cabinet_count = 2 + picked;
            }
            const int connected_displays = display_count();
            const bool second_display_missing =
                connected_displays < cabinet_count;
            const int mode = menu.select_modes(
                "Start " + choice.label + " - Linked Cabinets",
                "Choose how the linked cabinets should use your displays.",
                {{"Side By Side", "Tile every cabinet on this display",
                  launcher_menu::mode_icon::split_screen, cabinet_count},
                 {"Separate Monitors", "One cabinet per connected display",
                  launcher_menu::mode_icon::display_wall, cabinet_count,
                  second_display_missing,
                  second_display_missing ?
                      std::to_string(connected_displays) + " OF " +
                          std::to_string(cabinet_count) + " DISPLAYS" :
                                           "DISPLAYS READY"}},
                "Back to Games", 0);
            if (mode == launcher_menu::exit_requested) {
                rom_selection_result exit_result;
                exit_result.action = rom_selection_action::exit_requested;
                return exit_result;
            }
            if (mode < 0) continue;
            rom_selection_result linked{rom_selection_action::selected,
                                        choice.path,
                                        cabinet_launch_mode::linked_pair, 0,
                                        mode == 1, mode == 0, true, {}};
            linked.cabinet_count = cabinet_count;
            return linked;
        }

        // Back from platform selection: the system menu. Everything that is
        // not picking a game lives here, including leaving the program.
        const int selected_page = menu.select_modes(
            "MANX",
            lobby && lobby->connected() ?
                "Network player connected - Network Play launches a shared "
                "game across both computers." :
                "System pages. Network Play searches for a second "
                "MANX on the local network automatically.",
            {{"Network Play", "Play across two computers",
              launcher_menu::mode_icon::network, 0, false,
              lobby && lobby->connected() ? "CONNECTED" : "SEARCHING"},
             {"Arcade Wall", "Several games side by side",
              launcher_menu::mode_icon::display_wall},
             {"Settings", "Libraries, media and system help",
              launcher_menu::mode_icon::settings},
             {"Controls", "Controllers and keyboard mapping",
              launcher_menu::mode_icon::controls},
             {"Saved Data", "EEPROM and NVRAM manager",
              launcher_menu::mode_icon::storage},
             {"High Scores", "Records from every cabinet",
              launcher_menu::mode_icon::scores},
             {"Exit MANX", "Close the arcade launcher",
              launcher_menu::mode_icon::exit}},
            "Back to Platforms");
        if (selected_page == launcher_menu::exit_requested ||
            selected_page == 6) {
            rom_selection_result exit_result;
            exit_result.action = rom_selection_action::exit_requested;
            return exit_result;
        }
        if (selected_page < 0) continue;
        if (selected_page == 0) {
            if (!lobby) {
                menu.show_text(
                    "Multiplayer - Network",
                    "Automatic multiplayer discovery is available when "
                    "MANX is opened without a ROM on the command "
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
                        "Multiplayer - Network - Finding Player 2",
                        "Open MANX on the second screen or computer. "
                        "The apps detect each other automatically; no cabinet "
                        "role or IP address is required.",
                        {"Searching for another MANX..."},
                        "Back to Main Menu", 0, [lobby] {
                            return lobby->connected() ||
                                   lobby->launch_pending();
                        });
                    if (waiting == launcher_menu::exit_requested) {
                        rom_selection_result exit_result;
                        exit_result.action =
                            rom_selection_action::exit_requested;
                        return exit_result;
                    }
                    if (waiting == -1) break;
                    continue;
                }
                if (lobby->node() == 2) {
                    const int waiting = menu.select_interruptible(
                        "Multiplayer - Network - Player 2 Connected",
                        "Player 1 is choosing the game. This screen will "
                        "launch automatically.",
                        {"Waiting for Player 1..."},
                        "Back to Main Menu", 0,
                        [lobby] {
                            return lobby->launch_pending() ||
                                   !lobby->connected();
                        });
                    if (waiting == launcher_menu::exit_requested) {
                        rom_selection_result exit_result;
                        exit_result.action =
                            rom_selection_action::exit_requested;
                        return exit_result;
                    }
                    if (waiting == -1) break;
                    continue;
                }

                // Both machines are here, so the styles are offered
                // directly rather than behind a generic "network play"
                // entry: each one lists only the games that support it and
                // launches both cabinets itself.
                const int multiplayer_kind = menu.select_modes(
                    "Two Machines Connected - Choose How To Play",
                    "Choose a network play style. The second machine will "
                    "launch automatically.",
                    {{"Take Turns", "A full screen on each computer",
                      launcher_menu::mode_icon::local_players},
                     {"Play Together", "Both players at the same time",
                      launcher_menu::mode_icon::network},
                     {"Arcade System Link", "The original cabinet network",
                      launcher_menu::mode_icon::linked_cabinets}},
                    "Back to Main Menu", 0,
                    [lobby] { return !lobby->connected(); });
                if (multiplayer_kind == launcher_menu::exit_requested) {
                    rom_selection_result exit_result;
                    exit_result.action =
                        rom_selection_action::exit_requested;
                    return exit_result;
                }
                if (multiplayer_kind == launcher_menu::interrupted) continue;
                if (multiplayer_kind < 0) break;

                const bool system_link = multiplayer_kind == 2;
                const arcade_multiplayer_mode wanted_mode =
                    multiplayer_kind == 0 ?
                        arcade_multiplayer_mode::alternating :
                        arcade_multiplayer_mode::simultaneous;
                std::vector<std::size_t> linked_indices;
                std::vector<std::string> linked_labels;
                for (std::size_t index = 0; index < choices.size(); ++index) {
                    const auto identity =
                        identify_arcade_game(choices[index].path);
                    const rom_set_manifest* manifest = identity ?
                        find_supported_rom_set(identity->short_name) : nullptr;
                    if (!identity || !manifest ||
                        (system_link ?
                            !supports_native_system_link(*manifest) :
                            (!supports_network_two_player(*manifest) ||
                             manifest->multiplayer != wanted_mode)) ||
                        !lobby->peer_has_game(identity->short_name))
                        continue;
                    linked_indices.push_back(index);
                    linked_labels.push_back(
                        std::string(system_link ? "SYSTEM LINK  |  " :
                            wanted_mode ==
                                    arcade_multiplayer_mode::alternating ?
                                "ALTERNATING  |  " : "SIMULTANEOUS  |  ") +
                        choices[index].label);
                }
                const int selected = menu.select_interruptible(
                    system_link ? "Arcade System Link" :
                        wanted_mode == arcade_multiplayer_mode::alternating ?
                            "Two Machines - Alternating" :
                            "Two Machines - Simultaneous",
                    linked_labels.empty() ?
                        (system_link ?
                            "No System Link game is installed on both "
                            "machines." :
                            "No game of this style is installed on both "
                            "machines. Try the other style, or check the "
                            "same game is present on each.") :
                        "Choose once here. Player 2 will launch the same game "
                        "automatically.",
                    linked_labels, "Back to Network Multiplayer", 0,
                    [lobby] { return !lobby->connected(); });
                if (selected == launcher_menu::exit_requested) {
                    rom_selection_result exit_result;
                    exit_result.action =
                        rom_selection_action::exit_requested;
                    return exit_result;
                }
                if (selected == launcher_menu::interrupted) continue;
                if (selected < 0) continue;
                if (selected >= static_cast<int>(linked_indices.size()))
                    continue;
                const rom_choice& choice = choices[
                    linked_indices[static_cast<std::size_t>(selected)]];
                const auto identity = identify_arcade_game(choice.path);
                if (!identity) continue;
                lobby->launch_game(identity->short_name);
                run_loading_screen(menu, choice);

                return {rom_selection_action::selected, choice.path, cabinet_launch_mode::linked_network, 1, false, false,
                        true, {}};
            }
            continue;
        }
        if (selected_page == 1) {
            // Arcade wall: pick how many columns, then a game for each. Every
            // column runs its own independent cabinet in one fullscreen
            // window; the highlighted column is the one you play, and Tab
            // moves that highlight along.
            if (choices.size() < 2) {
                menu.show_text(
                    "Arcade Wall",
                    "At least two installed games are needed to build a "
                    "wall.", "Back");
                continue;
            }
            // How many cabinets this machine can actually drive: each
            // column is a whole board in its own process, so the number
            // offered comes from the processor and the display rather than
            // being fixed. Anything beyond that is still listed, greyed out,
            // so the ceiling is visible rather than mysterious.
            int wall_display_width = 0;
            if (const SDL_DisplayMode* desktop =
                    SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay()))
                wall_display_width = desktop->w;
            if (wall_display_width <= 0) wall_display_width = 1920;
            const int capacity = wall_column_capacity(
                std::thread::hardware_concurrency(), wall_display_width);
            std::vector<launcher_menu::mode_card> counts;
            for (int columns = wall_min_columns; columns <= wall_max_columns;
                 ++columns) {
                const bool unavailable = columns > capacity;
                counts.push_back({
                    std::to_string(columns) + " Games",
                    "Independent cabinets across this display",
                    launcher_menu::mode_icon::display_wall, columns,
                    unavailable,
                    unavailable ? "ABOVE THIS MACHINE'S CAPACITY" :
                                  "READY"});
            }
            const int chosen = menu.select_modes(
                "Arcade Wall",
                "Choose how many independent games to run side by side. "
                "This machine can drive " + std::to_string(capacity) +
                    " at once.", counts, "Back to Main Menu", 0);
            if (chosen == launcher_menu::exit_requested) {
                rom_selection_result exit_result;
                exit_result.action = rom_selection_action::exit_requested;
                return exit_result;
            }
            if (chosen < 0) continue;
            const std::size_t wanted =
                static_cast<std::size_t>(chosen + wall_min_columns);
            // Columns are picked one at a time through the same browser as
            // everywhere else - views, filters and covers included. Backing
            // out of a column re-picks the previous one, and the same game
            // may be chosen twice.
            std::vector<std::string> picked;
            while (picked.size() < wanted) {
                const std::optional<std::size_t> pick = browse_for_game(
                    "Arcade Wall - Column " +
                        std::to_string(picked.size() + 1) + " of " +
                        std::to_string(wanted),
                    {});
                if (exit_from_nested_browser) {
                    rom_selection_result exit_result;
                    exit_result.action =
                        rom_selection_action::exit_requested;
                    return exit_result;
                }
                if (!pick) {
                    if (picked.empty()) break;
                    picked.pop_back();
                    continue;
                }
                picked.push_back(choices[*pick].path);
            }
            if (picked.size() != wanted) continue;
            rom_selection_result wall;
            wall.action = rom_selection_action::selected;
            wall.path = picked[0];
            wall.launch_mode = cabinet_launch_mode::single;
            wall.wall_games = std::move(picked);
            return wall;
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
            "MANX defaults to English, upright, demo sound on, "
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
