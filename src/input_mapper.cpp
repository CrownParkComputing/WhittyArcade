#include "input_mapper.h"

#include "arcade_catalog.h"
#include "input_mapping.h"
#include "launcher_menu.h"

#include <SDL2/SDL_scancode.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
struct mapper_profile {
    std::string short_name;
    std::string display_name;
    arcade_board_type board{arcade_board_type::system22};

    bool general() const { return short_name.empty(); }
};

std::vector<mapper_profile> make_profiles(
    const std::vector<rom_choice>& installed_games);
void copy_device_profile(
    launcher_menu& menu, input_mapping_config& config,
    const std::vector<mapper_profile>& profiles,
    const mapper_profile& target, bool keyboard,
    const launcher_controller_info* controller);

std::string controller_label(const launcher_controller_info& controller,
                             std::size_t number) {
    std::string label = "Controller " + std::to_string(number) + " - " +
                        controller.name;
    if (!controller.guid.empty()) {
        const std::size_t shown = std::min<std::size_t>(8,
                                                        controller.guid.size());
        label += "  [" + controller.guid.substr(0, shown) + "]";
    }
    return label;
}

std::vector<input_action> relevant_actions(std::string_view short_name) {
    if (short_name.empty()) {
        std::vector<input_action> result;
        result.reserve(input_action_count);
        for (const input_action_descriptor& descriptor :
             input_action_descriptors())
            result.push_back(descriptor.action);
        return result;
    }

    std::vector<input_action> result;
    const auto add = [&](std::initializer_list<input_action> values) {
        for (input_action value : values) {
            if (std::find(result.begin(), result.end(), value) == result.end())
                result.push_back(value);
        }
    };
    add({input_action::coin1, input_action::coin2,
         input_action::start1, input_action::start2,
         input_action::service, input_action::test});

    if (short_name == "ridgerac" || short_name == "ridgera2" ||
        short_name == "raverace" || short_name == "acedrive" ||
        short_name == "victlap" || short_name == "ridgeracf" ||
        short_name == "vformula" || short_name == "srallyc") {
        add({input_action::steer_left, input_action::steer_right,
             input_action::gas, input_action::brake,
             input_action::shift_down, input_action::shift_up,
             input_action::view1, input_action::view2,
             input_action::view3, input_action::view4});
    } else if (short_name == "cybrcomm") {
        add({input_action::p1_left, input_action::p1_right,
             input_action::p1_up, input_action::p1_down,
             input_action::right_left, input_action::right_right,
             input_action::right_up, input_action::right_down,
             input_action::shift_down, input_action::shift_up,
             input_action::view1});
    } else if (short_name == "timecris") {
        add({input_action::p1_left, input_action::p1_right,
             input_action::p1_up, input_action::p1_down,
             input_action::p1_action1, input_action::p1_action2});
    } else if (short_name == "vf") {
        add({input_action::p1_left, input_action::p1_right,
             input_action::p1_up, input_action::p1_down,
             input_action::view1, input_action::view2,
             input_action::view3});
    } else if (short_name == "swa") {
        add({input_action::p1_left, input_action::p1_right,
             input_action::p1_up, input_action::p1_down,
             input_action::right_left, input_action::right_right,
             input_action::right_up, input_action::right_down,
             input_action::gas, input_action::view1,
             input_action::view2, input_action::view3});
    } else if (short_name == "wingwar") {
        add({input_action::p1_left, input_action::p1_right,
             input_action::p1_up, input_action::p1_down,
             input_action::gas, input_action::brake,
             input_action::shift_down, input_action::shift_up,
             input_action::view1, input_action::view2,
             input_action::view3, input_action::view4});
    } else if (short_name == "phoenix") {
        add({input_action::p1_left, input_action::p1_right,
             input_action::p1_action1, input_action::p1_action2});
    } else if (short_name == "mooncrst" || short_name == "uniwars") {
        add({input_action::p1_left, input_action::p1_right,
             input_action::p1_action1, input_action::p2_left,
             input_action::p2_right, input_action::p2_action1});
    } else if (short_name == "shinobi4") {
        add({input_action::p1_left, input_action::p1_right,
             input_action::p1_up, input_action::p1_down,
             input_action::p1_action1, input_action::p1_action2,
             input_action::p1_action3, input_action::p2_left,
             input_action::p2_right, input_action::p2_up,
             input_action::p2_down, input_action::p2_action1});
    } else {
        // Unknown future games remain configurable without waiting for a UI
        // update; the runtime still resolves the same stable action IDs.
        for (const input_action_descriptor& descriptor :
             input_action_descriptors())
            add({descriptor.action});
    }
    return result;
}

