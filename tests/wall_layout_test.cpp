// Divides a presentation surface into arcade-wall columns. This is the whole
// difference between a wall that is one fullscreen window and a wall that is
// several windows the compositor arranges, so it is worth pinning: a column
// that overlaps its neighbour, stops short of the edge, or collapses to zero
// width is a visible fault on the only screen the user has.
#include "wall_layout.h"

#include <array>
#include <cstdio>
#include <string>

namespace {

// The wall's ceiling: three columns, matching max_panes in the presenter and
// max_wall_columns in main.
constexpr int max_columns = 3;

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
}

// Every column must be at least a pixel wide, none may overlap the next, and
// together they must reach both edges of the surface exactly.
void check_columns_tile_surface(int surface, int wanted) {
    std::array<present_pane, max_columns> panes{};
    const int count =
        layout_wall_panes(surface, wanted, panes.data(), max_columns);
    const std::string label =
        std::to_string(wanted) + " columns across " + std::to_string(surface);
    check(count == wanted, label + ": produced " + std::to_string(count));
    if (count != wanted) return;

    check(panes[0].x == 0, label + ": first column leaves a margin");
    const present_pane& last = panes[static_cast<std::size_t>(count - 1)];
    check(last.x + last.w == surface, label + ": last column misses the edge");
    for (int index = 0; index < count; ++index) {
        const present_pane& pane = panes[static_cast<std::size_t>(index)];
        check(pane.w > 0, label + ": column " + std::to_string(index) +
                              " has no width");
        // Columns are one game each and share the surface equally. The last
        // one takes the rounding remainder, which is under a pixel per
        // column - anything more means the split itself is lopsided.
        check(pane.w >= panes[0].w - count && pane.w <= panes[0].w + count,
              label + ": column " + std::to_string(index) +
                  " is not the same width as the others");
        if (index == 0) continue;
        const present_pane& previous =
            panes[static_cast<std::size_t>(index - 1)];
        // A visible gap, not merely no overlap: butted-up columns read as one
        // smeared picture rather than as separate cabinets.
        check(pane.x > previous.x + previous.w,
              label + ": column " + std::to_string(index) +
                  " is not separated from the one before it");
    }
}


// The wall as it is actually laid out on the desktop: the cabinet being
// played is the big one, the others keep running beside it. Getting this
// wrong is immediately visible - a seam down the display, columns on top of
// each other, or a "focused" column no larger than its neighbours.
void check_wall_columns(int surface, int count, int focused) {
    std::array<wall_column_rect, max_columns> columns{};
    const int produced = layout_wall_columns(surface, count, focused,
                                             columns.data(), max_columns);
    const std::string label = std::to_string(count) + " columns across " +
                              std::to_string(surface) + " playing " +
                              std::to_string(focused);
    check(produced == count, label + ": produced " + std::to_string(produced));
    if (produced != count) return;

    check(columns[0].x == 0, label + ": first column leaves a margin");
    const wall_column_rect& last =
        columns[static_cast<std::size_t>(count - 1)];
    check(last.x + last.width == surface,
          label + ": last column misses the edge");
    for (int index = 0; index < count; ++index) {
        const wall_column_rect& column =
            columns[static_cast<std::size_t>(index)];
        check(column.width > 0,
              label + ": column " + std::to_string(index) + " has no width");
        if (index == 0) continue;
        const wall_column_rect& previous =
            columns[static_cast<std::size_t>(index - 1)];
        check(column.x > previous.x + previous.width,
              label + ": column " + std::to_string(index) +
                  " is not separated from the one before it");
    }
    if (focused < 0 || focused >= count) return;
    // The whole point: the cabinet you are playing is the largest one.
    const int played = columns[static_cast<std::size_t>(focused)].width;
    for (int index = 0; index < count; ++index) {
        if (index == focused) continue;
        check(columns[static_cast<std::size_t>(index)].width < played,
              label + ": column " + std::to_string(index) +
                  " is not smaller than the one being played");
    }
}

} // namespace

