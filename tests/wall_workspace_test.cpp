// Choosing which virtual desktop an arcade wall occupies. A wall covers the
// display, so putting it on a free workspace keeps it from burying whatever
// the user had open - and picking an occupied one would do exactly that.
#include "twin_window_layout.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    std::printf("FAIL: %s\n", what.c_str());
    ++failures;
}

// Shaped like hyprctl workspaces -j, trimmed to the two fields that matter.
std::string report(const std::string& entries) {
    return "[" + entries + "]";
}

std::string workspace(int id, int windows) {
    return "{\"id\": " + std::to_string(id) +
           ", \"name\": \"" + std::to_string(id) +
           "\", \"monitor\": \"DP-1\", \"windows\": " +
           std::to_string(windows) + "}";
}


// Reading back what the compositor actually did with our window. This is the
// only honest check that a wall column was placed - asking SDL just repeats
// the size we requested - so a parser that quietly finds nothing puts every
// column back on the pile.
//
// The sample is shaped like real hyprctl output, nested "workspace" object
// and all. That nesting is what broke the first version: "at" and "size" are
// listed before it, so scanning back from "pid" to the nearest brace landed
// inside the workspace and found neither.
void test_compositor_geometry() {
    const std::string report =
        "[{\n"
        "    \"address\": \"0xaaa\",\n"
        "    \"at\": [3698, 48],\n"
        "    \"size\": [1410, 60],\n"
        "    \"workspace\": {\n        \"id\": 2,\n        \"name\": \"2\"\n    },\n"
        "    \"floating\": false,\n"
        "    \"class\": \"other-app\",\n"
        "    \"pid\": 3233285\n"
        "},{\n"
        "    \"address\": \"0xbbb\",\n"
        "    \"at\": [1728, 0],\n"
        "    \"size\": [1664, 1440],\n"
        "    \"workspace\": {\n        \"id\": 4,\n        \"name\": \"4\"\n    },\n"
        "    \"floating\": true,\n"
        "    \"class\": \"WhittyArcade\",\n"
        "    \"pid\": 4242\n"
        "}]";

    int x = -1, y = -1, w = -1, h = -1;
    check(whitty_window::compositor_geometry_of(report, 4242, x, y, w, h),
          "our own window should be found");
    check(x == 1728 && y == 0,
          "position should come from our window, not the one before it");
    check(w == 1664 && h == 1440,
          "size should come from our window, not the one before it");

    // The first entry, to prove the right chunk is picked either way.
    check(whitty_window::compositor_geometry_of(report, 3233285, x, y, w, h),
          "another window should also be found");
    check(x == 3698 && w == 1410, "the first window's own geometry");

    // A window the compositor has never heard of - the normal state for the
    // moments after a window is created, when placing must be deferred.
    check(!whitty_window::compositor_geometry_of(report, 999, x, y, w, h),
          "an unknown window must report unknown, not stale geometry");
    check(!whitty_window::compositor_geometry_of("", 4242, x, y, w, h),
          "an empty report must report unknown");
}

} // namespace

int main() {
    // This machine as it actually was: 1, 2 and 3 in use, so the wall goes
    // to 4 rather than on top of the browser.
    check(whitty_window::spare_workspace_from(
              report(workspace(1, 3) + "," + workspace(2, 6) + "," +
                     workspace(3, 2))) == 4,
          "should pick the first workspace hyprland has never opened");

    // A workspace that exists but holds nothing is as good as a new one, and
    // is preferred because it is lower.
    check(whitty_window::spare_workspace_from(
              report(workspace(1, 3) + "," + workspace(2, 0) + "," +
                     workspace(3, 2))) == 2,
          "an empty existing workspace should be reused");

    // Gaps count: hyprland only reports workspaces it has opened.
    check(whitty_window::spare_workspace_from(
              report(workspace(1, 1) + "," + workspace(3, 1))) == 2,
          "a workspace hyprland never opened is free");

    // Everything in use means stay put rather than displace someone.
    std::string crowded;
    for (int id = 1; id <= 4; ++id) {
        if (!crowded.empty()) crowded += ",";
        crowded += workspace(id, 1);
    }
    check(whitty_window::spare_workspace_from(report(crowded), 4) == 0,
          "a full desktop should leave the wall where it is");

    // No hyprland, no answer - and never a bogus workspace 1 that would drag
    // the user off whatever they were doing.
    check(whitty_window::spare_workspace_from("") == 0,
          "no workspace report means stay put");
    check(whitty_window::spare_workspace_from("[]") == 0,
          "an empty report means stay put");

    test_compositor_geometry();

    if (failures == 0) std::printf("wall workspace tests passed\n");
    return failures == 0 ? 0 : 1;
}
