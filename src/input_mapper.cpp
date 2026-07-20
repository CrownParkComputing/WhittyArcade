#include "input_mapper.h"

#include "input_mapping.h"
#include "launcher_menu.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
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

void edit_device(launcher_menu& menu, input_mapping_config& config,
                 const std::string& title, bool keyboard,
                 const launcher_controller_info* controller) {
    input_binding_table* bindings = nullptr;
    if (keyboard) {
        bindings = &config.keyboard;
    } else if (controller) {
        bindings = &ensure_controller_mapping(config, controller->guid).bindings;
    } else {
        return;
    }

    int selected_row = 0;
    for (;;) {
        std::vector<std::string> rows;
        rows.reserve(input_action_count + 1);
        for (const input_action_descriptor& action :
             input_action_descriptors()) {
            rows.emplace_back(std::string(action.label) + "  :  " +
                input_binding_name(
                    (*bindings)[input_action_index(action.action)]));
        }
        rows.emplace_back("Reset this device to defaults");

        const std::string description = keyboard ?
            "Choose an arcade action, then press its keyboard key. Delete or "
            "Backspace clears a mapping." :
            "Choose an arcade action, then press a button or move an axis on "
            "this controller. Delete or Backspace clears a mapping.";
        const int chosen = menu.select(title, description, rows,
                                       "Back to Devices", selected_row);
        if (chosen < 0) return;
        selected_row = chosen;

        if (chosen == static_cast<int>(input_action_count)) {
            const int confirmed = menu.select(
                "Reset " + title,
                "Replace every mapping for this device with WhittyArcade's "
                "defaults?",
                {"Reset all mappings"}, "Cancel", -1);
            if (confirmed != 0) continue;
            const input_binding_table previous = *bindings;
            *bindings = keyboard ? default_keyboard_bindings() :
                                   default_controller_bindings();
            save_or_report(menu, config, previous, *bindings);
            continue;
        }
        if (chosen >= static_cast<int>(input_action_count)) continue;

        const input_action_descriptor& action =
            input_action_descriptors()[static_cast<std::size_t>(chosen)];
        const std::string capture_description =
            std::string("Mapping: ") + action.label +
            "\n\nEscape cancels without changing the current mapping.";
        const std::optional<input_binding> captured = menu.capture_binding(
            "Map " + title, capture_description, keyboard,
            controller ? controller->instance_id : -1);
        if (!captured) continue;

        const input_binding_table previous = *bindings;
        (*bindings)[input_action_index(action.action)] = *captured;
        save_or_report(menu, config, previous, *bindings);
    }
}
} // namespace

void show_input_mapper(launcher_menu& menu) {
    input_mapping_config config = load_input_mappings();
    for (;;) {
        const std::vector<launcher_controller_info> controllers =
            menu.controllers();
        std::vector<std::string> devices{"Keyboard"};
        devices.reserve(controllers.size() + 2);
        for (std::size_t index = 0; index < controllers.size(); ++index)
            devices.push_back(controller_label(controllers[index], index + 1));
        devices.emplace_back("Refresh plugged-in devices");

        std::string description =
            "Choose the keyboard or a plugged-in SDL controller. Mappings "
            "apply to every supported arcade board and are saved per "
            "controller model.";
        if (controllers.empty())
            description += " No SDL game controller is currently detected.";

        const int selected = menu.select(
            "Controllers / Keyboard", description, devices,
            "Back to Main Menu");
        if (selected < 0) return;
        if (selected == 0) {
            edit_device(menu, config, "Keyboard", true, nullptr);
            continue;
        }
        const std::size_t controller_index =
            static_cast<std::size_t>(selected - 1);
        if (controller_index < controllers.size()) {
            edit_device(menu, config, controllers[controller_index].name,
                        false, &controllers[controller_index]);
        }
        // The final row is Refresh; rebuilding this page enumerates SDL again.
    }
}
