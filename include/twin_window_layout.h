// Cross-desktop placement for the windowed Twin Display mode.
#pragma once

#include <SDL3/SDL.h>

#include "wall_layout.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace whitty_window {

struct twin_layout {
    int first_x{};
    int second_x{};
    int y{};
    int width{};
    int height{};
};

inline twin_layout fit_twin_layout(const SDL_Rect& usable,
                                   int requested_width) {
    const int gap = std::clamp(usable.w / 100, 16, 48);
    const int available_each = std::max((usable.w - gap) / 2, 160);
    const int height_limited_width = std::max(160, usable.h * 4 / 3);
    const int width = std::clamp(
        requested_width, 160, std::min(available_each, height_limited_width));
    const int height = std::max(width * 3 / 4, 120);
    const int pair_width = width * 2 + gap;
    const int first_x = usable.x + std::max((usable.w - pair_width) / 2, 0);
    return {
        first_x,
        first_x + width + gap,
        usable.y + std::max((usable.h - height) / 2, 0),
        width,
        height,
    };
}

inline void set_exact_size(SDL_Window* window, int width, int height) {
    if (!window) return;
    SDL_SetWindowMinimumSize(window, width, height);
    SDL_SetWindowMaximumSize(window, width, height);
    SDL_SetWindowSize(window, width, height);
}

#if defined(__linux__)
// Runs one hyprctl dispatch command, waiting for it and reporting whether it
// exited cleanly. Output is discarded: these run from a spawned game process
// with no terminal to write to.
inline bool run_hyprctl_dispatch(const std::vector<std::string>& arguments) {
    std::vector<const char*> argv;
    argv.reserve(arguments.size() + 3);
    argv.push_back("hyprctl");
    argv.push_back("dispatch");
    for (const std::string& argument : arguments)
        argv.push_back(argument.c_str());
    argv.push_back(nullptr);
    pid_t child = -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                     O_WRONLY, 0);
    const int spawned = posix_spawnp(&child, "hyprctl", &actions, nullptr,
                                     const_cast<char* const*>(argv.data()),
                                     environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) return false;
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
}


// Reads hyprctl's own report of which workspaces exist. Returns an empty
// string when hyprctl is unavailable, which every caller treats as "no
// workspace information" rather than as an error.
inline std::string run_hyprctl_query(const char* subcommand) {
    if (!std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) return {};
    std::string command = "hyprctl ";
    command += subcommand;
    command += " -j 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return {};
    std::string output;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe)) output += buffer;
    pclose(pipe);
    return output;
}

#endif  // __linux__

// The lowest workspace with nothing on it, for an arcade wall to occupy so
// its columns do not land on top of whatever the user was doing. Hyprland
// creates workspaces on demand, so an id it has never heard of is by
// definition empty - and the ones it does report are only spare if they hold
// no windows.
//
// Returns 0 when there is no spare one, or when we are not on Hyprland at
// all, in which case the wall stays where it is.
inline int spare_workspace_from(const std::string& report,
                                int highest = 10) {
    if (report.empty()) return 0;
    std::vector<bool> occupied(static_cast<std::size_t>(highest) + 1, false);
    // Each entry reads "id": N ... "windows": M. Pair them up in order; a
    // workspace with no windows is as good as one that does not exist.
    std::size_t cursor = 0;
    int parsed = 0;
    while (true) {
        const std::size_t id_at = report.find("\"id\":", cursor);
        if (id_at == std::string::npos) break;
        const std::size_t windows_at = report.find("\"windows\":", id_at);
        if (windows_at == std::string::npos) break;
        const int id = std::atoi(report.c_str() + id_at + 5);
        const int windows = std::atoi(report.c_str() + windows_at + 10);
        ++parsed;
        if (id >= 1 && id <= highest && windows > 0)
            occupied[static_cast<std::size_t>(id)] = true;
        cursor = windows_at + 10;
    }
    // A report we could not read a single workspace out of tells us nothing.
    // Guessing from that would move the user off their desktop on the
    // strength of no information at all, so stay put instead.
    if (parsed == 0) return 0;
    for (int id = 1; id <= highest; ++id)
        if (!occupied[static_cast<std::size_t>(id)]) return id;
    return 0;
}

#if defined(__linux__)
inline int find_spare_workspace(int highest = 10) {
    return spare_workspace_from(run_hyprctl_query("workspaces"), highest);
}

// Sends this process's window to a workspace without following it. The wall's
// first column switches the view once, after every column has been sent, so
// the desktop does not flick between workspaces as each one starts.
inline bool send_window_to_workspace(SDL_Window* window, int workspace) {
    if (!window || workspace <= 0) return false;
    if (!std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) return false;
    return run_hyprctl_dispatch({"movetoworkspacesilent",
                                 std::to_string(workspace) + ",pid:" +
                                     std::to_string(getpid())});
}