const input_action_descriptor& descriptor_for(input_action action) {
    return input_action_descriptors()[input_action_index(action)];
}

std::string action_label(const mapper_profile& profile,
                         const input_action_descriptor& action) {
    if (profile.short_name == "vf") {
        if (action.action == input_action::view1) return "Fighter - Punch";
        if (action.action == input_action::view2) return "Fighter - Kick";
        if (action.action == input_action::view3) return "Fighter - Guard";
    } else if (profile.short_name == "cybrcomm") {
        if (action.action == input_action::shift_down)
            return "Weapons - Gun trigger";
        if (action.action == input_action::shift_up)
            return "Weapons - Missile";
    } else if (profile.short_name == "timecris") {
        if (action.action == input_action::p1_left)
            return "Light gun - Aim left";
        if (action.action == input_action::p1_right)
            return "Light gun - Aim right";
        if (action.action == input_action::p1_up)
            return "Light gun - Aim up";
        if (action.action == input_action::p1_down)
            return "Light gun - Aim down";
        if (action.action == input_action::p1_action1)
            return "Light gun - Trigger (mouse left click)";
        if (action.action == input_action::p1_action2)
            return "Foot pedal - Hold to stand (Space; right click reloads)";
    } else if (profile.short_name == "mooncrst" ||
               profile.short_name == "uniwars") {
        if (action.action == input_action::p1_action1)
            return "Player 1 - Fire";
        if (action.action == input_action::p2_action1)
            return "Player 2 - Fire";
    } else if (profile.short_name == "wingwar") {
        if (action.action == input_action::shift_down)
            return "Weapons - Machine gun";
        if (action.action == input_action::shift_up)
            return "Weapons - Missile";
        if (action.action == input_action::brake)
            return "Aircraft - Smoke";
    }
    return action.label;
}

bool save_or_report(launcher_menu& menu, input_mapping_config& config,
                    const input_binding_table& previous,
                    input_binding_table& edited) {
    if (save_input_mappings(config)) return true;
    edited = previous;
    menu.show_text(
        "Could Not Save Controls",
        "WhittyArcade could not write the input mapping file:\n\n" +
            input_mapping_path() +
            "\n\nThe previous mappings have been restored.",
        "Back to Controls");
    return false;
}

enum class control_category : uint8_t {
    cabinet,
    driving,
    shooting,
    player1,
    player2,
};

constexpr std::array<control_category, 5> control_categories{{
    control_category::cabinet,
    control_category::driving,
    control_category::shooting,
    control_category::player1,
    control_category::player2,
}};

const char* category_label(control_category category) {
    switch (category) {
    case control_category::cabinet: return "Cabinet";
    case control_category::driving: return "Driving / Flight";
    case control_category::shooting: return "Shooting / Right Stick";
    case control_category::player1: return "Player 1";
    case control_category::player2: return "Player 2";
    }
    return "Controls";
}

control_category category_for(const mapper_profile& profile,
                              input_action action) {
    if (action <= input_action::test) return control_category::cabinet;
    if (profile.short_name == "vf" && action >= input_action::view1 &&
        action <= input_action::view3)
        return control_category::player1;
    if (profile.short_name == "timecris" &&
        action >= input_action::p1_left &&
        action <= input_action::p1_action2)
        return control_category::shooting;
    const bool weapons_game = profile.short_name == "cybrcomm" ||
                              profile.short_name == "swa" ||
                              profile.short_name == "wingwar";
    if (weapons_game && action >= input_action::shift_down &&
        action <= input_action::view4)
        return control_category::shooting;
    if (action >= input_action::steer_left && action <= input_action::view4)
        return control_category::driving;
    if (action >= input_action::p1_left && action <= input_action::p1_action3)
        return control_category::player1;
    if (action >= input_action::right_left &&
        action <= input_action::right_down)
        return control_category::shooting;
    return control_category::player2;
}