int main() {
    // The wall Jon actually runs: three games across one 5120x1440 desktop.
    check_columns_tile_surface(5120, 3);
    check_columns_tile_surface(5120, 2);
    // Widths that do not divide evenly are where a naive split loses or
    // duplicates a pixel column at the right-hand edge.
    check_columns_tile_surface(1921, 3);
    check_columns_tile_surface(1279, 3);
    check_columns_tile_surface(1000, 2);

    // A single column is the whole surface, with no gap taken out of it.
    std::array<present_pane, max_columns> panes{};
    check(layout_wall_panes(1920, 1, panes.data(), 4) == 1,
          "one column should stay one column");
    check(panes[0].x == 0 && panes[0].w == 1920,
          "a single column must span the surface");

    // More games than the surface can hold clamps to capacity rather than
    // writing past the caller's array.
    panes = {};
    check(layout_wall_panes(1920, 9, panes.data(), max_columns) == max_columns,
          "column count must clamp to capacity");

    // Too narrow to divide: better one usable picture than four slivers.
    panes = {};
    check(layout_wall_panes(20, max_columns, panes.data(), max_columns) == 1,
          "a surface too narrow to divide keeps a single column");
    check(panes[0].w == 20, "the fallback column still spans the surface");

    // Fullscreen dual output splits the surface in two, one player each; a
    // windowed single cabinet keeps it whole.
    emulator_settings twin{};
    twin.output = output_mode::dual;
    twin.fullscreen = true;
    check(wanted_wall_panes(twin, max_columns, false) == 2,
          "fullscreen dual output wants two columns");
    twin.fullscreen = false;
    check(wanted_wall_panes(twin, max_columns, false) == 1,
          "windowed dual output stays a single column");

    // A wall column is a window of its own, so its surface is never split -
    // splitting it as well would show each game at a third of a third.
    emulator_settings column{};
    column.wall_count = 3;
    column.wall_slot = 1;
    check(wanted_wall_panes(column, max_columns, false) == 1,
          "a wall column owns its whole window");
    // The menu still takes the surface undivided.
    check(wanted_wall_panes(twin, max_columns, true) == 1,
          "the menu must own the whole surface");


    // The wall on Jon's 5120x1440, played column by played column.
    for (int played = 0; played < 3; ++played)
        check_wall_columns(5120, 3, played);
    for (int played = 0; played < 2; ++played)
        check_wall_columns(5120, 2, played);
    // Widths that do not divide cleanly are where a seam appears.
    check_wall_columns(1921, 3, 1);
    check_wall_columns(1279, 3, 2);
    check_wall_columns(1000, 2, 0);

    // Nothing focused yet: equal columns, so a wall never depends on knowing
    // which cabinet is being played.
    {
        std::array<wall_column_rect, max_columns> columns{};
        check(layout_wall_columns(5120, 3, -1, columns.data(),
                                  max_columns) == 3,
              "an unfocused wall should still lay out");
        check(columns[0].width == columns[1].width,
              "with no focus the columns should be equal");
        check_wall_columns(5120, 3, -1);
        // A focus index outside the wall is treated the same way.
        check_wall_columns(5120, 3, 7);
    }

    // One column is the whole display, with no gap taken out of it.
    {
        std::array<wall_column_rect, max_columns> columns{};
        check(layout_wall_columns(1920, 1, 0, columns.data(),
                                  max_columns) == 1,
              "a single column stays single");
        check(columns[0].x == 0 && columns[0].width == 1920,
              "a single column spans the display");
        // Too narrow to divide: one usable picture beats three slivers.
        check(layout_wall_columns(20, 3, 0, columns.data(), max_columns) == 1,
              "a display too narrow to divide keeps one column");
    }

    if (failures == 0) std::printf("wall layout tests passed\n");
    return failures == 0 ? 0 : 1;
}