// Brings the user to the workspace the wall was sent to.
inline bool show_workspace(int workspace) {
    if (workspace <= 0) return false;
    if (!std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) return false;
    return run_hyprctl_dispatch({"workspace", std::to_string(workspace)});
}

// Puts this process's window at an exact desktop rectangle.
//
// Both halves matter. Wayland denies clients global coordinates, so the
// position has to go through the compositor - but the size does too:
// SDL_SetWindowSize is only a request, and a compositor-managed window keeps
// whatever geometry it was given. Moving without resizing is what makes an
// arcade wall look like a row of windows sitting at the right places instead
// of columns filling the screen.
inline bool run_hyprctl_place(int x, int y, int width, int height) {
    if (!std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) return false;
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!driver || std::string(driver) != "wayland") return false;
    // Every dispatch names this process's own window. Focusing first and then
    // acting on "activewindow" was a real hazard: if focus did not land where
    // expected - focus-follows-mouse, a column still mapping, a dialog taking
    // it - the resize and move would land on whatever the user had focused.
    const std::string selector = "pid:" + std::to_string(getpid());
    const std::string suffix = "," + selector;
    // A tiled window ignores both an explicit position and size.
    if (!run_hyprctl_dispatch({"setfloating", selector})) return false;
    // Borders and rounding would leave seams between columns.
    run_hyprctl_dispatch({"setprop", selector, "noborder", "1"});
    run_hyprctl_dispatch({"setprop", selector, "rounding", "0"});
    // Size before position: resizing can shift where the window sits.
    if (width > 0 && height > 0)
        run_hyprctl_dispatch(
            {"resizewindowpixel", "exact " + std::to_string(width) + " " +
                                      std::to_string(height) + suffix});
    return run_hyprctl_dispatch(
        {"movewindowpixel", "exact " + std::to_string(x) + " " +
                                std::to_string(y) + suffix});
}
#endif

// An arcade wall: the primary display is divided into `count` equal columns
// and this process occupies column `slot`, sized to the largest aspect-correct
// fit inside it. Every column runs a different game simultaneously; the one
// with keyboard focus takes the sound and the controls.


// This process's window as the compositor actually has it. Returns false when
// it does not know about the window at all, which is the normal state for the
// first moments after a window is created.
//
// Reading this back is the only honest way to tell whether a placement took.
// Asking SDL is worthless: place_wall_slot sets the size through SDL first, so
// SDL_GetWindowSize reports the size we asked for whether or not the
// compositor ever honoured it - which is exactly how a wall of windows piled
// on top of each other reported success on every column.
inline bool compositor_geometry_of(const std::string& report, int pid, int& x,
                                   int& y, int& width, int& height) {
    // Each window entry begins with "address". Splitting on that is what keeps
    // this honest: a window object contains a nested "workspace" object, so
    // walking back to the nearest brace from "pid" lands inside the workspace
    // instead - and "at" and "size", which are listed before it, fall outside
    // the chunk entirely. That silently reported every window as unknown.
    const std::string boundary = "\"address\"";
    std::size_t entry = report.find(boundary);
    while (entry != std::string::npos) {
        const std::size_t next = report.find(boundary, entry + boundary.size());
        const std::string chunk = report.substr(
            entry, next == std::string::npos ? std::string::npos
                                             : next - entry);
        int found_pid = -1;
        const std::size_t pid_at = chunk.find("\"pid\"");
        if (pid_at != std::string::npos) {
            const std::size_t colon = chunk.find(':', pid_at);
            if (colon != std::string::npos)
                found_pid = std::atoi(chunk.c_str() + colon + 1);
        }
        if (found_pid == pid) {
            const auto pair = [&chunk](const char* key, int& first,
                                       int& second) {
                const std::size_t found = chunk.find(key);
                if (found == std::string::npos) return false;
                const std::size_t bracket = chunk.find('[', found);
                if (bracket == std::string::npos) return false;
                return std::sscanf(chunk.c_str() + bracket, "[%d, %d", &first,
                                   &second) == 2;
            };
            return pair("\"at\"", x, y) && pair("\"size\"", width, height);
        }
        entry = next;
    }
    return false;
}

#if defined(__linux__)
inline bool compositor_window_geometry(int& x, int& y, int& width,
                                       int& height) {
    return compositor_geometry_of(run_hyprctl_query("clients"),
                                  static_cast<int>(getpid()), x, y, width,
                                  height);
}
#else
// No compositor to ask. Callers treat this exactly as they treat a window the
// compositor has not heard of yet, which is the honest answer here.
inline bool compositor_window_geometry(int&, int&, int&, int&) {
    return false;
}
#endif