std::vector<input_action> actions_in_category(
        const mapper_profile& profile,
        const std::vector<input_action>& actions,
        control_category category) {
    std::vector<input_action> result;
    for (input_action action : actions)
        if (category_for(profile, action) == category)
            result.push_back(action);
    return result;
}

void edit_control_category(
        launcher_menu& menu, input_mapping_config& config,
        const mapper_profile& profile, const std::string& title,
        bool keyboard, const launcher_controller_info* controller,
        input_binding_table& bindings,
        control_category category,
        const std::vector<input_action>& actions) {
    int selected_row = 0;
    for (;;) {
        std::vector<std::string> rows;
        rows.reserve(actions.size());
        const input_binding_table general_bindings = keyboard ?
            config.keyboard : controller_bindings_for(config,
                                                        controller->guid);
        for (input_action action_id : actions) {
            const input_action_descriptor& action = descriptor_for(action_id);
            const input_binding& configured =
                bindings[input_action_index(action.action)];
            const bool inherited =
                configured.type == input_binding_type::inherit;
            const input_binding& shown = inherited ?
                general_bindings[input_action_index(action.action)] :
                configured;
            rows.emplace_back(action_label(profile, action) + "  :  " +
                              input_binding_name(shown) +
                              (inherited ? "  [General]" : ""));
        }

        std::string description = keyboard ?
            "Choose a control, then press its keyboard key." :
            "Choose a control, then press a button or move an axis on this "
            "controller.";
        if (!profile.general())
            description += " Delete leaves it unmapped for this game; "
                           "Backspace restores the General mapping.";
        else
            description += " Delete or Backspace clears a mapping.";
        const int chosen = menu.select(
            profile.display_name + " - " + title + " - " +
                category_label(category),
            description, rows, "Back to Control Tabs", selected_row);
        if (chosen < 0) return;
        if (chosen >= static_cast<int>(actions.size())) continue;
        selected_row = chosen;

        const input_action_descriptor& action = descriptor_for(
            actions[static_cast<std::size_t>(chosen)]);
        const std::string capture_description =
            "Mapping: " + action_label(profile, action) +
            "\n\nEscape cancels without changing the current mapping.";
        const std::optional<input_binding> captured = menu.capture_binding(
            "Map " + profile.display_name + " - " + title,
            capture_description, keyboard,
            controller ? controller->instance_id : -1, !profile.general());
        if (!captured) continue;
        if (captured->type == input_binding_type::keyboard &&
            captured->code == SDL_SCANCODE_C) {
            menu.show_text(
                "C Is Reserved",
                "C opens the current game's controller mapper while a "
                "cabinet is running. Choose a different gameplay key.",
                "Back to Controls");
            continue;
        }

        const input_binding_table previous = bindings;
        bindings[input_action_index(action.action)] = *captured;
        save_or_report(menu, config, previous, bindings);
    }
}

