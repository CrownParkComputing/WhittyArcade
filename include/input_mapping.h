// Persistent logical arcade-control mappings for keyboards and SDL devices.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class input_action : uint8_t {
    coin1,
    coin2,
    start1,
    start2,
    service,
    test,
    steer_left,
    steer_right,
    gas,
    brake,
    shift_down,
    shift_up,
    view1,
    view2,
    view3,
    view4,
    p1_left,
    p1_right,
    p1_up,
    p1_down,
    p1_action1,
    p1_action2,
    p1_action3,
    right_left,
    right_right,
    right_up,
    right_down,
    p2_left,
    p2_right,
    p2_up,
    p2_down,
    p2_action1,
    p2_action2,
    p2_action3,
    count,
};

constexpr std::size_t input_action_count =
    static_cast<std::size_t>(input_action::count);

struct input_action_descriptor {
    input_action action;
    const char* id;
    const char* label;
};

enum class input_binding_type : uint8_t {
    none,
    keyboard,
    controller_button,
    controller_axis,
    // Per-game profiles use this sentinel to follow the corresponding
    // General mapping. It is never used by the live input adapter.
    inherit,
};

struct input_binding {
    input_binding_type type{input_binding_type::none};
    int code{-1};
    int direction{0};
};

bool operator==(const input_binding& left,
                const input_binding& right) noexcept;
bool operator!=(const input_binding& left,
                const input_binding& right) noexcept;

using input_binding_table =
    std::array<input_binding, input_action_count>;

struct controller_input_mapping {
    std::string guid;
    input_binding_table bindings{};
};

struct game_input_mapping {
    std::string short_name;
    input_binding_table keyboard{};
    std::vector<controller_input_mapping> controllers;
};

struct input_mapping_config {
    input_binding_table keyboard{};
    std::vector<controller_input_mapping> controllers;
    std::vector<game_input_mapping> games;
};

const std::array<input_action_descriptor, input_action_count>&
input_action_descriptors();
std::size_t input_action_index(input_action action) noexcept;

input_binding_table default_keyboard_bindings();
input_binding_table default_controller_bindings();
// Returns a short description when a scancode belongs to the application
// shell rather than an emulated cabinet. Reserved keys cannot be captured as
// gameplay mappings because the host consumes them first.
std::string_view reserved_host_key_purpose(int scancode) noexcept;
// Stock left-stick directions also accept the matching D-pad button. The
// alias is returned only while `primary` is still the built-in binding, so a
// user remap or explicit clear remains authoritative.
input_binding default_controller_alias(input_action action,
                                       const input_binding& primary);
input_binding_table inherited_input_bindings();
input_mapping_config default_input_mapping_config();

std::string input_mapping_path();
input_mapping_config load_input_mappings();
input_mapping_config load_input_mappings(const std::string& path);
bool save_input_mappings(const input_mapping_config& config);
bool save_input_mappings(const input_mapping_config& config,
                         const std::string& path);

controller_input_mapping& ensure_controller_mapping(
    input_mapping_config& config, std::string_view guid);
game_input_mapping& ensure_game_mapping(
    input_mapping_config& config, std::string_view short_name);
controller_input_mapping& ensure_game_controller_mapping(
    game_input_mapping& game, std::string_view guid);
bool copy_game_keyboard_mapping(input_mapping_config& config,
                                std::string_view target_short_name,
                                std::string_view source_short_name);
bool copy_game_controller_mapping(input_mapping_config& config,
                                  std::string_view target_short_name,
                                  std::string_view source_short_name,
                                  std::string_view guid);

input_binding_table keyboard_bindings_for(
    const input_mapping_config& config, std::string_view game_short_name);
input_binding_table controller_bindings_for(
    const input_mapping_config& config, std::string_view guid);
input_binding_table controller_bindings_for(
    const input_mapping_config& config, std::string_view guid,
    std::string_view game_short_name);

std::string input_binding_name(const input_binding& binding);