// Where the wall's columns agree on which of them is being played.
//
// Each process learns only its own focus from SDL - "I gained focus", "I lost
// it" - and that is not enough to lay the wall out, because a column's
// position depends on which *other* column is the big one. The focused
// process writes its slot here and the rest read it, which is the whole
// coordination the wall needs: one integer, no IPC, and a stale file left by
// a crash is harmless because the next focus event overwrites it.
inline std::string wall_focus_path() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::string root = runtime && *runtime ? runtime : "/tmp";
    return root + "/whitty-wall-focus";
}

inline bool publish_wall_focus(int slot) {
    if (slot < 0) return false;
    FILE* file = std::fopen(wall_focus_path().c_str(), "w");
    if (!file) return false;
    std::fprintf(file, "%d\n", slot);
    std::fclose(file);
    return true;
}

// The column currently being played, or -1 when nothing has claimed focus
// yet - in which case the wall lays itself out in equal columns.
inline int read_wall_focus() {
    FILE* file = std::fopen(wall_focus_path().c_str(), "r");
    if (!file) return -1;
    int slot = -1;
    if (std::fscanf(file, "%d", &slot) != 1) slot = -1;
    std::fclose(file);
    return slot;
}

inline void forget_wall_focus() { std::remove(wall_focus_path().c_str()); }


// Tells the compositor to leave the wall's windows alone.
//
// Hyprland tiles every new window, and re-tiles the existing ones each time a
// sibling appears - so a column placed correctly is resized again the moment
// the next column opens. The logs show exactly that: columns reporting
// 5096x1380 and 2541x1380, which are the whole usable area and half of it,
// never sizes the wall asked for.
//
// A rule registered through hyprctl keyword lives only until the compositor
// next reloads its config, so this changes nothing the user has written down.
inline void register_wall_window_rules() {
    if (!std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) return;
    // Both spellings of the rule keyword: windowrulev2 is what older builds
    // want, windowrule what current ones do. The wrong one is simply refused.
    for (const char* keyword : {"windowrule", "windowrulev2"}) {
        for (const char* rule : {"float, class:^([Ww]hitty[Aa]rcade)$",
                                 "noborder, class:^([Ww]hitty[Aa]rcade)$",
                                 "rounding 0, class:^([Ww]hitty[Aa]rcade)$"}) {
            std::string command = "hyprctl keyword ";
            command += keyword;
            command += " \"";
            command += rule;
            command += "\" >/dev/null 2>&1";
            if (std::system(command.c_str()) != 0) break;
        }
    }
}

// The workspace an arcade wall was told to occupy, or 0 for "stay put". The
// first column chooses it and passes it to the others on their command line,
// because each one choosing independently would race: once the first window
// lands, that workspace is no longer spare and the next column would pick a
// different one.
inline int wall_workspace() {
    const char* value = std::getenv("WHITTY_WALL_WORKSPACE");
    return value ? std::atoi(value) : 0;
}

inline bool place_wall_slot(SDL_Window* window, int slot, int count) {
    if (!window || count < 1 || slot < 0 || slot >= count) return false;
    int display_count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&display_count);
    if (!displays || display_count == 0) {
        if (displays) SDL_free(displays);
        return false;
    }
    SDL_Rect bounds{0, 0, 1920, 1080};
    const SDL_DisplayID display = displays[0];
    // Full display bounds, not the usable area: a wall covers the screen
    // edge to edge, so panels and bars are not carved out of it.
    if (!SDL_GetDisplayBounds(display, &bounds))
        SDL_GetDisplayUsableBounds(display, &bounds);
    SDL_free(displays);
    // Each column takes its whole slot with no border. The game letterboxes
    // itself inside that, so the wall reads as one fullscreen surface split
    // into cabinets rather than a row of floating windows. The cabinet being
    // played is the large one; the layout absorbs the rounding so no seam is
    // left at the right-hand edge.
    std::array<wall_column_rect, 8> columns{};
    const int produced = layout_wall_columns(
        bounds.w, count, read_wall_focus(), columns.data(),
        static_cast<int>(columns.size()));
    if (produced < 1) return false;
    const wall_column_rect& mine =
        columns[static_cast<std::size_t>(std::min(slot, produced - 1))];
    const int width = mine.width;
    const int height = bounds.h;
    const int x = bounds.x + mine.x;
    const int y = bounds.y;
    SDL_SetWindowBordered(window, false);
    SDL_SetWindowMinimumSize(window, 1, 1);
    SDL_SetWindowMaximumSize(window, 16384, 16384);
    SDL_SetWindowSize(window, width, height);
    SDL_ShowWindow(window);
    SDL_SyncWindow(window);
