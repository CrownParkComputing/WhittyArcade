// launcher_menu.h - shared vertical menu used before an arcade board starts.
#pragma once

#include "input_mapping.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct launcher_controller_info {
    std::string guid;
    std::string name;
    int32_t instance_id{-1};
};

class launcher_menu {
public:
    launcher_menu();
    ~launcher_menu();

    launcher_menu(const launcher_menu&) = delete;
    launcher_menu& operator=(const launcher_menu&) = delete;

    // Returns the selected item index, or -1 for Back/window close.
    int select(const std::string& title, const std::string& description,
               const std::vector<std::string>& items,
               const std::string& back_label = "Back",
               int initial_selection = 0);

    // Displays scrollable text using the same launcher presentation.
    void show_text(const std::string& title, const std::string& text,
                   const std::string& back_label = "Back");

    // Returns controllers currently recognised by SDL's GameController API.
    std::vector<launcher_controller_info> controllers();

    // Waits for one key/button/axis movement. An empty optional means cancel;
    // an input_binding of type none means the user requested Clear.
    std::optional<input_binding> capture_binding(
        const std::string& title, const std::string& description,
        bool keyboard, int32_t controller_instance = -1);

private:
    struct implementation;
    std::unique_ptr<implementation> m_impl;
};
