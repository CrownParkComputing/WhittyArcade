#include "input_mapping.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

constexpr std::array<input_action_descriptor, input_action_count> actions{{
    {input_action::coin1, "coin1", "Cabinet - Coin 1"},
    {input_action::coin2, "coin2", "Cabinet - Coin 2"},
    {input_action::start1, "start1", "Cabinet - Start 1"},
    {input_action::start2, "start2", "Cabinet - Start 2"},
    {input_action::service, "service", "Cabinet - Service"},
    {input_action::test, "test", "Cabinet - Test"},
    {input_action::steer_left, "steer_left", "Driving - Steer left"},
    {input_action::steer_right, "steer_right", "Driving - Steer right"},
    {input_action::gas, "gas", "Driving - Accelerator"},
    {input_action::brake, "brake", "Driving - Brake"},
    {input_action::shift_down, "shift_down", "Driving - Shift down"},
    {input_action::shift_up, "shift_up", "Driving - Shift up"},
    {input_action::view1, "view1", "Driving - View 1"},
    {input_action::view2, "view2", "Driving - View 2"},
    {input_action::view3, "view3", "Driving - View 3"},
    {input_action::view4, "view4", "Driving - View 4"},
    {input_action::p1_left, "p1_left", "Player 1 - Left"},
    {input_action::p1_right, "p1_right", "Player 1 - Right"},
    {input_action::p1_up, "p1_up", "Player 1 - Up"},
    {input_action::p1_down, "p1_down", "Player 1 - Down"},
    {input_action::p1_action1, "p1_action1", "Player 1 - Action 1"},
    {input_action::p1_action2, "p1_action2", "Player 1 - Action 2"},
    {input_action::p1_action3, "p1_action3", "Player 1 - Action 3"},
    {input_action::right_left, "right_left", "Right stick - Left"},
    {input_action::right_right, "right_right", "Right stick - Right"},
    {input_action::right_up, "right_up", "Right stick - Up"},
    {input_action::right_down, "right_down", "Right stick - Down"},
    {input_action::p2_left, "p2_left", "Player 2 - Left"},
    {input_action::p2_right, "p2_right", "Player 2 - Right"},
    {input_action::p2_up, "p2_up", "Player 2 - Up"},
    {input_action::p2_down, "p2_down", "Player 2 - Down"},
    {input_action::p2_action1, "p2_action1", "Player 2 - Action 1"},
}};

input_binding key(SDL_Scancode scancode) {
    return {input_binding_type::keyboard, static_cast<int>(scancode), 0};
}

input_binding controller_button(SDL_GameControllerButton button) {
    return {input_binding_type::controller_button,
            static_cast<int>(button), 0};
}

input_binding controller_axis(SDL_GameControllerAxis axis, int direction) {
    return {input_binding_type::controller_axis,
            static_cast<int>(axis), direction < 0 ? -1 : 1};
}

fs::path config_root() {
    if (const char* config_home = std::getenv("XDG_CONFIG_HOME"))
        return fs::path(config_home);
    if (const char* user_home = std::getenv("HOME"))
        return fs::path(user_home) / ".config";
    return {};
}

std::optional<int> parse_integer(std::string_view text) {
    int value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
    return value;
}

std::optional<input_binding> parse_binding(std::string_view text) {
    if (text == "none") return input_binding{};
    const std::size_t first = text.find(':');
    if (first == std::string_view::npos) return std::nullopt;
    const std::string_view type = text.substr(0, first);
    const std::size_t second = text.find(':', first + 1);
    const std::string_view code_text = text.substr(
        first + 1, second == std::string_view::npos ?
                       std::string_view::npos : second - first - 1);
    const std::optional<int> code = parse_integer(code_text);
    if (!code || *code < 0) return std::nullopt;
    if (type == "key")
        return input_binding{input_binding_type::keyboard, *code, 0};
    if (type == "button")
        return input_binding{input_binding_type::controller_button, *code, 0};
    if (type != "axis" || second == std::string_view::npos)
        return std::nullopt;
    const std::optional<int> direction =
        parse_integer(text.substr(second + 1));
    if (!direction || (*direction != -1 && *direction != 1))
        return std::nullopt;
    return input_binding{input_binding_type::controller_axis, *code,
                         *direction};
}

