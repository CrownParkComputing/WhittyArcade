#include "input_mapping.h"

#include <SDL2/SDL.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <unistd.h>

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
        start.code != SDL_SCANCODE_1)
        return 3;

    controller_input_mapping& controller =
        ensure_controller_mapping(config, "00112233445566778899aabbccddeeff");
    controller.bindings[input_action_index(input_action::coin1)] = {
        input_binding_type::controller_button,
        SDL_CONTROLLER_BUTTON_GUIDE, 0};
    config.keyboard[input_action_index(input_action::p1_action1)] = {
        input_binding_type::keyboard, SDL_SCANCODE_SPACE, 0};

    const fs::path root = fs::temp_directory_path() /
        ("whittyarcade-input-test-" + std::to_string(getpid()));
    const fs::path path = root / "nested" / "input.ini";
    fs::remove_all(root);
    if (!save_input_mappings(config, path.string())) return 4;
    const input_mapping_config loaded = load_input_mappings(path.string());
    if (loaded.keyboard[input_action_index(input_action::p1_action1)] !=
            config.keyboard[input_action_index(input_action::p1_action1)])
        return 5;
    if (controller_bindings_for(
            loaded, "00112233445566778899aabbccddeeff")
            [input_action_index(input_action::coin1)] !=
        controller.bindings[input_action_index(input_action::coin1)])
        return 6;

    // Saving an existing file must be atomic and replace the prior content.
    config.keyboard[input_action_index(input_action::coin1)] = {
        input_binding_type::keyboard, SDL_SCANCODE_F5, 0};
    if (!save_input_mappings(config, path.string()) ||
        load_input_mappings(path.string())
                .keyboard[input_action_index(input_action::coin1)] !=
            config.keyboard[input_action_index(input_action::coin1)])
        return 7;

    const fs::path malformed = root / "malformed.ini";
    {
        std::ofstream output(malformed);
        output << "keyboard.coin1=button:2\n"
               << "keyboard.start1=key:99999\n"
               << "keyboard.gas=key:" << SDL_SCANCODE_G << "\n"
               << "controller.aabb.coin1=key:3\n"
               << "controller.aabb.start1=axis:999:1\n"
               << "controller.aabb.gas=axis:4:-1\n"
               << "unknown.value=key:1\n";
    }
    const input_mapping_config repaired =
        load_input_mappings(malformed.string());
    if (repaired.keyboard[input_action_index(input_action::coin1)].code !=
            SDL_SCANCODE_5 ||
        repaired.keyboard[input_action_index(input_action::start1)].code !=
            SDL_SCANCODE_1 ||
        repaired.keyboard[input_action_index(input_action::gas)].code !=
            SDL_SCANCODE_G)
        return 8;
    const input_binding_table repaired_controller =
        controller_bindings_for(repaired, "aabb");
    if (repaired_controller[input_action_index(input_action::coin1)].type !=
            input_binding_type::controller_button ||
        repaired_controller[input_action_index(input_action::start1)].type !=
            input_binding_type::controller_button ||
        repaired_controller[input_action_index(input_action::gas)] !=
            input_binding{input_binding_type::controller_axis, 4, -1})
        return 9;

    if (input_binding_name(input_binding{}).empty() ||
        input_binding_name({input_binding_type::keyboard,
                            SDL_SCANCODE_RETURN, 0}).empty() ||
        input_binding_name({input_binding_type::controller_button,
                            SDL_CONTROLLER_BUTTON_A, 0}).empty() ||
        input_binding_name({input_binding_type::controller_axis,
                            SDL_CONTROLLER_AXIS_LEFTX, -1}).empty())
        return 10;

    fs::remove_all(root);
    return 0;
}
