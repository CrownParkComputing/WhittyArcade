// launcher_menu.h - shared vertical menu used before an arcade board starts.
#pragma once

#include "input_mapping.h"

#include <cstdint>
#include <functional>
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
    static constexpr int interrupted = -2;
    // Compact utility mode is used for menus shown over a running game.
    explicit launcher_menu(bool compact_utility_window = false);
    ~launcher_menu();

    launcher_menu(const launcher_menu&) = delete;
    launcher_menu& operator=(const launcher_menu&) = delete;

    // Returns the selected item index, or -1 for Back/window close.
    int select(const std::string& title, const std::string& description,
               const std::vector<std::string>& items,
               const std::string& back_label = "Back",
               int initial_selection = 0);

    // As select(), but returns interrupted when the background condition
    // becomes true. Used by the multiplayer lobby so Player 2 launches
    // without touching the second app again.
    int select_interruptible(
        const std::string& title, const std::string& description,
        const std::vector<std::string>& items,
        const std::string& back_label, int initial_selection,
        std::function<bool()> interrupt);

    // Displays scrollable text using the same launcher presentation.
    void show_text(const std::string& title, const std::string& text,
                   const std::string& back_label = "Back");

    // Returns controllers currently recognised by SDL's GameController API.
    std::vector<launcher_controller_info> controllers();

    // Waits for one key/button/axis movement. An empty optional means cancel;
    // an input_binding of type none means Clear. When allow_inherit is true,
    // Backspace returns an inherit binding for a per-game profile.
    std::optional<input_binding> capture_binding(
        const std::string& title, const std::string& description,
        bool keyboard, int32_t controller_instance = -1,
        bool allow_inherit = false);

private:
    struct implementation;
    std::unique_ptr<implementation> m_impl;
};