#if defined(__linux__)
    // Nothing can be placed before the compositor knows the window exists. A
    // selector that matches nothing still reports success, so dispatching
    // early looks exactly like dispatching correctly - and left every column
    // piled at the compositor's own default position. The caller retries, so
    // declining here costs a frame and nothing else.
    int had_x = 0;
    int had_y = 0;
    int had_width = 0;
    int had_height = 0;
    const bool known =
        compositor_window_geometry(had_x, had_y, had_width, had_height);
    if (!known && std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) return false;
    // A wall takes over the display, so it goes somewhere of its own if there
    // is a free workspace rather than burying whatever was already open.
    // Before the geometry, because arriving on a workspace re-lays the window
    // out and would undo it.
    send_window_to_workspace(window, wall_workspace());
    run_hyprctl_place(x, y, width, height);
#endif
    SDL_SetWindowPosition(window, x, y);
    SDL_SyncWindow(window);
    // Ask the compositor what it actually did. SDL would just repeat the size
    // set above whether or not anything honoured it.
    int got_x = 0;
    int got_y = 0;
    int got_width = 0;
    int got_height = 0;
    if (compositor_window_geometry(got_x, got_y, got_width, got_height)) {
        // A little slack: fractional scaling and borders move the reported
        // numbers by a pixel or two without the wall looking any different.
        return std::abs(got_x - x) <= 4 && std::abs(got_width - width) <= 4 &&
               std::abs(got_height - height) <= 4;
    }
    int actual_width = 0;
    int actual_height = 0;
    SDL_GetWindowSize(window, &actual_width, &actual_height);
    return std::abs(actual_width - width) <= 2 &&
           std::abs(actual_height - height) <= 2;
}

#if defined(__linux__)
// Legacy directional mover, kept only for callers that have no exact target.
// It focuses this process's window and nudges it toward one side; prefer
// run_hyprctl_place() above, which asks for a coordinate outright.
inline bool run_hyprctl_move(int x) {
    if (!std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) return false;
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!driver || std::string(driver) != "wayland") return false;
    if (!run_hyprctl_dispatch({"focuswindow",
                               "pid:" + std::to_string(getpid())}))
        return false;
    // Several moves, to get past any window already attached to that side.
    const std::string direction = x <= 0 ? "l" : "r";
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (!run_hyprctl_dispatch({"movewindow", direction})) return false;
        usleep(50000);
    }
    return true;
}
#endif

// Two cabinet processes sharing one screen: each fills its own half, edge to
// edge and without a border. This is exactly a two-column wall, so it is one -
// a shared cabinet must never look like a pair of windows on a desktop.
inline bool place_cabinet_half(SDL_Window* window, int cabinet_node) {
    if (cabinet_node < 1 || cabinet_node > 2) return false;
    return place_wall_slot(window, cabinet_node - 1, 2);
}

inline void arrange_twin_windows(SDL_Window* player1, SDL_Window* player2,
                                 int requested_width) {
    if (!player1 || !player2) return;
    SDL_DisplayID display = SDL_GetDisplayForWindow(player1);
    if (!display) display = SDL_GetPrimaryDisplay();
    SDL_Rect usable{0, 0, 1920, 1080};
    if (!display || !SDL_GetDisplayUsableBounds(display, &usable)) {
        if (display) SDL_GetDisplayBounds(display, &usable);
    }
    const twin_layout layout = fit_twin_layout(usable, requested_width);
    set_exact_size(player1, layout.width, layout.height);
    set_exact_size(player2, layout.width, layout.height);
    SDL_SetWindowTitle(player1, "WhittyArcade - Player 1");
    SDL_SetWindowTitle(player2, "WhittyArcade - Player 2");
    SDL_ShowWindow(player1);
    SDL_ShowWindow(player2);
    SDL_SyncWindow(player1);
    SDL_SyncWindow(player2);

    const bool first_placed =
        SDL_SetWindowPosition(player1, layout.first_x, layout.y);
    const bool second_placed =
        SDL_SetWindowPosition(player2, layout.second_x, layout.y);
#if defined(__linux__)
    // Wayland intentionally denies clients global window coordinates. Hyprland
    // exposes an authenticated per-user compositor command for this purpose;
    // use it only when SDL's normal cross-platform request is unavailable.
    if (!first_placed || !second_placed) {
        // hyprctl selects by pid, and both windows belong to this process, so
        // only the focused one can be addressed. Nudge it to the left half and
        // leave the compositor to tile the other.
        run_hyprctl_move(-1);
    }
#else
    (void)first_placed;
    (void)second_placed;
#endif
    SDL_RaiseWindow(player1);
}

} // namespace whitty_window