void edit_device(launcher_menu& menu, input_mapping_config& config,
                 const std::vector<mapper_profile>& profiles,
                 const mapper_profile& profile, const std::string& title,
                 bool keyboard,
                 const launcher_controller_info* controller) {
    const bool per_game = !profile.general();
    input_binding_table* bindings = nullptr;
    if (!per_game && keyboard) {
        bindings = &config.keyboard;
    } else if (!per_game && controller) {
        bindings = &ensure_controller_mapping(config, controller->guid).bindings;
    } else if (keyboard) {
        bindings = &ensure_game_mapping(config, profile.short_name).keyboard;
    } else if (controller) {
        game_input_mapping& game =
            ensure_game_mapping(config, profile.short_name);
        bindings = &ensure_game_controller_mapping(game, controller->guid)
                        .bindings;
    } else {
        return;
    }

    const std::vector<input_action> actions =
        relevant_actions(profile.short_name);
    std::vector<control_category> categories;
    for (control_category category : control_categories) {
        if (!actions_in_category(profile, actions, category).empty())
            categories.push_back(category);
    }

    int selected_tab = 0;
    for (;;) {
        std::vector<std::string> rows;
        rows.reserve(categories.size() + (per_game ? 2 : 1));
        for (control_category category : categories) {
            const std::size_t count =
                actions_in_category(profile, actions, category).size();
            rows.emplace_back(std::string(category_label(category)) + "  (" +
                              std::to_string(count) + ")");
        }
        if (per_game)
            rows.emplace_back(keyboard ?
                "Copy this keyboard from another game..." :
                "Copy this controller from another game...");
        rows.emplace_back(per_game ?
            "Use General mappings for every control" :
            "Reset this device to WhittyArcade defaults");

        const int chosen = menu.select(profile.display_name + " - " + title,
            "Choose a control tab. Each tab contains only one type of arcade "
            "assignment.", rows, "Back to Devices", selected_tab);
        if (chosen < 0) return;
        selected_tab = chosen;

        const int copy_row = static_cast<int>(categories.size());
        const int reset_row = copy_row + (per_game ? 1 : 0);
        if (per_game && chosen == copy_row) {
            copy_device_profile(menu, config, profiles, profile, keyboard,
                                controller);
            continue;
        }
        if (chosen == reset_row) {
            const int confirmed = menu.select(
                (per_game ? "Restore General - " : "Reset ") + title,
                per_game ?
                    "Remove every game-specific override for this device? "
                    "This game will follow its General mappings again." :
                    "Replace every mapping for this device with "
                    "WhittyArcade's defaults?",
                {per_game ? "Use all General mappings" :
                            "Reset all mappings"},
                "Cancel", -1);
            if (confirmed != 0) continue;
            const input_binding_table previous = *bindings;
            *bindings = per_game ? inherited_input_bindings() :
                (keyboard ? default_keyboard_bindings() :
                            default_controller_bindings());
            save_or_report(menu, config, previous, *bindings);
            continue;
        }
        if (chosen >= static_cast<int>(categories.size())) continue;
        const control_category category =
            categories[static_cast<std::size_t>(chosen)];
        edit_control_category(
            menu, config, profile, title, keyboard, controller, *bindings,
            category, actions_in_category(profile, actions, category));
    }
}

std::vector<std::size_t> profiles_on_board(
        const std::vector<mapper_profile>& profiles,
        arcade_board_type board, std::string_view excluded_short_name = {}) {
    std::vector<std::size_t> indices;
    for (std::size_t index = 1; index < profiles.size(); ++index) {
        if (profiles[index].board == board &&
            profiles[index].short_name != excluded_short_name)
            indices.push_back(index);
    }
    return indices;
}

std::optional<std::size_t> select_copy_source(
        launcher_menu& menu, const std::vector<mapper_profile>& profiles,
        const mapper_profile& target) {
    for (;;) {
        std::vector<std::string> rows{
            "General Defaults  [remove all game overrides]"};
        std::vector<arcade_board_type> boards;
        for (const arcade_board_descriptor& board : arcade_boards()) {
            const std::size_t count =
                profiles_on_board(profiles, board.type,
                                  target.short_name).size();
            if (count == 0) continue;
            boards.push_back(board.type);
            rows.emplace_back(std::string(board.display_name) + "  (" +
                              std::to_string(count) + ")");
        }
        const int selected = menu.select(
            "Copy Controls From",
            "Choose General or a board containing an installed source game. "
            "Copying replaces every override for " + target.display_name +
            ".",
            rows, "Cancel Copy");
        if (selected < 0) return std::nullopt;
        if (selected == 0) return std::size_t{0};
        const std::size_t board_index =
            static_cast<std::size_t>(selected - 1);
        if (board_index >= boards.size()) continue;

        const std::vector<std::size_t> candidates = profiles_on_board(
            profiles, boards[board_index], target.short_name);
        std::vector<std::string> game_rows;
        game_rows.reserve(candidates.size());
        for (std::size_t profile_index : candidates)
            game_rows.push_back(profiles[profile_index].display_name);
        const int game = menu.select(
            std::string(arcade_board(boards[board_index]).display_name) +
                " Profiles",
            "Choose the installed game whose keyboard and controller "
            "overrides should be copied.",
            game_rows, "Back to Boards");
        if (game >= 0 && game < static_cast<int>(candidates.size()))
            return candidates[static_cast<std::size_t>(game)];
    }
}