std::string serialize_binding(const input_binding& binding) {
    switch (binding.type) {
    case input_binding_type::keyboard:
        return "key:" + std::to_string(binding.code);
    case input_binding_type::controller_button:
        return "button:" + std::to_string(binding.code);
    case input_binding_type::controller_axis:
        return "axis:" + std::to_string(binding.code) + ":" +
               std::to_string(binding.direction < 0 ? -1 : 1);
    case input_binding_type::none:
        return "none";
    }
    return "none";
}

bool valid_keyboard_binding(const input_binding& binding) {
    return binding.type == input_binding_type::none ||
           (binding.type == input_binding_type::keyboard &&
            binding.code >= 0 && binding.code < SDL_NUM_SCANCODES);
}

bool valid_controller_binding(const input_binding& binding) {
    return binding.type == input_binding_type::none ||
           (binding.type == input_binding_type::controller_button &&
            binding.code >= 0 &&
            binding.code < SDL_CONTROLLER_BUTTON_MAX) ||
           (binding.type == input_binding_type::controller_axis &&
            binding.code >= 0 && binding.code < SDL_CONTROLLER_AXIS_MAX &&
            (binding.direction == -1 || binding.direction == 1));
}

const input_action_descriptor* find_action(std::string_view id) {
    const auto found = std::find_if(
        actions.begin(), actions.end(),
        [id](const input_action_descriptor& descriptor) {
            return id == descriptor.id;
        });
    return found == actions.end() ? nullptr : &*found;
}

} // namespace

bool operator==(const input_binding& left,
                const input_binding& right) noexcept {
    return left.type == right.type && left.code == right.code &&
           left.direction == right.direction;
}

bool operator!=(const input_binding& left,
                const input_binding& right) noexcept {
    return !(left == right);
}

const std::array<input_action_descriptor, input_action_count>&
input_action_descriptors() {
    return actions;
}

std::size_t input_action_index(input_action action) noexcept {
    return static_cast<std::size_t>(action);
}

input_binding_table default_keyboard_bindings() {
    input_binding_table bindings{};
    const auto set = [&](input_action action, input_binding binding) {
        bindings[input_action_index(action)] = binding;
    };
    set(input_action::coin1, key(SDL_SCANCODE_5));
    set(input_action::coin2, key(SDL_SCANCODE_6));
    set(input_action::start1, key(SDL_SCANCODE_1));
    set(input_action::start2, key(SDL_SCANCODE_2));
    set(input_action::service, key(SDL_SCANCODE_9));
    set(input_action::test, key(SDL_SCANCODE_F2));
    set(input_action::steer_left, key(SDL_SCANCODE_LEFT));
    set(input_action::steer_right, key(SDL_SCANCODE_RIGHT));
    set(input_action::gas, key(SDL_SCANCODE_UP));
    set(input_action::brake, key(SDL_SCANCODE_DOWN));
    set(input_action::shift_down, key(SDL_SCANCODE_Z));
    set(input_action::shift_up, key(SDL_SCANCODE_X));
    set(input_action::view1, key(SDL_SCANCODE_V));
    set(input_action::view2, key(SDL_SCANCODE_B));
    set(input_action::view3, key(SDL_SCANCODE_N));
    set(input_action::view4, key(SDL_SCANCODE_G));
    set(input_action::p1_left, key(SDL_SCANCODE_A));
    set(input_action::p1_right, key(SDL_SCANCODE_D));
    set(input_action::p1_up, key(SDL_SCANCODE_W));
    set(input_action::p1_down, key(SDL_SCANCODE_S));
    set(input_action::p1_action1, key(SDL_SCANCODE_Z));
    set(input_action::p1_action2, key(SDL_SCANCODE_X));
    set(input_action::p1_action3, key(SDL_SCANCODE_C));
    set(input_action::right_left, key(SDL_SCANCODE_LEFT));
    set(input_action::right_right, key(SDL_SCANCODE_RIGHT));
    set(input_action::right_up, key(SDL_SCANCODE_UP));
    set(input_action::right_down, key(SDL_SCANCODE_DOWN));
    set(input_action::p2_left, key(SDL_SCANCODE_J));
    set(input_action::p2_right, key(SDL_SCANCODE_L));
    set(input_action::p2_up, key(SDL_SCANCODE_I));
    set(input_action::p2_down, key(SDL_SCANCODE_K));
    set(input_action::p2_action1, key(SDL_SCANCODE_U));
    return bindings;
}

