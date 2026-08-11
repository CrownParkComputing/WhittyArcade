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
#include "manx_cloud_config.h"
#include "manx_http.h"
#include "online_link.h"
#include "persistent_data.h"
#include "platform_file_dialog.h"
#include "rom_library.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <array>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include "stb_image.h"
#include <fstream>
#include <map>
#include <optional>
#include <tuple>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {
using board_counts = std::array<int, arcade_board_count>;

bool hidden_from_launcher(arcade_board_type board) {
    // Native game plugins are installed, playable titles. Hiding this board
    // type removed every recomp from `choices` after discovery, so startup
    // reported the plugins while the X360 Recomp shelf remained empty.
    (void)board;
    return false;
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

// Fetching the link settings the website publishes, and putting them where
// the boards look.
//
// The download is the same manifest the site draws its own list from, so
// there is one source for "which boards need this" rather than a list here
// that goes stale. Files that already exist are left alone: somebody may
// have configured a board themselves, and quietly overwriting that would be
// a poor way to repay them for reading the instructions.
struct eeprom_import { int installed{}; int kept{}; int failed{}; bool reached{}; };

eeprom_import install_link_eeproms() {
    eeprom_import result;
    manx_http::request ask;
    ask.verb = manx_http::method::get;
    ask.url = manx_cloud::lobby_service() + "/api/../nvram/index.json";
    ask.timeout_seconds = 20;
    manx_http::response answer = manx_http::perform(ask);
    if (!answer.ok()) {
        ask.url = manx_cloud::lobby_service() + "/nvram/index.json";
        answer = manx_http::perform(ask);
    }
    if (!answer.ok()) return result;
    result.reached = true;

    const nlohmann::json body =
        nlohmann::json::parse(answer.body, nullptr, false);
    if (body.is_discarded()) return result;
    const auto settings = body.find("settings");
    if (settings == body.end() || !settings->is_object()) return result;

    const std::filesystem::path root(nvram_root_path());
    for (auto board = settings->begin(); board != settings->end(); ++board) {
        const auto files = board.value().find("files");
        if (files == board.value().end() || !files->is_array()) continue;
        for (const nlohmann::json& file : *files) {
            const std::string where = file.value("path", std::string());
            const std::string url = file.value("url", std::string());
            if (where.empty() || url.empty()) continue;
            // No traversal out of the nvram folder, whatever the manifest
            // says: this writes to disk from something fetched over the
            // network, and the only safe assumption is that it is hostile.
            if (where.find("..") != std::string::npos ||
                where.front() == '/') { ++result.failed; continue; }

            const std::filesystem::path target = root / where;
            std::error_code error;
            if (std::filesystem::exists(target, error)) { ++result.kept; continue; }

            manx_http::request fetch;
            fetch.verb = manx_http::method::get;
            fetch.url = manx_cloud::lobby_service() + url;
            fetch.timeout_seconds = 20;
            const manx_http::response got = manx_http::perform(fetch);
            if (!got.ok() || got.body.empty()) { ++result.failed; continue; }

            std::filesystem::create_directories(target.parent_path(), error);
            std::ofstream out(target, std::ios::binary | std::ios::trunc);
            if (!out) { ++result.failed; continue; }
            out.write(got.body.data(),
                      static_cast<std::streamsize>(got.body.size()));
            if (!out.good()) { ++result.failed; continue; }
            ++result.installed;
        }
    }
    return result;
}

// The first run.
//
// MANX needs four things put somewhere before it is any use, and the old
// first-run screen asked for two of them in one sentence and explained
// none. Somebody who has just downloaded an arcade emulator does not yet
// know that a "CHD" is a disc image, that a linked-cabinet board keeps its
// wiring in a settings chip, or that artwork is optional - and finding out
// by trial is how a working program gets deleted as broken.
//
// So it is a walk through: what this folder is for, what goes in it, and
// what happens if it is empty. It ends by pointing at the thing the whole
// program is for rather than at a shelf somebody has to work out.
enum class setup_outcome { quit, ready, host_a_game };

setup_outcome run_setup_wizard(launcher_menu& menu,
                               online_link* online) {
    using card = launcher_menu::mode_card;
    using icon = launcher_menu::mode_icon;
    emulator_settings settings = load_settings();

    const auto page = [&](const std::string& title,
                          const std::string& body,
                          std::vector<card> choices,
                          const std::string& back) {
        return menu.select_modes(title, body, choices, back, 0, {});
    };

    // --- what this is ----------------------------------------------------
    if (page("Welcome to MANX",
             "MANX plays arcade boards - properly emulated, and able to play "
             "across two machines anywhere in the world. It needs to be told "
             "where a few things live. Nothing is copied, imported or "
             "repacked: your files stay exactly where they are.\n\n"
             "This takes about a minute.",
             {card("Set MANX up", "Four questions, then a game",
                   icon::settings)},
             "Exit MANX") < 0)
        return setup_outcome::quit;

    // --- the games -------------------------------------------------------
    // A folder that is already set is an answer, not a question. Somebody
    // re-running this - or running it after an upgrade - should be able to
    // say "those are right" rather than find the picker for a path they
    // chose last week.
    for (;;) {
        std::vector<card> choices;
        // There is always an effective folder - an explicit one, or MANX's
        // own per-user library when nobody has chosen. Either way it is
        // already set, so offer it rather than opening a picker for a path
        // that is not going to change.
        const bool have_roms = true;
        choices.push_back(card("Keep what I have", rom_library_path(),
                               icon::folder));
        choices.push_back(card(have_roms ? "Choose a different folder"
                                         : "Choose my ROM folder",
                               "Where your zips live", icon::folder));
        choices.push_back(card("Use MANX's own folder",
                               "An empty one to fill later", icon::storage));

        const int chosen = page(
            "1 of 5 - Your games",
            "MANX reads the same ROM sets MAME does: one zip per game, named "
            "by its MAME short name - galaxian.zip, srallyc.zip. Point it at "
            "the folder you keep them in.\n\n"
            "MANX ships no games. If you have none yet, the website says "
            "where the legitimate ones are - and you can come back to this "
            "in Settings at any time.",
            choices, "Back");
        if (chosen < 0) return setup_outcome::quit;
        if (have_roms && chosen == 0) break;               // keep it
        if (chosen == static_cast<int>(choices.size()) - 1) {
            settings.rom_directory.clear();
            break;
        }
        const std::vector<std::string> picked =
            platform_file_selection(true, false, "Choose your MAME ROM folder");
        if (picked.empty()) continue;
        settings.rom_directory = picked.front();
        break;
    }

    // --- the disc images -------------------------------------------------
    for (;;) {
        std::vector<card> choices;
        const bool have_chd = true;
        choices.push_back(card("Keep what I have", chd_library_path(),
                               icon::folder));
        choices.push_back(card("Beside my ROMs", "The same folder as the zips",
                               icon::folder));
        choices.push_back(card(have_chd ? "Choose a different folder"
                                        : "A separate folder",
                               "Keep disc images apart", icon::storage));

        const int chosen = page(
            "2 of 5 - Disc images",
            "The bigger boards - Killer Instinct, the Model 2 racers - shipped "
            "with a hard disk as well as ROM chips, and that disk is a .chd "
            "file. If you have any, they can sit beside your zips or in a "
            "folder of their own.\n\n"
            "Games that need one simply will not appear until it is there.",
            choices, "Back");
        if (chosen < 0) return setup_outcome::quit;
        if (have_chd && chosen == 0) break;                // keep it
        if (chosen == static_cast<int>(choices.size()) - 2) {
            settings.chd_directory = settings.rom_directory;
            break;
        }
        const std::vector<std::string> picked = platform_file_selection(
            true, false, "Choose your arcade CHD folder");
        if (picked.empty()) continue;
        settings.chd_directory = picked.front();
        break;
    }

    // --- the link settings ------------------------------------------------
    // Not a folder to choose: a thing to know about, because the failure it
    // causes looks like a broken emulator rather than an unconfigured board.
    for (;;) {
        const int chosen = page(
            "3 of 5 - Linked cabinets",
            "A few boards - Sega Rally, Daytona, Manx TT, Motor Raid, Rave "
            "Racer, Ridge Racer 2 - link real cabinets together through their "
            "own hardware, and read their cabinet number and link mode out of "
            "a settings chip. A real arcade had that set once, on "
            "installation. A freshly emulated board has not, and sits on "
            "CHECKING NETWORK for ever.\n\n"
            "MANX can fetch those chips now - 128 bytes each - and put them "
            "where the boards look. Anything you have already configured is "
            "left alone.\n\n"
            "Every other game needs none of this: MANX drives the second "
            "player itself.",
            {card("Fetch them now", "Six boards, a few hundred bytes",
                  icon::refresh),
             card("Not now", "I will do it by hand later", icon::exit)},
            "Back");
        if (chosen < 0) return setup_outcome::quit;
        if (chosen != 0) break;

        menu.show_splash("Fetching link settings", 0.5f);
        const eeprom_import got = install_link_eeproms();
        if (!got.reached) {
            if (page("Could not reach the website",
                     "The link settings live at " +
                     manx_cloud::lobby_service() + " and it did not answer. "
                     "This is not fatal - the boards that need them will say "
                     "CHECKING NETWORK until they have them, and you can try "
                     "again from Settings whenever you like.",
                     {card("Try again", "", icon::refresh),
                      card("Carry on", "", icon::exit)}, "Back") == 0)
                continue;
            break;
        }
        std::string said = std::to_string(got.installed) +
                           " settings file(s) installed";
        if (got.kept)
            said += ", " + std::to_string(got.kept) +
                    " left alone because this machine already had them";
        if (got.failed)
            said += ", " + std::to_string(got.failed) + " could not be written";
        page("Link settings installed", said + ".\n\nThey went into:\n" +
             nvram_root_path(),
             {card("Carry on", "", icon::information)}, "");
        break;
    }

    // --- the artwork -------------------------------------------------------
    if (page("4 of 5 - Artwork",
             "Optional, and entirely cosmetic: box art, marquees and "
             "screenshots make the game list something to browse rather than "
             "a list of names. Without it every game still plays; it just "
             "shows its name.\n\n"
             "Settings, Import Media brings in a pack from a folder or a "
             "network share whenever you have one.",
             settings.media_directory.empty()
                 ? std::vector<card>{
                       card("Skip for now", "Names are enough to start",
                            icon::exit),
                       card("Choose a media folder", "I have a pack already",
                            icon::title_art)}
                 : std::vector<card>{
                       card("Keep what I have", settings.media_directory,
                            icon::title_art),
                       card("Choose a different folder", "Somewhere else",
                            icon::folder)},
             "Back") == 1 && settings.media_directory.empty()) {
        const std::vector<std::string> picked = platform_file_selection(
            true, false, "Choose your media folder");
        if (!picked.empty()) settings.media_directory = picked.front();
    }

    settings.library_setup_complete = true;
    save_settings(settings);

    // --- who you are ------------------------------------------------------
    // Without this the last question is a promise the program cannot keep:
    // opening a lobby needs an account, and a cabinet that has never signed
    // in queues the request against nobody. Asked here, once, rather than
    // found later in the corner of another screen.
    // Skipped entirely when this machine already has an account: a cabinet
    // that has been signed in for months is not signed in during the first
    // few seconds of a start, and asking it to sign up again is asking
    // somebody to answer a question the program already knows the answer to.
    if (online && online->state() != online_state::disabled &&
        !online->signed_in() && !online->remembered()) {
        for (;;) {
            const int chosen = page(
                "5 of 5 - Your account",
                "Playing across machines needs an account: it is how another "
                "cabinet knows who you are, how friends find you, and how a "
                "lobby you open is yours.\n\n"
                "It is free, it is made here on this machine, and the address "
                "is only ever used to sign in. Skip it and everything else "
                "still works - you just play on your own.",
                {card("Create an account", "Name, email and a password",
                      icon::local_players),
                 card("Sign in", "I already have one", icon::network),
                 card("Skip for now", "Play on my own", icon::exit)},
                "Back");
            if (chosen < 0) return setup_outcome::quit;
            if (chosen == 2) break;

            if (chosen == 0) {
                const auto name = menu.prompt_text(
                    "Your name", "What other players will see.", {}, false, 32);
                if (!name || name->empty()) continue;
                const auto email = menu.prompt_text(
                    "Email address", "Used to sign in on any machine.", {},
                    false, 128);
                if (!email || email->empty()) continue;
                const auto password = menu.prompt_text(
                    "Password", "At least six characters.", {}, true, 64);
                if (!password || password->size() < 6) continue;
                online->register_account(*name, *email, *password, true);
            } else {
                const auto email = menu.prompt_text(
                    "Email address", "The address you registered with.",
                    online->account_email(), false, 128);
                if (!email || email->empty()) continue;
                const auto password = menu.prompt_text(
                    "Password", "", {}, true, 64);
                if (!password || password->empty()) continue;
                online->sign_in(*email, *password, true);
            }

            // Signing in is a round trip on another thread. Waited for here
            // rather than hoped about, because the next screen offers to open
            // a lobby and needs to know whether that is going to work.
            for (int tick = 0; tick < 100 && !online->signed_in(); ++tick) {
                if (online->state() == online_state::error) break;
                menu.show_splash(online->status_text(), 0.5f);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (online->signed_in()) break;

            if (page("That did not work", online->status_text(),
                     {card("Try again", "Back to the account screen",
                           icon::refresh),
                      card("Skip for now", "Play on my own", icon::exit)},
                     "Back") != 0)
                break;
        }
    }

    // --- and now a game ----------------------------------------------------
    // The point of finishing a setup screen is playing something, so the last
    // question is the first game rather than "OK".
    // A remembered account is about to be signed in - the worker is doing it
    // now - so the offer stands. Waiting a moment is better than deciding on
    // a fact that is three seconds out of date.
    if (online && online->remembered() && !online->signed_in())
        for (int tick = 0; tick < 40 && !online->signed_in(); ++tick) {
            menu.show_splash(online->status_text(), 0.6f);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    const bool can_host = online && (online->signed_in() ||
                                     online->remembered());
    const int finish = page(
        "Ready",
        can_host
            ? "That is everything. MANX is at its best with somebody else on "
              "the other end: choose a game, open a lobby, and whoever joins "
              "plays the same board on their own machine - not a video of "
              "yours.\n\nYou can do this any time from the account button "
              "in the corner of the shelf."
            : "That is everything. Playing across machines needs an account, "
              "which you can make at any time from the account button in the "
              "corner of the shelf.",
        can_host
            ? std::vector<card>{
                  card("Open my first lobby",
                       "Choose a game and wait for somebody", icon::network),
                  card("Just look around", "Browse the shelf", icon::solo)}
            : std::vector<card>{
                  card("Browse the shelf", "See what is installed",
                       icon::solo)},
        "");
    return (can_host && finish == 0) ? setup_outcome::host_a_game
                                     : setup_outcome::ready;
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
              launcher_menu::mode_icon::settings},
             {"Run Setup Again", "Walk through first-time setup",
              launcher_menu::mode_icon::refresh}},
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
        } else if (selected == 8) {
            // The wizard only ever ran on a machine that had never been set
            // up, so on every other machine there was no way to see it again
            // - not to change a folder, not to fetch the link settings it
            // offers, not to read what it says about any of them. Clearing
            // the flag is all it takes: the launcher runs it on the way back
            // in, and everything it asks defaults to what is already set.
            settings.library_setup_complete = false;
            save_settings(settings);
            menu.show_text(
                "Setup will run next time MANX starts",
                "The flag it checks is read once, when the launcher opens, "
                "so this takes effect the next time you start MANX - or when "
                "you come back to the shelf after a game.\n\nEvery question "
                "already knows your current answer, so it is confirming "
                "rather than choosing again.");
            continue;
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
            items, "Back");
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
            counts, false, "Back");
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