void copy_device_profile(
        launcher_menu& menu, input_mapping_config& config,
        const std::vector<mapper_profile>& profiles,
        const mapper_profile& target, bool keyboard,
        const launcher_controller_info* controller) {
    const std::optional<std::size_t> source_index =
        select_copy_source(menu, profiles, target);
    if (!source_index) return;
    const mapper_profile& source = profiles[*source_index];
    const std::string device_name = keyboard ? "keyboard" :
        (controller ? controller->name : "controller");
    const int confirmed = menu.select(
        "Copy " + device_name + " to " + target.display_name,
        "Replace this device's game-specific mappings with " +
            source.display_name + "? Other devices will not be changed.",
        {"Copy this device"}, "Cancel", -1);
    if (confirmed != 0) return;

    input_binding_table* target_bindings = nullptr;
    input_binding_table previous{};
    bool copied = false;
    if (keyboard) {
        target_bindings =
            &ensure_game_mapping(config, target.short_name).keyboard;
        previous = *target_bindings;
        copied = copy_game_keyboard_mapping(
            config, target.short_name, source.short_name);
    } else if (controller) {
        game_input_mapping& target_game =
            ensure_game_mapping(config, target.short_name);
        target_bindings = &ensure_game_controller_mapping(
            target_game, controller->guid).bindings;
        previous = *target_bindings;
        copied = copy_game_controller_mapping(
            config, target.short_name, source.short_name, controller->guid);
    }
    if (!target_bindings) return;
    if (!copied) {
        *target_bindings = previous;
        menu.show_text(
            "Could Not Copy Controls",
            "The selected device profile could not be copied.",
            "Back to Controls");
        return;
    }
    if (!save_input_mappings(config)) {
        *target_bindings = previous;
        menu.show_text(
            "Could Not Copy Controls",
            "WhittyArcade could not save the copied profile. Existing "
            "mappings have been restored.\n\n" + input_mapping_path(),
            "Back to Controls");
        return;
    }
    menu.show_text(
        "Controls Copied",
        source.display_name + " is now the " + device_name +
            " profile for " + target.display_name + ".",
        "Back to Controls");
}

void edit_profile(launcher_menu& menu, input_mapping_config& config,
                  const std::vector<mapper_profile>& profiles,
                  const mapper_profile& profile,
                  const std::string& back_label = "Back to Games") {
    for (;;) {
        const std::vector<launcher_controller_info> controllers =
            menu.controllers();
        std::vector<std::string> devices{"Keyboard"};
        devices.reserve(controllers.size() + 2);
        for (std::size_t index = 0; index < controllers.size(); ++index)
            devices.push_back(controller_label(controllers[index], index + 1));
        devices.emplace_back("Refresh plugged-in devices");

        std::string description = profile.general() ?
            "These are the fallback controls used by every game unless that "
            "game has an override." :
            "Configure only " + profile.display_name +
            ". Controls marked [General] follow the fallback profile.";
        if (controllers.empty())
            description += " No SDL game controller is currently detected.";

        const int selected = menu.select(
            profile.display_name + " - Devices", description, devices,
            back_label);
        if (selected < 0) return;
        if (selected == 0) {
            edit_device(menu, config, profiles, profile, "Keyboard", true,
                        nullptr);
            continue;
        }
        const std::size_t controller_index =
            static_cast<std::size_t>(selected - 1);
        if (controller_index < controllers.size()) {
            edit_device(menu, config, profiles, profile,
                        controllers[controller_index].name, false,
                        &controllers[controller_index]);
        }
        // The final row is Refresh; rebuilding this page enumerates SDL again.
    }
}