input_binding_table default_controller_bindings() {
    input_binding_table bindings{};
    const auto set = [&](input_action action, input_binding binding) {
        bindings[input_action_index(action)] = binding;
    };
    set(input_action::coin1,
        controller_button(SDL_CONTROLLER_BUTTON_BACK));
    set(input_action::start1,
        controller_button(SDL_CONTROLLER_BUTTON_START));
    set(input_action::steer_left,
        controller_axis(SDL_CONTROLLER_AXIS_LEFTX, -1));
    set(input_action::steer_right,
        controller_axis(SDL_CONTROLLER_AXIS_LEFTX, 1));
    set(input_action::gas,
        controller_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1));
    set(input_action::brake,
        controller_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1));
    set(input_action::shift_down,
        controller_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
    set(input_action::shift_up,
        controller_button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
    set(input_action::view1,
        controller_button(SDL_CONTROLLER_BUTTON_B));
    set(input_action::view2,
        controller_button(SDL_CONTROLLER_BUTTON_X));
    set(input_action::view3,
        controller_button(SDL_CONTROLLER_BUTTON_Y));
    set(input_action::view4,
        controller_button(SDL_CONTROLLER_BUTTON_A));
    set(input_action::p1_left,
        controller_axis(SDL_CONTROLLER_AXIS_LEFTX, -1));
    set(input_action::p1_right,
        controller_axis(SDL_CONTROLLER_AXIS_LEFTX, 1));
    set(input_action::p1_up,
        controller_axis(SDL_CONTROLLER_AXIS_LEFTY, -1));
    set(input_action::p1_down,
        controller_axis(SDL_CONTROLLER_AXIS_LEFTY, 1));
    set(input_action::p1_action1,
        controller_button(SDL_CONTROLLER_BUTTON_A));
    set(input_action::p1_action2,
        controller_button(SDL_CONTROLLER_BUTTON_B));
    set(input_action::p1_action3,
        controller_button(SDL_CONTROLLER_BUTTON_X));
    set(input_action::right_left,
        controller_axis(SDL_CONTROLLER_AXIS_RIGHTX, -1));
    set(input_action::right_right,
        controller_axis(SDL_CONTROLLER_AXIS_RIGHTX, 1));
    set(input_action::right_up,
        controller_axis(SDL_CONTROLLER_AXIS_RIGHTY, -1));
    set(input_action::right_down,
        controller_axis(SDL_CONTROLLER_AXIS_RIGHTY, 1));
    return bindings;
}

input_mapping_config default_input_mapping_config() {
    input_mapping_config config;
    config.keyboard = default_keyboard_bindings();
    return config;
}

std::string input_mapping_path() {
    const fs::path root = config_root();
    const fs::path path = root.empty() ?
        fs::path("WhittyArcade-input.ini") :
        root / "WhittyArcade" / "input.ini";
    return path.string();
}

input_mapping_config load_input_mappings() {
    return load_input_mappings(input_mapping_path());
}

input_mapping_config load_input_mappings(const std::string& path) {
    input_mapping_config config = default_input_mapping_config();
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') continue;
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string_view key_name(line.data(), equals);
        const std::string_view binding_text(
            line.data() + equals + 1, line.size() - equals - 1);
        const std::optional<input_binding> binding =
            parse_binding(binding_text);
        if (!binding) continue;

        constexpr std::string_view keyboard_prefix = "keyboard.";
        constexpr std::string_view controller_prefix = "controller.";
        if (key_name.substr(0, keyboard_prefix.size()) == keyboard_prefix) {
            const input_action_descriptor* action =
                find_action(key_name.substr(keyboard_prefix.size()));
            if (action && valid_keyboard_binding(*binding))
                config.keyboard[input_action_index(action->action)] = *binding;
            continue;
        }
        if (key_name.substr(0, controller_prefix.size()) !=
            controller_prefix)
            continue;
        const std::string_view remainder =
            key_name.substr(controller_prefix.size());
        const std::size_t separator = remainder.find('.');
        if (separator == std::string_view::npos || separator == 0) continue;
        const std::string_view guid = remainder.substr(0, separator);
        const input_action_descriptor* action =
            find_action(remainder.substr(separator + 1));
        if (!action || !valid_controller_binding(*binding)) continue;
        controller_input_mapping& controller =
            ensure_controller_mapping(config, guid);
        controller.bindings[input_action_index(action->action)] = *binding;
    }
    return config;
}

