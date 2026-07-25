// wall_layout.h - how one presentation surface is divided into game columns.
#pragma once

#include "arcade_settings.h"

#include <algorithm>

// One column of the presentation surface, in surface pixels.
struct present_pane {
    int x{};
    int w{};
};

// How many columns the surface should be divided into. Fullscreen dual output
// asks for two - one player each - and everything else is a single pane
// spanning the whole surface. The menu takes the surface back undivided,
// because it belongs to the host rather than to any one player.
//
// An arcade wall is deliberately absent here: each of its games runs in its
// own process and its own window, tiled across the display, because three
// boards cannot share one renderer's texture sheets, palettes and vertex
// buffers - they would overwrite each other's every frame.
inline int wanted_wall_panes(const emulator_settings& settings, int capacity,
                             bool menu_visible) {
    if (menu_visible) return 1;
    const bool dual_fullscreen =
        settings.output == output_mode::dual && settings.fullscreen;
    return dual_fullscreen && capacity >= 2 ? 2 : 1;
}

// Divides the surface into equally wide columns separated by a gap, the last
// column absorbing the rounding remainder so the columns always span the full
// width exactly. One column is the whole surface.
//
// This is the whole of what makes an arcade wall a wall: the columns are
// regions of a single surface, so the number of games on screen owes nothing
// to how many windows a compositor would grant - which is why a wall is one
// fullscreen window and not one window per game.
//
// Returns the number of columns written to out.
inline int layout_wall_panes(int surface_width, int wanted, present_pane* out,
                             int capacity) {
    if (!out || capacity < 1 || surface_width < 1) return 0;
    const int count = std::clamp(wanted, 1, capacity);
    if (count == 1) {
        out[0] = {0, surface_width};
        return 1;
    }
    const int gap = std::max(surface_width / 80, 12);
    // A gap per join must still leave a pixel of picture per column; a surface
    // too narrow to divide keeps its single pane rather than emitting columns
    // of zero or negative width.
    if (surface_width - gap * (count - 1) < count) {
        out[0] = {0, surface_width};
        return 1;
    }
    const int column = (surface_width - gap * (count - 1)) / count;
    for (int pane = 0; pane < count; ++pane) {
        const int x = pane * (column + gap);
        out[pane] = {x, pane == count - 1 ? surface_width - x : column};
    }
    return count;
}

// The desktop rectangle for one column of an arcade wall.
struct wall_column_rect {
    int x{};
    int width{};
};

// Lays the wall's columns across the display with the one being played given
// the larger share: it is the cabinet you are at, and the others are running
// their attract modes for you to glance at.
//
// The focused column takes 2 shares and each of the others 1, so three
// columns split the display 50/25/25 and two split it 66/33. focused < 0 -
// nothing has focus yet, or the compositor cannot tell us - falls back to
// equal columns, so a wall never depends on knowing who is being played.
//
// The last column absorbs the rounding remainder, so the columns always span
// the full width exactly. Returns the number written to out.
inline int layout_wall_columns(int surface_width, int count, int focused,
                               wall_column_rect* out, int capacity) {
    if (!out || capacity < 1 || surface_width < 1 || count < 1) return 0;
    const int columns = std::min(count, capacity);
    if (columns == 1) {
        out[0] = {0, surface_width};
        return 1;
    }
    const int gap = std::max(surface_width / 80, 12);
    const int usable = surface_width - gap * (columns - 1);
    // Every column needs a pixel at minimum, and the focused one two shares
    // of it; a display too narrow to divide keeps a single column.
    const int shares = columns + 1;
    if (usable < shares) {
        out[0] = {0, surface_width};
        return 1;
    }
    const bool hero = focused >= 0 && focused < columns;
    const int unit = hero ? usable / shares : usable / columns;
    int x = 0;
    for (int column = 0; column < columns; ++column) {
        const int width = hero && column == focused ? unit * 2 : unit;
        out[column] = {x, std::max(width, 1)};
        x += out[column].width + gap;
    }
    // The rightmost column runs to the edge, taking whatever the division
    // left behind rather than leaving a seam down the side of the display.
    wall_column_rect& last = out[columns - 1];
    last.width = std::max(surface_width - last.x, 1);
    return columns;
}