void show_high_score_viewer(launcher_menu& menu, online_link* online) {
    const auto& manifests = supported_rom_sets();
    std::vector<const rom_set_manifest*> score_games;
    for (const auto& manifest : manifests) {
        if (manifest.working && has_high_score_decoder(manifest.short_name))
            score_games.push_back(&manifest);
    }
    const bool has_online = online &&
        (!online->leaderboard().empty() ||
         !online->leaderboard_status().empty());
    if (score_games.empty() && !has_online) {
        menu.show_text(
            "High Scores",
            "No verified high-score decoders are available yet.\n\n"
            "Each game's binary memory layout and checksums must be\n"
            "independently verified before MANX will decode its\n"
            "score table.",
            "Back");
        return;
    }
    const int total = static_cast<int>(score_games.size()) +
                      (has_online ? 1 : 0);
    menu.show_scoreboard(
        "Back", "Hall of Fame", total,
        [&](int index) -> launcher_menu::scoreboard_page {
            if (index >= static_cast<int>(score_games.size())) {
                launcher_menu::scoreboard_page page;
                page.title = "Online Leaderboard";
                page.subtitle = "MANX Online | client-submitted";
                const std::vector<online_leaderboard_entry> entries =
                    online->leaderboard();
                const std::size_t shown =
                    std::min<std::size_t>(entries.size(), 100);
                for (std::size_t rank = 0; rank < shown; ++rank) {
                    launcher_menu::scoreboard_row row;
                    row.rank = static_cast<int>(rank) + 1;
                    row.name = entries[rank].display_name;
                    if (!entries[rank].verified) row.name += "  UNVERIFIED";
                    row.score = format_high_score(entries[rank].value);
                    page.rows.push_back(std::move(row));
                }
                page.message = online->leaderboard_status();
                return page;
            }
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

// The letter a game files under. Leading articles are skipped, because a
// shelf where a third of the titles are under T is not a shelf anybody can
// use, and everything that is not a letter files together under #.
std::string initial_of(const rom_choice& choice) {
    std::string name = choice.label;
    for (const char* article : {"The ", "A ", "An "}) {
        const std::size_t length = std::strlen(article);
        if (name.size() > length &&
            std::equal(article, article + length, name.begin(),
                       [](char a, char b) {
                           return std::tolower(static_cast<unsigned char>(a)) ==
                                  std::tolower(static_cast<unsigned char>(b));
                       })) {
            name = name.substr(length);
            break;
        }
    }
    if (name.empty()) return "#";
    const unsigned char first = static_cast<unsigned char>(name[0]);
    if (!std::isalpha(first)) return "#";
    return std::string(1, static_cast<char>(std::toupper(first)));
}

// One publisher per page, not one credit line per page. "Armenia / Food
// and Fun" is two names on a cabinet, and paging by the pair puts a
// category of one game between Atari and Capcom.
std::string publisher_group_of(const rom_choice& choice) {
    const std::size_t split = choice.publisher.find(" / ");
    return split == std::string::npos ? choice.publisher
                                      : choice.publisher.substr(0, split);
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
    const std::string& back_label = "Back",
    // Called once per frame while the browser is on screen, for anything
    // the caller wants kept current without the page being torn down and
    // rebuilt around it - the online chip in the corner, chiefly.
    const std::function<void()>& on_frame = {}) {

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
    std::map<std::size_t, banner::banner_image> flow_fanart;
    // One cache per kind of art, because a game that has no cartridge photo
    // must not be re-searched for one on every frame.
    std::array<std::map<std::size_t, banner::banner_image>, 5> flow_extras;
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
        // 0 none, 1 boards, 2 publishers, 3 first letter. This is the
        // thing up and down move between, so it starts from the saved
        // choice rather than at "no grouping" - a carousel where one axis
        // does nothing is a carousel with half its library out of reach.
        const auto group_mode_of = [](const std::string& name) {
            if (name == "board") return 1;
            if (name == "publisher") return 2;
            if (name == "letter") return 3;
            return 0;
        };
        int page_mode = group_mode_of(load_settings().browse_group);
        int page_index = 0;
        // The first category shown is the fullest one, not whichever name
        // sorts first. Opening on a publisher with a single game reads as a
        // library with one game in it.
        bool page_picked = false;
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
                else if (!page_picked) {
                    page_picked = true;
                    int best = 0;
                    for (std::size_t index = 0; index < board_pages.size();
                         ++index) {
                        const std::size_t slot = static_cast<std::size_t>(
                            board_pages[index]);
                        if (page_counts[slot] > page_counts[
                                static_cast<std::size_t>(board_pages[
                                    static_cast<std::size_t>(best)])])
                            best = static_cast<int>(index);
                    }
                    page_index = best;
                }
            } else if (page_mode == 2 || page_mode == 3) {
                std::map<std::string, int> counted;
                for (const browse_entry& entry : entries) {
                    const rom_choice& choice = choices[entry.index];
                    if (play_style && !matches_play_style(choice, *play_style))
                        continue;
                    const std::string key = page_mode == 3 ?
                        initial_of(choice) : publisher_group_of(choice);
                    if (!key.empty()) ++counted[key];
                }
                for (const auto& page : counted)
                    publisher_pages.push_back(page.first);
                if (publisher_pages.empty()) page_mode = 0;
                else if (!page_picked) {
                    page_picked = true;
                    int best = 0;
                    for (std::size_t index = 0; index < publisher_pages.size();
                         ++index)
                        if (counted[publisher_pages[index]] >
                            counted[publisher_pages[
                                static_cast<std::size_t>(best)]])
                            best = static_cast<int>(index);
                    page_index = best;
                }
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
                    (page_mode == 3 ? initial_of(choice)
                                    : publisher_group_of(choice)) !=
                        publisher_narrow)
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
                    if (on_frame) on_frame();
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
                    // Screenshots only. Fan art used to be the last resort
                    // here, which meant the one wide image in the pack was
                    // spent on a small pane and the page behind it had
                    // nothing to show.
                    const banner::banner_image& snap = flow_art(
                        flow_snap,
                        {"images", "snaps", "snap", "thumbnails"});
                    if (snap.valid()) {
                        media.snap_pixels = snap.rgba.data();
                        media.snap_w = snap.width;
                        media.snap_h = snap.height;
                    }

                    const banner::banner_image& fanart = flow_art(
                        flow_fanart, {"fanarts", "fanart", "backgrounds"});
                    if (fanart.valid()) {
                        media.fanart_pixels = fanart.rgba.data();
                        media.fanart_w = fanart.width;
                        media.fanart_h = fanart.height;
                    }

                    // Everything else the pack happens to have. Each is
                    // one category only - no falling back - so a picture
                    // never appears twice on the page under two names.
                    static constexpr struct {
                        const char* directory;
                        const char* label;
                    } other_art[] = {
                        {"box3d",      "3D BOX"},
                        {"titles",     "TITLE"},
                        {"boxback",    "BACK"},
                        {"cartridges", "CART"},
                        {"cabinets",   "CABINET"},
                    };
                    for (std::size_t slot = 0;
                         slot < flow_extras.size(); ++slot) {
                        const banner::banner_image& art = flow_art(
                            flow_extras[slot], {other_art[slot].directory});
                        if (!art.valid()) continue;
                        media.extras.push_back({art.rgba.data(), art.width,
                                                art.height,
                                                other_art[slot].label});
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
            // Anything that leaves the browser and comes straight back -
            // an info page, a view change - comes back to the game that was
            // on screen rather than to the top of the list.
            remembered = menu.last_browse_selection();
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
                constexpr int first_group = 8;  // cards 8..11 pick the filter
                constexpr int arrange = 12;     // card 12 opens the designer
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
                     {"Filter: Board", "Up and down move between boards",
                      launcher_menu::mode_icon::storage},
                     {"Filter: Publisher", "Up and down move between makers",
                      launcher_menu::mode_icon::folder},
                     {"Filter: Letter", "Up and down move A to Z",
                      launcher_menu::mode_icon::audit},
                     {"Filter: Off", "One long run of every game",
                      launcher_menu::mode_icon::refresh},
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
                } else if (picked >= first_group &&
                           picked < first_group + 4) {
                    // Board, publisher, letter, off - in that order.
                    static const char* const names[] = {
                        "board", "publisher", "letter", "none"};
                    const int wanted = picked - first_group;
                    page_mode = wanted == 3 ? 0 : wanted + 1;
                    page_index = 0;
                    emulator_settings settings = load_settings();
                    settings.browse_group = names[wanted];
                    save_settings(settings);
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
                                       multiplayer_lobby* lobby,
                                       online_link* online) {
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
    bool wizard_wants_host = false;
    if (!load_settings().library_setup_complete) {
        const setup_outcome outcome = run_setup_wizard(menu, online);
        if (outcome == setup_outcome::quit) {
            rom_selection_result exit_result;
            exit_result.action = rom_selection_action::exit_requested;
            return exit_result;
        }
        wizard_wants_host = outcome == setup_outcome::host_a_game;
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
    // Another machine's console output, relayed over the discovery link.
    // Reachable while still searching as well as while connected, because
    // the case worth reading is the one where a machine has just died.
    const auto show_machine_log = [&menu, lobby](const lobby_machine& from) {
        const std::vector<std::string> lines = lobby->peer_log(from.nonce);
        std::string body;
        if (lines.empty()) {
            body = "Nothing received from this machine yet. Every machine "
                   "relays its output continuously once they have found "
                   "each other, so this fills in as they pair - and what a "
                   "machine printed just before it stopped stays here.";
        } else {
            for (const std::string& line : lines) {
                body += line;
                body += '\n';
            }
        }
        menu.show_text("Log from " + from.label() + "  (" + from.address + ")",
                       body, "Back");
    };

    // With several machines around, which one's log is being asked for has
    // to be a question rather than an assumption.
    const auto choose_machine_log = [&menu, lobby, show_machine_log] {
        for (;;) {
            const std::vector<lobby_machine> found = lobby->machines();
            if (found.empty()) {
                menu.show_text(
                    "Machine Logs",
                    "No other machine is answering, so there is nothing "
                    "being relayed to read.", "Back");
                return;
            }
            if (found.size() == 1) {
                show_machine_log(found.front());
                return;
            }
            std::vector<launcher_menu::mode_card> cards;
            for (const lobby_machine& machine : found)
                cards.emplace_back(
                    machine.label(),
                    machine.has_log ? machine.address :
                                      machine.address + " - nothing yet",
                    launcher_menu::mode_icon::audit, 0, !machine.has_log);
            const int picked = menu.select_modes(
                "Machine Logs", "Every machine relays what it prints to "
                "every other. Choose one to read.", cards, "Back");
            if (picked < 0 ||
                picked >= static_cast<int>(found.size())) return;
            show_machine_log(found[static_cast<std::size_t>(picked)]);
        }
    };

    const auto take_remote_launch = [&]() -> rom_selection_result {
        if (!lobby) return {};
        const std::optional<std::string> game = lobby->take_launch();
        if (!game) return {};
        // The player number comes from the host's roster, so with more than
        // two machines each one starts as the cabinet it was actually
        // assigned rather than everybody assuming they are Player 2.
        rom_selection_result result =
            linked_result(*game, std::max(lobby->node(), 2));
        if (result.action == rom_selection_action::selected) {
            result.cabinet_count = std::max(lobby->agreed_count(), 2);
            return result;
        }
        menu.show_text(
            "Multiplayer game unavailable",
            "Player 1 selected " + *game +
                ", but that ROM is not installed in this MANX "
                "library.", "Back");
        return {};
    };

    // A round trip, in words as well as milliseconds. The number alone
    // answers "how far away is it"; what somebody actually wants to know is
    // whether the connection is what is wrong, and whether a machine in the
    // middle would help.
    const auto ping_label = [](int rtt) {
        if (rtt <= 0) return std::string("NO PING");
        const std::string ms = std::to_string(rtt) + " ms";
        if (rtt < 30)  return ms + " - LOCAL";
        if (rtt < 80)  return ms + " - GOOD";
        if (rtt < 150) return ms + " - PLAYABLE";
        return ms + " - POOR";
    };

    // What turns an internet lobby into a running game.
    //
    // The lobby service agrees who is playing and what; the launch itself is
    // the same machinery a LAN game uses, because by this point the machines
    // are ordinary entries in the same roster and nothing downstream knows
    // the difference. So this is the join between the two: the host asks the
    // roster, the roster agrees, and the game goes.
    //
    // Called every time round the shelf, so it happens whether or not
    // anybody is looking at the lobby screen - the whole point is that
    // somebody who opened a lobby and wandered off still gets pulled in.
    // When the host said go. A lobby that says "starting" and then does
    // nothing is the worst thing this can do, so it is given a while and
    // then told the truth.
    std::chrono::steady_clock::time_point starting_since{};
    const auto drive_online_session = [&]() -> rom_selection_result {
        if (!online || !lobby) return {};
        // A launch the host has already sent.
        //
        // Taken here rather than only on the shelf, because the machine that
        // joined is standing on the lobby screen watching for exactly this -
        // and that screen is not the shelf, so nothing there ever consumed
        // it. The host started, went into the game and waited for a player
        // two who was sitting in a lobby screen that had already been told
        // to go.
        if (lobby->launch_pending()) return take_remote_launch();
        if (online->joined_lobby().empty()) return {};
        const std::string game = online->lobby_game();
        if (game.empty()) return {};
        const int places = std::max(2, online->lobby_places());

        // Two places, both filled: there is nothing left to decide and
        // nobody left to wait for. More than two waits for its host.
        if (online->hosting_lobby() && !online->lobby_starting() &&
            places == 2 && online->members().size() >= 2)
            online->start_lobby();

        if (!online->lobby_starting()) {
            starting_since = {};
            // A machine that joined. It said yes when it joined; being asked
            // again by the very machine it joined is a question with one
            // answer, so it is not asked.
            if (!online->hosting_lobby() && lobby->pending_invitation())
                lobby->answer_invite(true);
            return {};
        }

        // Going. From here the two are either talking directly within a few
        // seconds or they are never going to.
        const auto now = std::chrono::steady_clock::now();
        if (starting_since.time_since_epoch().count() == 0)
            starting_since = now;

        if (!online->hosting_lobby()) {
            if (lobby->pending_invitation()) lobby->answer_invite(true);
            if (lobby->agreed_count() >= 2 || lobby->connected()) return {};
        } else {
            if (!lobby->hosting()) lobby->invite_everyone();
        }

        // Everybody has to be on the roster with a player number before the
        // launch goes out, or a machine arrives in the game as nobody.
        if (lobby->agreed_count() < 2) {
            if (now - starting_since < std::chrono::seconds(25)) return {};
            // Said plainly, with the thing to try. The addresses were
            // swapped through the lobby service - that part worked, which is
            // why both machines are listed - and the direct link between
            // them never came up.
            starting_since = {};
            const bool same_house = lobby->remote_peer_count() > 0;
            menu.show_text(
                "Could not reach the other machine",
                std::string("Both machines are in the lobby, but they could "
                            "not open a direct link to each other, so the "
                            "game has not started.\n\n") +
                    (same_house
                         ? "If they are on the same network, switch Network "
                           "Play on from the system menu - two cabinets in "
                           "one house should find each other directly "
                           "instead of going out to the internet and back.\n\n"
                         : "") +
                    "Otherwise this is usually a router that will not let "
                    "two machines punch through to each other. The lobby has "
                    "been left; you can try again.",
                "Back");
            online->leave_lobby();
            lobby->cancel_invite();
            return {};
        }
        if (!online->hosting_lobby()) return {};
        lobby->launch_game(game, places);
        rom_selection_result mine = linked_result(game, 1);
        if (mine.action != rom_selection_action::selected) {
            menu.show_text(
                "Multiplayer game unavailable",
                "This lobby is for " + online->lobby_game_name() +
                    ", which is not installed on this machine any more.",
                "Back");
            online->leave_lobby();
            return {};
        }
        mine.cabinet_count = std::max(lobby->agreed_count(), 2);
        for (const rom_choice& choice : choices) {
            const auto identity = identify_arcade_game(choice.path);
            if (identity && identity->short_name == game) {
                run_loading_screen(menu, choice);
                break;
            }
        }
        return mine;
    };

    // Internet play. Everything about it is a screen showing what the online
    // link is doing, because there is nothing here to drive: the whole
    // feature is getting two machines to exchange addresses, after which the
    // ordinary lobby screen next door does the rest.
    // A plain question, asked with the same tiles as everything else so a
    // cabinet never has to render a dialog it has no widget for.
    const auto ask_yes_no = [&](const std::string& question,
                                const std::string& detail,
                                const std::string& yes,
                                const std::string& no) {
        using card = launcher_menu::mode_card;
        using icon = launcher_menu::mode_icon;
        const int picked = menu.select_modes(
            question, detail,
            {card(yes, {}, icon::local_players), card(no, {}, icon::exit)},
            "Cancel", 0, {});
        return picked == 0;
    };

    // Signing in, and creating an account, on the cabinet itself. No
    // website, no second device, no code to carry between them.
    //
    // Set when the account page is asked for the friends list. The friends
    // screen is defined below this one and both capture by reference, so the
    // way through is a flag the caller acts on rather than one lambda
    // reaching forward into the other.
    bool account_wants_friends = false;
    // Set when the account screen is asked to host: the game browser is
    // defined below it, so the trip out and back is how the two meet.
    bool account_wants_host = false;
    // A game that started while somebody was looking at the lobby screen.
    // The screen cannot return a launch - it returns nothing - so it leaves
    // it here and the caller, which can, picks it up.
    rom_selection_result online_started;
    const auto show_online_screen = [&]() {
        if (!online) return;
        using card = launcher_menu::mode_card;
        using icon = launcher_menu::mode_icon;
        online->set_foreground(true);
        for (;;) {
            // Driven from in here as well as from the shelf: a host waiting
            // on this very screen for somebody to join is the most likely
            // person in the building to be waiting for it.
            if (rom_selection_result go = drive_online_session();
                go.action == rom_selection_action::selected) {
                online_started = go;
                online->set_foreground(false);
                return;
            }
            const online_state state = online->state();
            // The corner is set when the shelf is drawn, so on any screen
            // that is not the shelf it kept whatever it last said - which
            // meant signing in and then watching OFFLINE sit there, because
            // nothing on this screen had ever updated it.
            const std::string who = online->display_name().empty()
                                        ? online->account_email()
                                        : online->display_name();
            menu.set_status(
                state == online_state::error
                    ? online->status_text()
                    : (online->signed_in()
                           ? (who.empty() ? std::string("ONLINE")
                                          : "ONLINE  " + who)
                           : std::string("OFFLINE")),
                online->signed_in());
            std::string description = online->status_text();
            if (state == online_state::online ||
                state == online_state::in_lobby) {
                description = "Signed in as " + online->account_email();
                if (!online->display_name().empty())
                    description = online->display_name() + " - " + description;
            }

            std::vector<card> cards;
            std::vector<int> what;
            enum { act_none, act_register, act_signin, act_signout,
                   act_forget, act_host, act_join, act_leave, act_friends,
                   act_start };
            const auto add = [&](card entry, int action) {
                cards.push_back(std::move(entry));
                what.push_back(action);
            };

            if (state == online_state::disabled) {
                add(card("Not Available", online->status_text(), icon::audit,
                         0, true), act_none);
            } else if (online->signed_in()) {
                if (online->joined_lobby().empty()) {
                    add(card("Host A Lobby",
                             "Get a code to read out to a friend",
                             icon::network), act_host);
                    add(card("Join A Lobby",
                             "Pick from the lobbies you can see",
                             icon::local_players), act_join);
                } else {
                    // The waiting room. What is being played, who is here,
                    // and - when there is a decision left to make - the one
                    // person who gets to make it.
                    const int places = std::max(2, online->lobby_places());
                    const std::vector<online_wire::member> here =
                        online->members();
                    const std::string what =
                        online->lobby_game_name().empty()
                            ? online->lobby_game()
                            : online->lobby_game_name();
                    add(card(what.empty() ? "Lobby " + online->joined_lobby()
                                          : what,
                             "Lobby " + online->joined_lobby() + "  -  " +
                                 std::to_string(here.size()) + " of " +
                                 std::to_string(places) + " here",
                             icon::network, 0, true,
                             online->lobby_starting() ? "STARTING" : "CODE"),
                        act_none);
                    const int rtt = lobby ? lobby->worst_rtt_ms() : 0;
                    for (const online_wire::member& member : here) {
                        const bool up = member.link == "connected";
                        std::string detail =
                            member.has_game
                                ? (up ? std::string("Here, and has the game")
                                      : std::string("Connecting"))
                                : std::string("Does not have this game");
                        // Measured by this cabinet, so it is only worth
                        // showing against somebody who is not this cabinet.
                        if (up && rtt > 0)
                            detail += "  -  " + ping_label(rtt);
                        add(card(member.name.empty() ? std::string("Cabinet")
                                                     : member.name,
                                 detail, icon::local_players, 0, true,
                                 !member.has_game ? "NO GAME"
                                                  : (up ? "READY"
                                                        : "CONNECTING")),
                            act_none);
                    }
                    // Two machines need no start button: the game goes the
                    // moment the second one is in. More than two is a
                    // judgement about who is still coming, and that is the
                    // host's to make.
                    if (online->hosting_lobby() && places > 2)
                        add(card("Start Now",
                                 here.size() >= 2
                                     ? "Play with everybody who is here"
                                     : "Nobody else has joined yet",
                                 icon::network, 0,
                                 here.size() < 2 || online->lobby_starting()),
                            act_start);
                    add(card("Leave Lobby", "Stop connecting to these machines",
                             icon::controls), act_leave);
                }
                // Who is about, and who is waiting for an answer. Counted
                // here so an unanswered request is visible from the corner
                // rather than only from inside the screen that lists it.
                {
                    const std::vector<online_friend> people = online->friends();
                    int waiting = 0;
                    int about = 0;
                    int agreed = 0;
                    for (const online_friend& person : people) {
                        if (person.incoming) ++waiting;
                        if (!person.accepted) continue;
                        ++agreed;
                        if (person.online) ++about;
                    }
                    std::string detail =
                        agreed == 0
                            ? std::string("Add somebody by their name")
                            : std::to_string(about) + " of " +
                                  std::to_string(agreed) + " online";
                    add(card("Friends", detail, icon::local_players, 0, false,
                             waiting ? std::to_string(waiting) + " WAITING"
                                     : std::string()),
                        act_friends);
                }
                add(card("Sign Out", online->account_email(), icon::controls),
                    act_signout);
                add(card("Forget This Machine",
                         "Wipe the saved account from this cabinet",
                         icon::controls), act_forget);
            } else {
                add(card("Create Account", "Name, email and a password",
                         icon::local_players), act_register);
                add(card("Sign In", "You already have an account",
                         icon::network), act_signin);
            }

            const uint64_t seen = online->revision();
            const std::function<bool()> changed = [online, lobby, seen] {
                return online->revision() != seen ||
                       (lobby && lobby->launch_pending());
            };
            const int picked = menu.select_modes(
                "Account", description, cards, "Back", 0, changed);
            if (picked == launcher_menu::interrupted) continue;
            if (picked < 0 || picked >= static_cast<int>(what.size())) break;

            switch (what[static_cast<std::size_t>(picked)]) {
            case act_register: {
                const auto name = menu.prompt_text(
                    "Your name", "What your friends will see.", {}, false, 32);
                if (!name || name->empty()) break;
                const auto email = menu.prompt_text(
                    "Email address", "Used to sign in on any cabinet.", {},
                    false, 128);
                if (!email || email->empty()) break;
                const auto password = menu.prompt_text(
                    "Password", "At least six characters.", {}, true, 64);
                if (!password || password->size() < 6) break;
                const bool remember = ask_yes_no(
                    "Stay signed in?",
                    "This cabinet will sign you in by itself next time. Say "
                    "no on a machine other people use.",
                    "Stay signed in", "Just this once");
                online->register_account(*name, *email, *password, remember);
                break;
            }
            case act_signin: {
                const auto email = menu.prompt_text(
                    "Email address", "The address you registered with.",
                    online->account_email(), false, 128);
                if (!email || email->empty()) break;
                const auto password = menu.prompt_text(
                    "Password", "", {}, true, 64);
                if (!password || password->empty()) break;
                const bool remember = ask_yes_no(
                    "Stay signed in?",
                    "This cabinet will sign you in by itself next time. Say "
                    "no on a machine other people use.",
                    "Stay signed in", "Just this once");
                online->sign_in(*email, *password, remember);
                break;
            }
            case act_friends:
                account_wants_friends = true;
                online->set_foreground(false);
                return;
            case act_signout: online->sign_out(); break;
            case act_forget:  online->forget_machine(); break;
            case act_host:
                // Picked in the cover browser, not in a list of tiles with
                // the names blown up to fill them. It is the same screen
                // used to choose a game to play alone - artwork, names, the
                // lot - and it lives further down this function, so the
                // caller runs it and comes back with an answer.
                account_wants_host = true;
                online->set_foreground(false);
                return;
            case act_join: {
                // A list, not a field. Nobody should have to be told a code
                // and type it in: an open lobby is there for anyone, and a
                // friends-only one is listed for the friends it was made for.
                using card = launcher_menu::mode_card;
                using icon = launcher_menu::mode_icon;
                auto last_asked = std::chrono::steady_clock::now() -
                                  std::chrono::seconds(60);
                for (;;) {
                    // Asked again while this screen is up, so a lobby
                    // somebody opens appears by itself - but on a timer, not
                    // every time round. Refreshing bumps the revision, the
                    // revision interrupts the screen, and the screen
                    // refreshed again: a query loop that never rested.
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_asked > std::chrono::seconds(4)) {
                        last_asked = now;
                        online->refresh_lobbies();
                    }
                    const std::vector<online_lobby> open = online->lobbies();
                    std::vector<card> rows;
                    for (const online_lobby& entry : open) {
                        std::string detail =
                            entry.host.empty() ? std::string("Someone")
                                               : entry.host;
                        if (!entry.game.empty()) detail += "  -  " + entry.game;
                        if (entry.places)
                            detail += "  -  " + std::to_string(entry.members) +
                                      " of " + std::to_string(entry.places);
                        rows.push_back(card(
                            entry.host.empty() ? entry.id
                                               : entry.host + "'s lobby",
                            detail, icon::network, 0, false,
                            entry.open_to_anyone ? "OPEN" : "FRIENDS"));
                    }
                    if (rows.empty())
                        rows.push_back(card("Nothing To Join",
                                            "No open lobbies, and none of "
                                            "your friends is hosting.",
                                            icon::audit, 0, true));

                    const uint64_t seen = online->revision();
                    const std::function<bool()> moved = [online, seen] {
                        return online->revision() != seen;
                    };
                    const int chosen = menu.select_modes(
                        "Join A Lobby", "Pick one and you are in.", rows,
                        "Back", 0, moved);
                    if (chosen == launcher_menu::interrupted) continue;
                    if (chosen >= 0 &&
                        static_cast<std::size_t>(chosen) < open.size())
                        online->join_lobby(open[
                            static_cast<std::size_t>(chosen)].id);
                    break;
                }
                break;
            }
            case act_start:   online->start_lobby(); break;
            case act_leave:   online->leave_lobby(); break;
            default: break;
            }
        }
        online->set_foreground(false);
    };

    // Who else is about. On a console this is the first thing anybody looks
    // at, and until now the only way to see it was the local network lobby -
    // which cannot show a friend in another house at all.
    bool friends_wants_lobby = false;
    const auto show_friends_screen = [&]() {
        if (!online) return;
        using card = launcher_menu::mode_card;
        using icon = launcher_menu::mode_icon;
        online->set_foreground(true);
        online->refresh_friends();
        int cursor = 0;
        for (;;) {
            std::vector<card> cards;
            // What each row is. A friends list is four kinds of thing in one
            // column - somebody to play with, somebody waiting on an answer,
            // a cabinet in the lobby, a machine on this network - and the
            // difference has to survive the trip back from the grid.
            enum class row { add, person, member, local };
            std::vector<std::pair<row, std::size_t>> rows;
            const std::vector<online_wire::member> members = online->members();
            const std::vector<online_friend> people = online->friends();
            const std::string joined = online->joined_lobby();

            std::string description;
            if (!online->signed_in()) {
                description =
                    "This cabinet is not signed in. Press the account button "
                    "in the corner to sign in, and your friends appear here.";
                cards.push_back(card("Not Signed In", online->status_text(),
                                     icon::audit, 0, true));
            } else {
                // The status line carries the answer to whatever was last
                // asked here - "nobody is called that", "Sam has been asked"
                // - which is the only place those answers can land.
                description = online->status_text();
                cards.push_back(card("Add A Friend",
                                     "Find somebody by the name they signed "
                                     "up with", icon::local_players));
                rows.emplace_back(row::add, 0);

                for (std::size_t index = 0; index < people.size(); ++index) {
                    const online_friend& person = people[index];
                    const std::string name =
                        person.name.empty() ? std::string("Somebody")
                                            : person.name;
                    std::string detail;
                    std::string badge;
                    if (person.incoming) {
                        detail = "Wants to be friends - press to answer";
                        badge = "ASKED YOU";
                    } else if (!person.accepted) {
                        detail = "Waiting for them to answer";
                        badge = "ASKED";
                    } else {
                        detail = person.online ? "Online now"
                                               : "Not online just now";
                        badge = person.online ? "ONLINE" : "OFFLINE";
                    }
                    cards.push_back(card(name, detail, icon::network, 0, false,
                                         badge));
                    rows.emplace_back(row::person, index);
                }
            }

            // Whoever is in the lobby with this machine right now, which is
            // a different question from who this account is friends with.
            if (!joined.empty()) {
                for (std::size_t index = 0; index < members.size(); ++index) {
                    const online_wire::member& member = members[index];
                    const bool up = member.link == "connected";
                    cards.push_back(card(
                        member.name.empty() ? std::string("Cabinet")
                                            : member.name,
                        up ? (member.has_game ? "In lobby " + joined +
                                                " - has the game"
                                              : "In lobby " + joined +
                                                " - does not have the game")
                           : (member.link.empty() ? "Waiting" : member.link),
                        icon::network, 0, !up, up ? "IN LOBBY" : "CONNECTING"));
                    rows.emplace_back(row::member, index);
                }
            }

            // Local machines are friends too, in the sense that matters:
            // somebody to play with right now.
            if (lobby) {
                const std::vector<lobby_machine> machines = lobby->machines();
                for (std::size_t index = 0; index < machines.size(); ++index) {
                    cards.push_back(
                        card(machines[index].label(),
                             "On this network - " + machines[index].address,
                             icon::network, 0, false, "LOCAL"));
                    rows.emplace_back(row::local, index);
                }
            }

            const uint64_t seen = online->revision();
            const std::function<bool()> changed = [online, lobby, seen] {
                return online->revision() != seen ||
                       (lobby && lobby->connected());
            };
            const int picked = menu.select_modes(
                "Friends", description, cards, "Back", cursor, changed);
            if (picked == launcher_menu::interrupted) continue;
            if (picked < 0 || static_cast<std::size_t>(picked) >= rows.size())
                break;
            cursor = picked;
            const auto& chosen = rows[static_cast<std::size_t>(picked)];
            if (chosen.first == row::add) {
                // Exact match, and it has to be: the handles collection
                // cannot be listed, which is what stops a cabinet reading
                // out the whole membership one keystroke at a time.
                const auto name = menu.prompt_text(
                    "Add a friend",
                    "Their name, exactly as they signed up with it.", {},
                    false, 32);
                if (name && !name->empty()) online->add_friend(*name);
                continue;
            }
            if (chosen.first == row::person) {
                const online_friend& person = people[chosen.second];
                if (!person.incoming) continue;
                // Three answers, not two: backing out of this leaves the
                // request where it was, because "I will decide later" is not
                // the same as "no" and should not be recorded as one.
                const int answer = menu.select_modes(
                    (person.name.empty() ? std::string("Somebody")
                                         : person.name) +
                        " wants to be friends",
                    "Friends can see when you are online, and can join the "
                    "lobbies you make for them.",
                    {card("Yes, we are friends", {}, icon::local_players),
                     card("No thanks", {}, icon::exit)},
                    "Ask me later", 0, {});
                if (answer == 0 || answer == 1)
                    online->answer_friend(person.uid, answer == 0);
                continue;
            }
            // Choosing a machine on this network goes where inviting it and
            // agreeing a game already lives, rather than duplicating that
            // negotiation on a second screen.
            if (chosen.first == row::local) friends_wants_lobby = true;
            break;
        }
        online->set_foreground(false);
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

    // Opening a lobby: what are we playing, how many machines, and who may
    // join. The game comes first because a lobby is an invitation to play
    // something, and it is chosen in the cover browser like any other game.
    const auto host_a_lobby = [&]() {
        if (!online) return;
        const auto picked = browse_for_game(
            "Host A Lobby - What Are You Playing?",
            [](const rom_choice& choice) {
                const auto identity = identify_arcade_game(choice.path);
                const rom_set_manifest* manifest = identity ?
                    find_supported_rom_set(identity->short_name) : nullptr;
                return manifest && (supports_network_two_player(*manifest) ||
                                    supports_native_system_link(*manifest));
            });
        if (!picked) return;

        const rom_choice& chosen = choices[*picked];
        const auto identity = identify_arcade_game(chosen.path);
        if (!identity) return;
        const rom_set_manifest* manifest =
            find_supported_rom_set(identity->short_name);
        if (!manifest) return;

        // Two-player games have nothing to ask: two is what they are. A
        // linked-cabinet game can take a row of them, and that answer also
        // decides whether the lobby starts by itself or waits for its host.
        int places = 2;
        std::string mode = "simultaneous";
        if (manifest->multiplayer == arcade_multiplayer_mode::alternating)
            mode = "alternating";
        if (supports_native_system_link(*manifest)) {
            mode = "native_link";
            using card = launcher_menu::mode_card;
            using icon = launcher_menu::mode_icon;
            const int how_many = menu.select_modes(
                "How many machines?",
                "Two starts as soon as somebody joins. More than two waits "
                "for you to say go.",
                {card("2 Machines", "Starts the moment one joins",
                      icon::local_players),
                 card("3 Machines", "You decide when to start",
                      icon::cabinet_count),
                 card("4 Machines", "You decide when to start",
                      icon::cabinet_count)},
                "Back", 0, {});
            if (how_many < 0) return;
            places = 2 + how_many;
        }

        const bool open_to_anyone = ask_yes_no(
            "Who can join?",
            "An open lobby is listed for anyone signed in. A friends only "
            "one is listed for your friends and nobody else.",
            "Open to anyone", "Friends only");
        online->create_lobby(open_to_anyone, identity->short_name,
                             chosen.label, places, mode);
    };

    // Grouped by who published the machine, not by the board inside it. A
    // player looking for Out Run is looking for Sega, and has no reason to
    // know or care that it is a System 16 or a Model 2 - the board is an
    // implementation detail of the emulator, and it made the shelf read like
    // a parts list.
    struct platform_entry {
        std::string publisher;
        std::string name;
        std::string logo;
        int games{};
        launcher_menu::mode_icon icon{launcher_menu::mode_icon::storage};
    };
    std::vector<platform_entry> platforms;
    const auto platform_name = [](arcade_board_type board) {
        if (board == arcade_board_type::game_plugin)
            return std::string("X360 Recomp");
        const int slot = board_slot(board);
        if (slot >= 0)
            return std::string(arcade_boards()[
                static_cast<std::size_t>(slot)].display_name);
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
    // A game with no publisher recorded still has to live somewhere, and a
    // shelf that silently omits it is worse than one with an "Other" tile.
    // The primary name only. Catalogue entries carry co-development credits
    // - "Sega / Westone", "Midway / Rare" - which split the shelf into tiles
    // nobody is looking for and match no logo anywhere, so every one of them
    // rendered as bare text. Wonder Boy belongs under Sega.
    const auto publisher_of = [&](const rom_choice& choice) {
        // Recompiled Xbox 360 titles arrive through the native game-plugin
        // boundary.  Their publisher is still useful on the game card, but it
        // is not their platform: grouping Bubble Bobble under Taito and
        // Geometry Wars under Bizarre made the Xbox shelf disappear entirely.
        if (choice.board == arcade_board_type::game_plugin)
            return platform_name(choice.board);
        if (choice.publisher.empty()) return std::string("Other");
        const std::size_t split = choice.publisher.find(" / ");
        return split == std::string::npos ? choice.publisher
                                          : choice.publisher.substr(0, split);
    };
    for (const rom_choice& choice : choices) {
        const std::string who = publisher_of(choice);
        const auto found = std::find_if(
            platforms.begin(), platforms.end(),
            [&](const platform_entry& entry) {
                return entry.publisher == who;
            });
        if (found != platforms.end()) {
            ++found->games;
        } else {
            // The logo key is the publisher's own name, so the artwork
            // fetcher looks for "Sega" rather than for whatever board
            // happened to be underneath.
            platforms.push_back({who, who, who, 1,
                                 platform_icon(choice.board)});
        }
    }
    std::stable_sort(platforms.begin(), platforms.end(),
                     [](const platform_entry& left,
                        const platform_entry& right) {
                         return left.name < right.name;
                     });

    // The wizard's last question was "play somebody?", and this is the first
    // moment the answer can be acted on: hosting needs the game browser and
    // the account, both of which are defined above this point.
    if (wizard_wants_host) {
        wizard_wants_host = false;
        if (!online) {
            menu.show_text(
                "Online play unavailable",
                "This copy of MANX was started without online support, so "
                "there is nobody to open a lobby with.");
        } else {
            // Not gated on being signed in yet. Signing in happens on its
            // own thread and takes a few seconds, and the wizard finishes
            // well inside that - so asking "are you signed in?" here
            // answered no on a machine that was about to be, and dropped
            // somebody who had just asked to play into a sign-in screen.
            //
            // The lobby command waits for the account by itself: it sits in
            // the queue until the cabinet is registered and then runs. So
            // pick the game now and let the two catch up with each other.
            host_a_lobby();
        }
    }

    // Platform is the first selection. Once inside one, Back returns to the
    // platform tiles; Back there opens the system menu. This keeps a marquee
    // carousel short and coherent instead of mixing every board generation.
    int play_style = 0;
    std::optional<std::string> selected_platform;
    // Start on the platform shelf. Recompiled Xbox 360 plugins belong to the
    // X360 Recomp category; native arcade boards keep their publisher shelf.
    bool browse_everything = false;
    int platform_cursor = 0;
    // Set when this machine has just agreed to a game: the network page is
    // opened for them rather than left to be found.
    bool enter_network_page = false;
    for (;;) {
        if (lobby) lobby->set_installed_games(choices);
        if (online) {
            // Short names, not the lobby's index mask. The mask numbers games
            // by their position in supported_rom_sets(), which plugins push
            // about, so two machines with different plugins installed give
            // the same game different bits and the mask means nothing between
            // them. Over the internet the builds will differ, so the cloud
            // path names games instead of numbering them.
            std::vector<std::string> installed;
            installed.reserve(choices.size());
            for (const rom_choice& choice : choices)
                if (const auto identity = identify_arcade_game(choice.path))
                    installed.push_back(identity->short_name);
            std::sort(installed.begin(), installed.end());
            installed.erase(std::unique(installed.begin(), installed.end()),
                            installed.end());
            online->set_installed_games(std::move(installed));
        }
        const bool lobby_connected_at_draw = lobby && lobby->connected();
        // Whether this cabinet is looking for the others on this network at
        // all. Off by default: it and Online Play sat side by side, one of
        // them searching from the moment the launcher opened, and they read
        // as one confusing feature rather than two clear ones.
        const bool lan_on = lobby && lobby->lan_discovery();
        // The grid re-enters this loop whenever the connection changes, so
        // setting it here is enough to keep it honest.
        const int machines_here = lobby ?
            static_cast<int>(lobby->machines().size()) : 0;
        // ONLINE or OFFLINE first, because that is the question, then who
        // is about. One line in the corner of the frame rather than two
        // tiles in the shelf.
        //
        // Written as something that can be called again while the browser is
        // still on screen. It used to be built once per lap of this loop,
        // which meant the only way to keep it honest was to leave the
        // browser and come back every time the online layer breathed - and
        // the online layer breathes every few seconds. That rebuilt the
        // page, restarted the snap video and put the selection back on the
        // first game in the first category, over and over.
        const auto refresh_status = [&] {
            const bool signed_in = online && online->signed_in();
            const bool broken = online &&
                                online->state() == online_state::error;
            // "OFFLINE" for a cabinet that signed in perfectly well and then
            // failed at the next step is the least useful thing this could
            // say. If something went wrong, say what.
            std::string state = broken ? online->status_text()
                                       : (signed_in ? "ONLINE" : "OFFLINE");
            if (signed_in && !online->display_name().empty())
                state += "  " + online->display_name();
            // A friend request nobody has answered belongs on the corner
            // itself. It is the one thing here that is waiting on the person
            // standing in front of the cabinet.
            int waiting = 0;
            if (signed_in)
                for (const online_friend& person : online->friends())
                    if (person.incoming) ++waiting;
            if (waiting)
                state += "  -  " + std::to_string(waiting) +
                         (waiting == 1 ? " friend request" : " friend requests");
            const std::size_t away = online ? online->members().size() : 0;
            if (machines_here || away) {
                state += "  -  " + std::to_string(machines_here + away) +
                         (machines_here + away == 1 ? " machine"
                                                    : " machines");
                if (away && machines_here) state += " here and online";
                else if (away)             state += " online";
                else                       state += " on this network";
            }
            menu.set_status(state, !broken &&
                                   (signed_in || (lobby && lobby->connected())));
        };
        refresh_status();
        // Only the things that genuinely need the browser to stop: a game
        // starting elsewhere, and an invitation that has to be answered.
        // Everything else the online layer reports - a heartbeat, a machine
        // appearing, the connection coming back - is a change to one chip in
        // the corner, and is drawn in place by the tick below.
        const std::function<bool()> interrupt = lobby ?
            std::function<bool()>([lobby] {
                return lobby->launch_pending() ||
                       lobby->pending_invitation().has_value();
            }) :
            std::function<bool()>{};
        // Someone on the other machine has asked for a game. Whatever this
        // machine was browsing, the question gets asked here rather than
        // waiting for the player to find the network page by themselves.
        // Not asked when it comes from an internet lobby this machine has
        // already joined: joining was the answer, and asking again is the
        // double confirmation this whole path exists to remove.
        const bool already_agreed_online =
            online && !online->joined_lobby().empty();
        const std::optional<lobby_machine> incoming_invitation =
            lobby && !already_agreed_online ? lobby->pending_invitation()
                                            : std::nullopt;
        if (incoming_invitation) {
            const int answer = menu.select_modes(
                "Multiplayer - Invitation",
                incoming_invitation->label() + " (" +
                    incoming_invitation->address +
                    ") wants to start a multiplayer game. Allowing it puts "
                    "this machine in their game; they choose what everyone "
                    "plays.",
                {{"Allow", "Join " + incoming_invitation->label() + "'s game",
                  launcher_menu::mode_icon::local_players},
                 {"Deny", "Stay on this machine",
                  launcher_menu::mode_icon::exit}},
                "Ask me later", 0,
                [lobby] { return !lobby->pending_invitation().has_value(); });
            if (answer == 0) {
                lobby->answer_invite(true);
                enter_network_page = true;
            } else if (answer == 1) {
                lobby->answer_invite(false);
            }
            if (answer == launcher_menu::exit_requested) {
                rom_selection_result exit_result;
                exit_result.action = rom_selection_action::exit_requested;
                return exit_result;
            }
            continue;
        }
        // An internet lobby that has filled up, or whose host has said go.
        // Driven here rather than from the lobby screen, so somebody who
        // opened a lobby and went back to browsing is still pulled into the
        // game when the other machine arrives.
        if (rom_selection_result online_launch = drive_online_session();
            online_launch.action == rom_selection_action::selected)
            return online_launch;

        // A lobby that has just appeared. Asked here, over whatever is on
        // screen, for the same reason an invitation is: a game somebody else
        // has opened is only worth knowing about while it is still open, and
        // a notification that waits in a screen nobody opens is no
        // notification at all. Each lobby is offered once - take_new_lobby
        // takes it - so saying no is not a question that comes back.
        if (online) {
            if (const std::optional<online_lobby> fresh =
                    online->take_new_lobby()) {
                const std::string who =
                    fresh->host.empty() ? std::string("Somebody") : fresh->host;
                std::string detail = who + " has just opened a lobby";
                if (!fresh->game.empty()) detail += " to play " + fresh->game;
                detail += ". Joining puts this cabinet in it, and you can "
                          "leave again whenever you like.";
                const int answer = menu.select_modes(
                    "A lobby has just opened", detail,
                    {{"Join " + who,
                      fresh->open_to_anyone ? "Open to anyone"
                                            : "They named you",
                      launcher_menu::mode_icon::network},
                     {"Not now", "Stay where you are",
                      launcher_menu::mode_icon::exit}},
                    "Not now", 0, {});
                if (answer == launcher_menu::exit_requested) {
                    rom_selection_result exit_result;
                    exit_result.action = rom_selection_action::exit_requested;
                    return exit_result;
                }
                if (answer == 0) online->join_lobby(fresh->id);
                continue;
            }
        }
        bool system_menu_requested = false;
        // The platform shelf is the front door. It keeps recomp titles under
        // one explicit X360 category instead of mixing them into arcade-board
        // publisher pages.
        if (!selected_platform && !browse_everything) {
            std::vector<launcher_menu::mode_card> platform_cards;
            std::vector<std::string> platform_logo_keys;
            platform_cards.reserve(platforms.size());
            platform_logo_keys.reserve(platforms.size());
            // The shelf is publishers and nothing else. Where this machine
            // stands - online, offline, who else is about - is drawn as a
            // status widget in the corner of the frame instead, because it
            // is something to glance at rather than something to scroll
            // past on the way to a game. Online Play and Friends live in
            // the system pages, where the things you act on live.
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
                "MANX",
                "Play on your own, or across the machines on your network.",
                platform_cards, "System Menu", platform_cursor, interrupt,
                collect_platform_logos);
            if (platform_choice == launcher_menu::exit_requested) {
                rom_selection_result exit_result;
                exit_result.action = rom_selection_action::exit_requested;
                return exit_result;
            }
            // The account button, pressed. Signing in, signing out, friends
            // and lobbies open over the shelf instead of being a trip
            // through the system pages.
            if (platform_choice == launcher_menu::shortcut) {
                if (!online) {
                    // A button that does nothing when pressed is worse than
                    // no button, so say why rather than blinking.
                    menu.show_text("Account",
                                   "This copy of MANX was started without "
                                   "online support, so there is no account "
                                   "to sign in to.");
                    continue;
                }
                show_online_screen();
                if (online_started.action == rom_selection_action::selected)
                    return online_started;
                if (account_wants_host) {
                    account_wants_host = false;
                    host_a_lobby();
                }
                if (account_wants_friends) {
                    account_wants_friends = false;
                    show_friends_screen();
                }
                continue;
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
                    static_cast<std::size_t>(platform_choice)].publisher;
            }
        }

        int picked = -1;
        if (!system_menu_requested) {
            const bool everything = !selected_platform;
            const std::string title = everything ? "MANX" : *selected_platform;
            picked = browse_library_grid(
                menu, choices, banners, banner_pixels, title,
                [&](const rom_choice& choice) {
                    return everything ||
                           publisher_of(choice) == *selected_platform;
                }, &play_style, interrupt,
                everything ? "System Menu" : "Platforms",
                refresh_status);
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
            // Back out of the whole library goes to the system menu, because
            // there is nothing above it any more. Back out of one publisher
            // returns to the whole library, which is where it came from.
            if (!selected_platform) {
                system_menu_requested = true;
            } else {
                selected_platform.reset();
                browse_everything = false;
                play_style = 0;
                continue;
            }
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
                    // Nobody has been invited yet here, so the question is
                    // whether a machine that is answering has the game - not
                    // whether every machine that accepted an invitation has
                    // it, which is a question with no machines in it and
                    // answered NOT ON THE OTHERS every single time.
                    const bool peer_ready = peer_connected && identity &&
                        lobby->somebody_has_game(identity->short_name);
                    // A game that cannot be played across two machines is
                    // not a game the others are missing. Saying so where it
                    // is true keeps NOT ON THE OTHERS meaning one thing.
                    const bool game_can_network = caps.network_two_player;
                    // Three ways to play, always the same three, in the
                    // same order. Which one is missing used to depend on
                    // the game's metadata, whether the LAN was switched on
                    // and whether another cabinet happened to be answering
                    // at that moment - so the menu changed shape under
                    // somebody who had learnt it. They are all here every
                    // time; the one you cannot use says why.
                    std::vector<launcher_menu::mode_card> how;
                    std::vector<int> how_style;
                    how.push_back({"Single Player", "One player, this cabinet",
                                   launcher_menu::mode_icon::solo});
                    how_style.push_back(0);

                    // Two people, one cabinet - however this particular
                    // board did it.
                    int local_style = -1;
                    const char* local_detail = "";
                    if (caps.multiplayer ==
                            arcade_multiplayer_mode::alternating) {
                        local_style = 1;
                        local_detail = "Take turns on this cabinet";
                    } else if (caps.multiplayer ==
                                   arcade_multiplayer_mode::simultaneous) {
                        local_style = 2;
                        local_detail = "Play together on this cabinet";
                    } else if (caps.network_two_player) {
                        local_style = 3;
                        local_detail = "A separate view for each player";
                    }
                    how.push_back({"Local Two Player",
                                   local_style >= 0 ? local_detail
                                                    : "This game is one "
                                                      "player only",
                                   launcher_menu::mode_icon::local_players,
                                   0, local_style < 0,
                                   local_style < 0 ? "ONE PLAYER" : ""});
                    how_style.push_back(local_style);

                    // One entry for playing somebody else, wherever they
                    // are. Nothing is hosted, joined or created by hand:
                    // picking this opens a lobby for this game - or steps
                    // into one that is already open for it - and waits.
                    // Another cabinet on this network that already has the
                    // game and is answering skips all of that and goes
                    // straight across.
                    const bool signed_in = online && online->signed_in();
                    const bool direct_now = peer_connected && peer_ready &&
                                            game_can_network;
                    const int online_style =
                        !game_can_network ? -1 :
                        direct_now        ?  5 :
                        signed_in         ?  6 : -1;
                    const char* online_badge =
                        !game_can_network ? "NOT THIS GAME" :
                        direct_now        ? "READY" :
                        signed_in         ? "" : "SIGN IN FIRST";
                    how.push_back({"Head to Head",
                                   !game_can_network
                                       ? "This game cannot be played across "
                                         "two machines"
                                       : direct_now
                                             ? "The other cabinet here is "
                                               "ready"
                                             : signed_in
                                                   ? "Waits here until "
                                                     "somebody joins you"
                                                   : "Sign in from the "
                                                     "corner to play online",
                                   launcher_menu::mode_icon::network,
                                   0, online_style < 0, online_badge});
                    how_style.push_back(online_style);

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
                    if (wanted < 0) {
                        // A card that says why it cannot be used, chosen
                        // anyway. Asking again is the honest response.
                        wanted = -1;
                        continue;
                    }
                }
                if (wanted < 0) continue;   // backed out
                if (wanted == 6) {
                    // Head to head with nobody here yet. There is no lobby
                    // to make and none to find by hand: if somebody already
                    // has one open for this game, this steps into it, and
                    // otherwise it opens one and waits. Either way the
                    // game was chosen first and everything else follows
                    // from it.
                    const auto identity = identify_arcade_game(choice.path);
                    const rom_set_manifest* manifest = identity ?
                        find_supported_rom_set(identity->short_name) :
                        nullptr;
                    if (!identity || !manifest) continue;
                    std::string mode = "simultaneous";
                    if (manifest->multiplayer ==
                            arcade_multiplayer_mode::alternating)
                        mode = "alternating";
                    if (supports_native_system_link(*manifest))
                        mode = "native_link";

                    online->refresh_lobbies();
                    std::string joining;
                    for (const online_lobby& open : online->lobbies()) {
                        if (open.stale || open.game != identity->short_name)
                            continue;
                        if (open.members >= std::max(open.places, 2)) continue;
                        joining = open.id;
                        break;
                    }
                    if (joining.empty())
                        online->create_lobby(true, identity->short_name,
                                             choice.label, 2, mode);
                    else
                        online->join_lobby(joining);

                    // Waiting, and nothing else. The only thing to decide
                    // here is whether to keep waiting.
                    online->set_foreground(true);
                    rom_selection_result started;
                    for (;;) {
                        if (rom_selection_result go = drive_online_session();
                            go.action == rom_selection_action::selected) {
                            started = go;
                            break;
                        }
                        const std::size_t here =
                            std::max<std::size_t>(online->members().size(), 1);
                        const std::string standing =
                            std::to_string(here) + " of 2 machines here";
                        const int said = menu.select_modes(
                            "Waiting for another player",
                            choice.label + "  -  " + standing +
                                ".\nIt starts by itself the moment somebody "
                                "joins.",
                            {launcher_menu::mode_card{
                                "Keep Waiting", standing,
                                launcher_menu::mode_icon::network}},
                            "Stop Waiting", 0,
                            [&, here] {
                                return online->members().size() != here ||
                                       online->lobby_starting() ||
                                       lobby->launch_pending();
                            });
                        if (said == launcher_menu::interrupted) continue;
                        if (said == launcher_menu::exit_requested) {
                            online->leave_lobby();
                            online->set_foreground(false);
                            rom_selection_result exit_result;
                            exit_result.action =
                                rom_selection_action::exit_requested;
                            return exit_result;
                        }
                        if (said < 0) {          // Stop Waiting
                            online->leave_lobby();
                            break;
                        }
                    }
                    online->set_foreground(false);
                    if (started.action == rom_selection_action::selected)
                        return started;
                    continue;               // gave up; back to the games
                }
                if (wanted == 5) {
                    // Across two computers: the partner launches itself.
                    const auto identity = identify_arcade_game(choice.path);
                    if (identity) lobby->launch_game(identity->short_name, 2);
                    run_loading_screen(menu, choice);

                    return {rom_selection_action::selected, choice.path, cabinet_launch_mode::linked_network, 1, false,
                            false, true, {}};
                }
                if (wanted == 0 || wanted == 1 || wanted == 2) {
                    run_loading_screen(menu, choice);

                    return {rom_selection_action::selected, choice.path, cabinet_launch_mode::single, 0, false, false,
                            wanted != 0, {}};
                }
            }
        }

        // Back from platform selection: the system menu. Everything that is
        // not picking a game lives here, including leaving the program.
        // Page 0 is Network Play, which is where a machine that has just
        // agreed to a game needs to be.
        const bool skip_page_menu = enter_network_page;
        enter_network_page = false;
        const int selected_page = skip_page_menu ? 0 : menu.select_modes(
            "MANX",
            lobby && lobby->connected() ?
                "Network player connected - Network Play launches a shared "
                "game across both computers." :
                "System pages. Online Play reaches machines anywhere; "
                "Network Play finds the ones on this network by itself.",
            {{"Online Play",
              online ? online->status_text()
                     : std::string("Not set up on this machine"),
              launcher_menu::mode_icon::network, 0,
              !online || online->state() == online_state::disabled,
              !online ? "OFF"
                      : (online->signed_in() ? "ONLINE" : "OFFLINE")},
             {"Friends",
              online && online->signed_in()
                  ? std::string("Who is about, here and online")
                  : std::string("Sign this cabinet in to see friends"),
              launcher_menu::mode_icon::local_players, 0, !online},
             {"Network Play",
              !lan_on
                  ? std::string("Off - press to look for cabinets on this "
                                "network")
                  : (lobby_connected_at_draw ?
                        std::to_string(machines_here) + " machine" +
                            (machines_here == 1 ? "" : "s") + " found" :
                        "No other machine on the network yet"),
              launcher_menu::mode_icon::network, 0,
              // Nothing to do in here on your own: the lobby needs somebody
              // to ask. It lights up by itself the moment one appears. When
              // the whole thing is switched off it stays pressable, because
              // what it does then is switch it on.
              lan_on && !lobby_connected_at_draw,
              !lan_on ? "OFF"
                      : (lobby_connected_at_draw ? "READY" : "SEARCHING")},
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
            selected_page == 8) {
            rom_selection_result exit_result;
            exit_result.action = rom_selection_action::exit_requested;
            return exit_result;
        }
        // The account chip is drawn on this page too, so pressing it has to
        // land somewhere: the same place the first card goes.
        if (selected_page == launcher_menu::shortcut || selected_page == 0) {
            show_online_screen();
            if (online_started.action == rom_selection_action::selected)
                return online_started;
            if (account_wants_host) {
                account_wants_host = false;
                host_a_lobby();
            }
            if (account_wants_friends) {
                account_wants_friends = false;
                show_friends_screen();
            }
            continue;
        }
        if (selected_page < 0) continue;
        if (selected_page == 1) {
            show_friends_screen();
            continue;
        }
        if (selected_page == 2) {
            // The switch lives on the thing it switches. Somebody who wants
            // cabinets on this network found presses Network Play and turns
            // it on; somebody who does not never sees it searching.
            if (lobby && !lobby->lan_discovery()) {
                const int answer = menu.select_modes(
                    "Network Play is off",
                    "This cabinet is not looking for the others on your "
                    "network, and they cannot see it. Online Play is "
                    "separate and works either way - it reaches machines "
                    "anywhere, including one in the next room.",
                    {{"Turn It On", "Find cabinets on this network",
                      launcher_menu::mode_icon::network},
                     {"Leave It Off", "Online Play only",
                      launcher_menu::mode_icon::exit}},
                    "Back", 0, {});
                if (answer == launcher_menu::exit_requested) {
                    rom_selection_result exit_result;
                    exit_result.action = rom_selection_action::exit_requested;
                    return exit_result;
                }
                if (answer != 0) continue;
                emulator_settings settings = load_settings();
                settings.network_play = true;
                save_settings(settings);
                lobby->set_lan_discovery(true);
                lobby->set_installed_games(choices);
                continue;
            }
            if (!lobby) {
                menu.show_text(
                    "Multiplayer Lobby",
                    "Automatic multiplayer discovery is available when "
                    "MANX is opened without a ROM on the command "
                    "line.");
                continue;
            }
            // One lobby screen for every stage of getting a game going:
            // finding machines, asking them, answering them, and choosing
            // what to play. It is drawn as cards rather than sentences so it
            // reads from across a room and drives from a pad, and it redraws
            // itself whenever anything on the network changes rather than
            // waiting for a keypress.
            using card = launcher_menu::mode_card;
            using icon = launcher_menu::mode_icon;
            for (;;) {
                if (lobby->launch_pending()) {
                    rom_selection_result remote = take_remote_launch();
                    if (remote.action == rom_selection_action::selected)
                        return remote;
                }

                const std::vector<lobby_machine> found = lobby->machines();
                const std::optional<lobby_machine> asking =
                    lobby->pending_invitation();
                const bool hosting = lobby->hosting();
                const int my_node = lobby->node();

                // A snapshot of everything the screen is drawn from. The
                // interrupt compares against it, so a change on any machine
                // redraws this screen instead of stranding it.
                const auto state_now = [lobby] {
                    std::string shape;
                    for (const lobby_machine& machine : lobby->machines()) {
                        shape += machine.label();
                        shape += static_cast<char>('0' + machine.node);
                        shape += static_cast<char>(
                            '0' + static_cast<int>(machine.invite));
                        shape += machine.has_log ? 'L' : '-';
                    }
                    return std::make_tuple(
                        shape, lobby->hosting(), lobby->node(),
                        lobby->agreed_count(), lobby->launch_pending(),
                        lobby->search_exhausted());
                };
                const auto snapshot = state_now();
                const std::function<bool()> changed =
                    [lobby, state_now, snapshot] {
                        return state_now() != snapshot;
                    };

                std::vector<card> cards;
                std::vector<int> actions;
                std::vector<uint64_t> targets;
                enum action {
                    act_nothing, act_invite_one, act_invite_all, act_allow,
                    act_deny, act_withdraw, act_logs, act_alternating,
                    act_simultaneous, act_system_link, act_presence,
                    act_online, act_lan_off,
                };
                const auto add = [&](card entry, int what,
                                     uint64_t target = 0) {
                    cards.push_back(std::move(entry));
                    actions.push_back(what);
                    targets.push_back(target);
                };

                std::string title = "Multiplayer Lobby";
                std::string description;

                if (found.empty()) {
                    const bool blocked = lobby->search_exhausted();
                    title = "Multiplayer Lobby - Looking For Machines";
                    description = blocked ?
                        "No other MANX has answered on this network. Open "
                        "MANX on the other computers, check they are all on "
                        "the same network, and if a firewall asks whether to "
                        "allow MANX, answer yes everywhere." :
                        "Open MANX on the other computers. They find each "
                        "other automatically - no address to type, nothing "
                        "to open. This machine is \"" + lobby->local_name() +
                        "\".";
                    add(card(blocked ? "Still Searching" : "Searching",
                             blocked ? "Nothing has answered yet" :
                                       "Looking for other machines",
                             icon::refresh, 0, true), act_nothing);
                } else if (asking) {
                    title = "Multiplayer Lobby - Invitation";
                    description = asking->label() + " (" + asking->address +
                        ") wants to start a multiplayer game. Allowing it "
                        "puts this machine in their game; they choose what "
                        "everyone plays.";
                    add(card("Allow", "Join " + asking->label() + "'s game",
                             icon::local_players), act_allow);
                    add(card("Deny", "Stay on this machine", icon::exit),
                        act_deny);
                } else if (lobby->invitation_refused()) {
                    title = "Multiplayer Lobby - Declined";
                    description = "Nobody accepted. Nothing has been started "
                        "on any machine - ask again, or pick a different "
                        "one.";
                    for (const lobby_machine& machine : found)
                        add(card("Ask " + machine.label(),
                                 machine.invite ==
                                         multiplayer_invite::declined ?
                                     "Said no last time" : machine.address,
                                 icon::network), act_invite_one,
                            machine.nonce);
                    add(card("Leave It", "Back to playing on this machine",
                             icon::exit), act_withdraw);
                } else if (!hosting && my_node >= 2) {
                    title = "Multiplayer Lobby - Player " +
                            std::to_string(my_node);
                    description = "In the game as Player " +
                        std::to_string(my_node) + " of " +
                        std::to_string(lobby->agreed_count()) +
                        ". The host is choosing what everyone plays; this "
                        "machine starts it automatically.";
                    add(card("Waiting", "The host is choosing the game",
                             icon::refresh, 0, true), act_nothing);
                    add(card("Leave", "Drop out of the game", icon::exit),
                        act_withdraw);
                } else if (hosting) {
                    title = "Multiplayer Lobby - You Are The Host";
                    const int agreed = lobby->agreed_count();
                    description = agreed >= 2 ?
                        std::to_string(agreed) + " machines are in. Choose "
                        "how to play and they all start together - or wait "
                        "for more to accept." :
                        "Waiting for an answer. The invitation is on their "
                        "screens now; somebody there has to allow it.";
                    for (const lobby_machine& machine : found) {
                        std::string state = "Not asked";
                        switch (machine.invite) {
                        case multiplayer_invite::inviting:
                        case multiplayer_invite::idle:
                            state = machine.node ? "Asked - no answer yet"
                                                 : "Not asked";
                            break;
                        case multiplayer_invite::accepted:
                            state = machine.node ?
                                "In as Player " + std::to_string(machine.node)
                                : "Accepted - no place left";
                            break;
                        case multiplayer_invite::declined:
                            state = "Declined";
                            break;
                        }
                        add(card(machine.label(), state,
                                 machine.invite ==
                                         multiplayer_invite::accepted ?
                                     icon::local_players : icon::network,
                                 0, true), act_nothing, machine.nonce);
                    }
                    if (agreed >= 2) {
                        add(card("Take Turns",
                                 "A full screen on each computer",
                                 icon::local_players), act_alternating);
                        add(card("Play Together",
                                 "Both players at the same time",
                                 icon::network), act_simultaneous);
                        add(card("Arcade System Link",
                                 "The original cabinet network, up to 8",
                                 icon::linked_cabinets), act_system_link);
                    }
                    add(card("Withdraw", "Cancel and start over", icon::exit),
                        act_withdraw);
                } else {
                    description = "These machines are on the network and "
                        "ready. Asking one to play makes this machine the "
                        "host: you choose the game and everyone who accepts "
                        "starts it together.";
                    for (const lobby_machine& machine : found) {
                        const bool askable =
                            machine.presence == machine_presence::available;
                        add(card("Ask " + machine.label(),
                                 machine.presence ==
                                         machine_presence::in_game ?
                                     "In a game" :
                                 machine.presence ==
                                         machine_presence::unavailable ?
                                     "Not accepting invitations" :
                                     machine.address,
                                 icon::network, 0, !askable),
                            act_invite_one, machine.nonce);
                    }
                    // Somewhere to say "not now" without leaving the lobby.
                    add(card(lobby->presence() ==
                                 machine_presence::unavailable ?
                                 "Accept Invitations" :
                                 "Do Not Disturb",
                             lobby->presence() ==
                                 machine_presence::unavailable ?
                                 "Let other machines ask again" :
                                 "Refuse invitations automatically",
                             icon::controls), act_presence);
                    if (found.size() > 1)
                        add(card("Ask Everyone",
                                 std::to_string(found.size()) +
                                     " machines found",
                                 icon::cabinet_count), act_invite_all);
                }

                // Always available, wherever the negotiation has got to: the
                // moment worth reading a machine's output is the moment it
                // has stopped answering.
                // Machines somewhere else. Once they are found they appear in
                // the list above with everything else, so this is only the
                // way in - pairing this machine to an account, and seeing
                // what the connection is doing.
                if (online && online_link::available())
                    add(card("Internet Play",
                             online->signed_in()
                                 ? (online->joined_lobby().empty()
                                        ? "Signed in - no lobby yet"
                                        : "In lobby " + online->joined_lobby())
                                 : "Play machines on other networks",
                             icon::cabinet_count), act_online);

                add(card("Machine Logs",
                         lobby->has_any_log() ? "Relayed from the network"
                                              : "Nothing relayed yet",
                         icon::audit, 0, !lobby->has_any_log()), act_logs);

                // The way back out of the feature, beside the feature. A
                // cabinet that has no others to find should be able to stop
                // looking without going hunting through Settings for it.
                add(card("Stop Looking",
                         "Turn network play off - Online Play is unaffected",
                         icon::exit), act_lan_off);

                const int picked = menu.select_modes(
                    title, description, cards, "Back", 0,
                    changed);
                if (picked == launcher_menu::exit_requested) {
                    rom_selection_result exit_result;
                    exit_result.action = rom_selection_action::exit_requested;
                    return exit_result;
                }
                if (picked == launcher_menu::interrupted) continue;
                if (picked < 0) {
                    // Leaving the lobby withdraws whatever this machine had
                    // outstanding, so nobody is left waiting on an answer
                    // that is not coming.
                    lobby->cancel_invite();
                    break;
                }
                if (picked >= static_cast<int>(actions.size())) continue;
                const std::size_t slot = static_cast<std::size_t>(picked);

                // Handled here rather than in the switch below, because it
                // is the one action that leaves this screen: a `break` in a
                // case breaks the switch, and the lobby would redraw itself
                // as an empty search for machines it has just stopped
                // looking for.
                if (actions[slot] == act_lan_off) {
                    emulator_settings off = load_settings();
                    off.network_play = false;
                    save_settings(off);
                    lobby->cancel_invite();
                    lobby->set_lan_discovery(false);
                    break;
                }

                bool system_link = false;
                arcade_multiplayer_mode wanted_mode =
                    arcade_multiplayer_mode::simultaneous;
                switch (actions[slot]) {
                case act_nothing:
                    continue;
                case act_online:
                    show_online_screen();
                    if (online_started.action ==
                        rom_selection_action::selected)
                        return online_started;
                    if (account_wants_host) {
                        account_wants_host = false;
                        host_a_lobby();
                    }
                    continue;
                case act_logs:
                    choose_machine_log();
                    continue;
                case act_presence:
                    lobby->set_presence(
                        lobby->presence() == machine_presence::unavailable ?
                            machine_presence::available :
                            machine_presence::unavailable);
                    continue;
                case act_invite_one:
                    lobby->clear_refusal();
                    lobby->invite(targets[slot]);
                    continue;
                case act_invite_all:
                    lobby->invite_everyone();
                    continue;
                case act_allow:
                    lobby->answer_invite(true);
                    continue;
                case act_deny:
                    lobby->answer_invite(false);
                    continue;
                case act_withdraw:
                    lobby->clear_refusal();
                    lobby->cancel_invite();
                    continue;
                case act_alternating:
                    wanted_mode = arcade_multiplayer_mode::alternating;
                    break;
                case act_simultaneous:
                    wanted_mode = arcade_multiplayer_mode::simultaneous;
                    break;
                case act_system_link:
                    system_link = true;
                    break;
                }

                // How many machines this style can actually use. Lockstep
                // netplay shares one board between two players and cannot be
                // stretched further; the cabinet comm ring is the thing the
                // arcade hardware itself built for a row of machines, so
                // that one takes everybody.
                const int places = system_link ?
                    std::min(lobby->agreed_count(), lobby_max_machines) : 2;

                // Player 1 choosing the game. Only games present on every
                // taking-part machine are offered, so a pick can never leave
                // one of them with nothing to load.
                std::vector<std::size_t> linked_indices;
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
                        !lobby->everyone_has_game(identity->short_name,
                                                  places))
                        continue;
                    linked_indices.push_back(index);
                }
                std::vector<card> game_cards;
                for (std::size_t entry = 0; entry < linked_indices.size();
                     ++entry) {
                    const rom_choice& listed =
                        choices[linked_indices[entry]];
                    game_cards.emplace_back(
                        listed.label,
                        system_link ? "System Link" :
                            wanted_mode ==
                                    arcade_multiplayer_mode::alternating ?
                                "Take turns" : "Play together",
                        system_link ? icon::linked_cabinets :
                            wanted_mode ==
                                    arcade_multiplayer_mode::alternating ?
                                icon::local_players : icon::network);
                    game_cards.back().plain_title = true;
                }
                const int selected = menu.select_modes(
                    system_link ? "Arcade System Link" :
                        wanted_mode == arcade_multiplayer_mode::alternating ?
                            "Take Turns" : "Play Together",
                    game_cards.empty() ?
                        (system_link ?
                            "No System Link game is installed on every "
                            "machine in the session." :
                            "No game of this style is installed on every "
                            "machine. Try the other style, or check the same "
                            "game is present on each.") :
                        "Choose once here. The other machines start the "
                        "same game automatically.",
                    game_cards, "Back to the Lobby", 0,
                    [lobby] { return lobby->node() != 1; });
                if (selected == launcher_menu::exit_requested) {
                    rom_selection_result exit_result;
                    exit_result.action = rom_selection_action::exit_requested;
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
                lobby->launch_game(identity->short_name, places);
                run_loading_screen(menu, choice);

                rom_selection_result host_result{
                    rom_selection_action::selected, choice.path,
                    cabinet_launch_mode::linked_network, 1, false, false,
                    true, {}};
                host_result.cabinet_count = std::max(places, 2);
                return host_result;
            }
            continue;
        }
        if (selected_page == 3) {
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
                    " at once.", counts, "Back", 0);
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
        if (selected_page == 4) {
            show_rom_library_manager(menu);
            choices = discover_rom_choices(current_path);
            continue;
        }
        if (selected_page == 5) {
            show_input_mapper(menu, choices);
            continue;
        }
        if (selected_page == 6) {
            show_eeprom_manager(menu);
            continue;
        }
        if (selected_page == 7) {
            show_high_score_viewer(menu, online);
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