std::vector<mapper_profile> make_profiles(
        const std::vector<rom_choice>& installed_games) {
    std::vector<mapper_profile> profiles{{"", "General Defaults",
                                          arcade_board_type::system22}};
    std::unordered_set<std::string> seen;
    for (const rom_choice& choice : installed_games) {
        const std::optional<arcade_game_identity> identity =
            identify_arcade_game(choice.path);
        if (!identity || !identity->short_name ||
            !seen.emplace(identity->short_name).second)
            continue;
        const rom_set_manifest* manifest =
            find_supported_rom_set(identity->short_name);
        profiles.push_back({identity->short_name,
                            manifest ? manifest->display_name : choice.label,
                            identity->board});
    }
    std::sort(profiles.begin() + 1, profiles.end(),
              [](const mapper_profile& left, const mapper_profile& right) {
                  const std::size_t left_board =
                      arcade_board_index(left.board);
                  const std::size_t right_board =
                      arcade_board_index(right.board);
                  return left_board != right_board ?
                      left_board < right_board :
                      left.display_name < right.display_name;
              });
    return profiles;
}
} // namespace

void show_input_mapper(launcher_menu& menu,
                       const std::vector<rom_choice>& installed_games) {
    input_mapping_config config = load_input_mappings();
    const std::vector<mapper_profile> profiles = make_profiles(installed_games);

    for (;;) {
        std::vector<std::string> board_rows{
            "General Defaults  [fallback for all games]"};
        std::vector<arcade_board_type> boards;
        for (const arcade_board_descriptor& board : arcade_boards()) {
            const std::size_t count =
                profiles_on_board(profiles, board.type).size();
            if (count == 0) continue;
            boards.push_back(board.type);
            board_rows.emplace_back(std::string(board.display_name) + "  (" +
                                    std::to_string(count) + ")");
        }
        const int selected = menu.select(
            "Controllers / Keyboard",
            profiles.size() == 1 ?
                "Set General fallback controls. Installed games will appear "
                "here after they are imported." :
                "Choose General fallback controls or an arcade board. Games "
                "are grouped like the ROM selector.",
            board_rows, "Back to Main Menu");
        if (selected < 0) return;
        if (selected == 0) {
            edit_profile(menu, config, profiles, profiles.front(),
                         "Back to Boards");
            continue;
        }
        const std::size_t selected_board =
            static_cast<std::size_t>(selected - 1);
        if (selected_board >= boards.size()) continue;

        for (;;) {
            const std::vector<std::size_t> game_indices =
                profiles_on_board(profiles, boards[selected_board]);
            std::vector<std::string> game_rows;
            game_rows.reserve(game_indices.size());
            for (std::size_t profile_index : game_indices)
                game_rows.push_back(profiles[profile_index].display_name);
            const int game = menu.select(
                std::string(arcade_board(boards[selected_board]).display_name) +
                    " Controls",
                "Choose an installed game. Only overrides are stored; every "
                "other control continues to follow General.",
                game_rows, "Back to Boards");
            if (game < 0) break;
            if (game < static_cast<int>(game_indices.size())) {
                const std::size_t profile_index =
                    game_indices[static_cast<std::size_t>(game)];
                edit_profile(menu, config, profiles,
                             profiles[profile_index]);
            }
        }
    }
}

void show_game_input_mapper(launcher_menu& menu,
                            const std::vector<rom_choice>& installed_games,
                            const std::string& game_short_name) {
    input_mapping_config config = load_input_mappings();
    std::vector<mapper_profile> profiles = make_profiles(installed_games);
    auto current = std::find_if(
        profiles.begin() + 1, profiles.end(),
        [&game_short_name](const mapper_profile& profile) {
            return profile.short_name == game_short_name;
        });
    if (current == profiles.end()) {
        const rom_set_manifest* manifest =
            find_supported_rom_set(game_short_name);
        if (!manifest) {
            menu.show_text("Controls Unavailable",
                           "The running game has no registered input profile.",
                           "Back to Game");
            return;
        }
        profiles.push_back({manifest->short_name, manifest->display_name,
                            manifest->board});
        current = profiles.end() - 1;
    }
    const std::size_t current_index =
        static_cast<std::size_t>(current - profiles.begin());
    edit_profile(menu, config, profiles, profiles[current_index],
                 "Back to Game");
}