bool save_input_mappings(const input_mapping_config& config) {
    return save_input_mappings(config, input_mapping_path());
}

bool save_input_mappings(const input_mapping_config& config,
                         const std::string& path_text) {
    const fs::path path(path_text);
    std::error_code error;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), error);
    if (error) return false;

    fs::path temporary = path;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) return false;
    output << "# WhittyArcade input mappings. Edit through the launcher.\n";
    for (const input_action_descriptor& action : actions) {
        output << "keyboard." << action.id << '='
               << serialize_binding(
                      config.keyboard[input_action_index(action.action)])
               << '\n';
    }
    for (const controller_input_mapping& controller : config.controllers) {
        if (controller.guid.empty() ||
            controller.guid.find('.') != std::string::npos)
            continue;
        for (const input_action_descriptor& action : actions) {
            output << "controller." << controller.guid << '.' << action.id
                   << '='
                   << serialize_binding(controller.bindings[
                          input_action_index(action.action)])
                   << '\n';
        }
    }
    output.close();
    if (!output) {
        fs::remove(temporary, error);
        return false;
    }
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(temporary, error);
        return false;
    }
    return true;
}

controller_input_mapping& ensure_controller_mapping(
    input_mapping_config& config, std::string_view guid) {
    const auto found = std::find_if(
        config.controllers.begin(), config.controllers.end(),
        [guid](const controller_input_mapping& controller) {
            return controller.guid == guid;
        });
    if (found != config.controllers.end()) return *found;
    config.controllers.push_back(
        {std::string(guid), default_controller_bindings()});
    return config.controllers.back();
}

input_binding_table controller_bindings_for(
    const input_mapping_config& config, std::string_view guid) {
    const auto found = std::find_if(
        config.controllers.begin(), config.controllers.end(),
        [guid](const controller_input_mapping& controller) {
            return controller.guid == guid;
        });
    return found == config.controllers.end() ?
        default_controller_bindings() : found->bindings;
}

std::string input_binding_name(const input_binding& binding) {
    switch (binding.type) {
    case input_binding_type::none:
        return "Not mapped";
    case input_binding_type::keyboard: {
        if (binding.code < 0 || binding.code >= SDL_NUM_SCANCODES)
            return "Unknown key";
        const char* name = SDL_GetScancodeName(
            static_cast<SDL_Scancode>(binding.code));
        return std::string("Key ") + (name && *name ? name : "Unknown");
    }
    case input_binding_type::controller_button: {
        if (binding.code < 0 || binding.code >= SDL_CONTROLLER_BUTTON_MAX)
            return "Unknown button";
        const char* name = SDL_GameControllerGetStringForButton(
            static_cast<SDL_GameControllerButton>(binding.code));
        return std::string("Button ") + (name && *name ? name : "Unknown");
    }
    case input_binding_type::controller_axis: {
        if (binding.code < 0 || binding.code >= SDL_CONTROLLER_AXIS_MAX)
            return "Unknown axis";
        const char* name = SDL_GameControllerGetStringForAxis(
            static_cast<SDL_GameControllerAxis>(binding.code));
        return std::string("Axis ") + (name && *name ? name : "Unknown") +
               (binding.direction < 0 ? " -" : " +");
    }
    }
    return "Not mapped";
}
