// operator_menu.h - board-neutral in-game operator menu model.
#pragma once

#include <string>
#include <vector>

struct operator_menu_row {
    int id{};
    std::string label;
    std::vector<std::string> values;
    int selected{};
    bool action{};
    bool enabled{true};
};

struct operator_menu_definition {
    std::string title{"OPERATOR / DIP SETTINGS"};
    std::string description;
    std::vector<operator_menu_row> rows;
};

struct operator_menu_action {
    int row_id{};
    int selected{};
};
