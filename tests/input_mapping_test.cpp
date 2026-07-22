#include "input_mapping.h"
#include "test_platform.h"

#include <SDL2/SDL.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

int main() {
    namespace fs = std::filesystem;

    const auto& actions = input_action_descriptors();
    if (actions.size() != input_action_count) return 1;
    std::set<std::string> ids;
    for (const input_action_descriptor& action : actions) {
        if (!action.id || !*action.id || !action.label || !*action.label ||
            !ids.emplace(action.id).second)
            return 2;
    }

    input_mapping_config config = default_input_mapping_config();
    const input_binding coin =
        config.keyboard[input_action_index(input_action::coin1)];
    const input_binding start =
        config.keyboard[input_action_index(input_action::start1)];
    if (coin.type != input_binding_type::keyboard ||
        coin.code != SDL_SCANCODE_5 ||
        start.type != input_binding_type::keyboard ||
        start.code != SDL_SCANCODE_1 ||
        config.keyboard[input_action_index(input_action::p1_action3)].code !=
            SDL_SCANCODE_V)
        return 3;

    input_mapping_config time_crisis_config = config;
    if (keyboard_bindings_for(time_crisis_config, "timecris")
            [input_action_index(input_action::p1_action2)].code !=
        SDL_SCANCODE_SPACE)
        return 31;
    game_input_mapping& time_crisis =
        ensure_game_mapping(time_crisis_config, "timecris");
    time_crisis.keyboard[input_action_index(input_action::p1_action2)] = {
        input_binding_type::keyboard, SDL_SCANCODE_R, 0};
    if (keyboard_bindings_for(time_crisis_config, "timecris")
            [input_action_index(input_action::p1_action2)] !=
        time_crisis.keyboard[input_action_index(input_action::p1_action2)])
        return 32;

    controller_input_mapping& controller =
        ensure_controller_mapping(config, "00112233445566778899aabbccddeeff");
    controller.bindings[input_action_index(input_action::coin1)] = {
        input_binding_type::controller_button,
        SDL_CONTROLLER_BUTTON_GUIDE, 0};
    config.keyboard[input_action_index(input_action::p1_action1)] = {
        input_binding_type::keyboard, SDL_SCANCODE_SPACE, 0};

    game_input_mapping& rave = ensure_game_mapping(config, "raverace");
    if (rave.keyboard != inherited_input_bindings()) return 4;
    rave.keyboard[input_action_index(input_action::start1)] = {
        input_binding_type::keyboard, SDL_SCANCODE_F7, 0};
    rave.keyboard[input_action_index(input_action::gas)] = input_binding{};
    controller_input_mapping& rave_controller =
        ensure_game_controller_mapping(
            rave, "00112233445566778899aabbccddeeff");
    rave_controller.bindings[input_action_index(input_action::start1)] = {
        input_binding_type::controller_button, SDL_CONTROLLER_BUTTON_B, 0};

    input_binding_table resolved_keyboard =
        keyboard_bindings_for(config, "raverace");
    if (resolved_keyboard[input_action_index(input_action::start1)] !=
            rave.keyboard[input_action_index(input_action::start1)] ||
        resolved_keyboard[input_action_index(input_action::gas)].type !=
            input_binding_type::none ||
        resolved_keyboard[input_action_index(input_action::coin1)] !=
            config.keyboard[input_action_index(input_action::coin1)])
        return 5;
    input_binding_table resolved_controller = controller_bindings_for(
        config, "00112233445566778899aabbccddeeff", "raverace");
    if (resolved_controller[input_action_index(input_action::coin1)] !=
            controller.bindings[input_action_index(input_action::coin1)] ||
        resolved_controller[input_action_index(input_action::start1)] !=
            rave_controller.bindings[input_action_index(input_action::start1)])
        return 6;

    // Inherited actions are live fallbacks, not copies made when the game
    // profile was first created.
    config.keyboard[input_action_index(input_action::coin1)] = {
        input_binding_type::keyboard, SDL_SCANCODE_F6, 0};
    if (keyboard_bindings_for(config, "raverace")
            [input_action_index(input_action::coin1)] !=
        config.keyboard[input_action_index(input_action::coin1)])
        return 7;

    input_mapping_config copied = config;
    game_input_mapping& moon = ensure_game_mapping(copied, "mooncrst");
    moon.keyboard[input_action_index(input_action::coin1)] = {
        input_binding_type::keyboard, SDL_SCANCODE_M, 0};
    ensure_game_controller_mapping(
        moon, "00112233445566778899aabbccddeeff");
    if (!copy_game_keyboard_mapping(copied, "mooncrst", "raverace") ||
        !copy_game_controller_mapping(
            copied, "mooncrst", "raverace",
            "00112233445566778899aabbccddeeff"))
        return 21;
    if (keyboard_bindings_for(copied, "mooncrst") !=
            keyboard_bindings_for(copied, "raverace") ||
        controller_bindings_for(
            copied, "00112233445566778899aabbccddeeff", "mooncrst") !=
            controller_bindings_for(
                copied, "00112233445566778899aabbccddeeff", "raverace"))
        return 22;
    if (!copy_game_keyboard_mapping(copied, "mooncrst", "") ||
        !copy_game_controller_mapping(
            copied, "mooncrst", "",
            "00112233445566778899aabbccddeeff") ||
        keyboard_bindings_for(copied, "mooncrst") != copied.keyboard ||
        controller_bindings_for(
            copied, "00112233445566778899aabbccddeeff", "mooncrst") !=
            controller_bindings_for(
                copied, "00112233445566778899aabbccddeeff") ||
        copy_game_keyboard_mapping(copied, "mooncrst", "mooncrst") ||
        copy_game_keyboard_mapping(copied, "BadName", "raverace") ||
        copy_game_controller_mapping(
            copied, "mooncrst", "raverace", ""))
        return 23;

    const fs::path root = fs::temp_directory_path() /
        ("whittyarcade-input-test-" + std::to_string(test_process_id()));
    const fs::path path = root / "nested" / "input.ini";
    fs::remove_all(root);
    if (!save_input_mappings(config, path.string())) return 8;
    const input_mapping_config loaded = load_input_mappings(path.string());
    if (loaded.keyboard[input_action_index(input_action::p1_action1)] !=
            config.keyboard[input_action_index(input_action::p1_action1)])
        return 9;
    if (controller_bindings_for(
            loaded, "00112233445566778899aabbccddeeff")
            [input_action_index(input_action::coin1)] !=
        controller.bindings[input_action_index(input_action::coin1)])
        return 10;
    if (keyboard_bindings_for(loaded, "raverace")
            [input_action_index(input_action::start1)] !=
            rave.keyboard[input_action_index(input_action::start1)] ||
        keyboard_bindings_for(loaded, "raverace")
            [input_action_index(input_action::gas)].type !=
            input_binding_type::none ||
        controller_bindings_for(
            loaded, "00112233445566778899aabbccddeeff", "raverace")
            [input_action_index(input_action::start1)] !=
            rave_controller.bindings[input_action_index(input_action::start1)])
        return 11;

    // A game with no profile is exactly the General profile.
    if (keyboard_bindings_for(loaded, "mooncrst") != loaded.keyboard ||
        controller_bindings_for(
            loaded, "00112233445566778899aabbccddeeff", "mooncrst") !=
        controller_bindings_for(
            loaded, "00112233445566778899aabbccddeeff"))
        return 12;

    // Saving an existing file must be atomic and replace the prior content.
    config.keyboard[input_action_index(input_action::coin1)] = {
        input_binding_type::keyboard, SDL_SCANCODE_F5, 0};
    if (!save_input_mappings(config, path.string()) ||
        load_input_mappings(path.string())
                .keyboard[input_action_index(input_action::coin1)] !=
            config.keyboard[input_action_index(input_action::coin1)])
        return 13;

    const fs::path malformed = root / "malformed.ini";
    {
        std::ofstream output(malformed);
        output << "keyboard.coin1=button:2\n"
               << "keyboard.start1=key:99999\n"
               << "keyboard.gas=key:" << SDL_SCANCODE_G << "\n"
               << "keyboard.p1_action3=key:" << SDL_SCANCODE_C << "\n"
               << "controller.aabb.coin1=key:3\n"
               << "controller.aabb.start1=axis:999:1\n"
               << "controller.aabb.gas=axis:4:-1\n"
               << "game.raverace.keyboard.coin1=button:2\n"
               << "game.raverace.keyboard.start1=key:"
               << SDL_SCANCODE_G << "\n"
               << "game.raverace.keyboard.gas=none\n"
               << "game.raverace.controller.aabb.start1=button:"
               << SDL_CONTROLLER_BUTTON_Y << "\n"
               << "game.raverace.controller.aabb.gas=key:3\n"
               << "game.BadName.keyboard.coin1=key:3\n"
               << "unknown.value=key:1\n";
    }
    const input_mapping_config repaired =
        load_input_mappings(malformed.string());
    if (repaired.keyboard[input_action_index(input_action::coin1)].code !=
            SDL_SCANCODE_5 ||
        repaired.keyboard[input_action_index(input_action::start1)].code !=
            SDL_SCANCODE_1 ||
        repaired.keyboard[input_action_index(input_action::gas)].code !=
            SDL_SCANCODE_G ||
        repaired.keyboard[input_action_index(input_action::p1_action3)].code !=
            SDL_SCANCODE_V)
        return 14;
    const input_binding_table repaired_controller =
        controller_bindings_for(repaired, "aabb");
    if (repaired_controller[input_action_index(input_action::coin1)].type !=
            input_binding_type::controller_button ||
        repaired_controller[input_action_index(input_action::start1)].type !=
            input_binding_type::controller_button ||
        repaired_controller[input_action_index(input_action::gas)] !=
            input_binding{input_binding_type::controller_axis, 4, -1})
        return 15;
    const input_binding_table repaired_game_keyboard =
        keyboard_bindings_for(repaired, "raverace");
    if (repaired_game_keyboard[input_action_index(input_action::coin1)] !=
            repaired.keyboard[input_action_index(input_action::coin1)] ||
        repaired_game_keyboard[input_action_index(input_action::start1)] !=
            input_binding{input_binding_type::keyboard, SDL_SCANCODE_G, 0} ||
        repaired_game_keyboard[input_action_index(input_action::gas)].type !=
            input_binding_type::none)
        return 16;
    const input_binding_table repaired_game_controller =
        controller_bindings_for(repaired, "aabb", "raverace");
    if (repaired_game_controller[input_action_index(input_action::start1)] !=
            input_binding{input_binding_type::controller_button,
                          SDL_CONTROLLER_BUTTON_Y, 0} ||
        repaired_game_controller[input_action_index(input_action::gas)] !=
            repaired_controller[input_action_index(input_action::gas)])
        return 17;
    if (keyboard_bindings_for(repaired, "BadName") != repaired.keyboard)
        return 18;

    // Existing general-only files remain valid without a migration step.
    const fs::path legacy = root / "legacy.ini";
    {
        std::ofstream output(legacy);
        output << "keyboard.coin1=key:" << SDL_SCANCODE_F8 << "\n"
               << "keyboard.p1_action3=key:" << SDL_SCANCODE_C << "\n"
               << "controller.cafe.start1=button:"
               << SDL_CONTROLLER_BUTTON_A << "\n";
    }
    const input_mapping_config migrated = load_input_mappings(legacy.string());
    if (!migrated.games.empty() ||
        keyboard_bindings_for(migrated, "raverace")
                [input_action_index(input_action::coin1)] !=
            input_binding{input_binding_type::keyboard, SDL_SCANCODE_F8, 0} ||
        controller_bindings_for(migrated, "cafe", "raverace")
                [input_action_index(input_action::start1)] !=
            input_binding{input_binding_type::controller_button,
                          SDL_CONTROLLER_BUTTON_A, 0} ||
        migrated.keyboard[input_action_index(input_action::p1_action3)].code !=
            SDL_SCANCODE_V)
        return 19;

    if (input_binding_name(input_binding{}).empty() ||
        input_binding_name({input_binding_type::keyboard,
                            SDL_SCANCODE_RETURN, 0}).empty() ||
        input_binding_name({input_binding_type::controller_button,
                            SDL_CONTROLLER_BUTTON_A, 0}).empty() ||
        input_binding_name({input_binding_type::controller_axis,
                            SDL_CONTROLLER_AXIS_LEFTX, -1}).empty() ||
        input_binding_name({input_binding_type::inherit, -1, 0}).empty())
        return 20;

    fs::remove_all(root);
    return 0;
}
