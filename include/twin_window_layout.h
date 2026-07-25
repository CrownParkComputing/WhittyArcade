// Cross-desktop placement for the windowed Twin Display mode.
#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

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
inline bool run_hyprctl_move(const char* title, int x, int y) {
    if (!title || !*title || !std::getenv("HYPRLAND_INSTANCE_SIGNATURE"))
        return false;
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!driver || std::string(driver) != "wayland") return false;

    // Hyprland's movewindowpixel selector is unreliable from
    // non-interactive spawned processes. Use a reliable two-step
    // approach instead: focus our own window by PID, then nudge
    // it left or right with directional moves.
    const int direction = x <= 0 ? -1 : 1;
    const std::string focus_spec = "pid:" + std::to_string(getpid());
    const char* focus_argv[] = {
        "hyprctl", "dispatch", "focuswindow",
        focus_spec.c_str(), nullptr,
    };
    pid_t focus_pid = -1;
    posix_spawn_file_actions_t focus_actions;
    posix_spawn_file_actions_init(&focus_actions);
    posix_spawn_file_actions_addopen(
        &focus_actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(
        &focus_actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    const int focus_spawned = posix_spawnp(
        &focus_pid, "hyprctl", &focus_actions, nullptr,
        const_cast<char* const*>(focus_argv), environ);
    posix_spawn_file_actions_destroy(&focus_actions);
    if (focus_spawned != 0) return false;
    int focus_status = 0;
    if (waitpid(focus_pid, &focus_status, 0) != focus_pid ||
        !WIFEXITED(focus_status) || WEXITSTATUS(focus_status) != 0) {
        return false;
    }

    // Nudge the window to the correct side of the display. Use a
    // few movewindow calls to get past any attached side.
    const char* move_dir = direction < 0 ? "l" : "r";
    const char* move_argv[] = {
        "hyprctl", "dispatch", "movewindow", move_dir, nullptr,
    };
    const int moves = 5;
    for (int i = 0; i < moves; ++i) {
        pid_t move_proc = -1;
        posix_spawn_file_actions_t move_actions;
        posix_spawn_file_actions_init(&move_actions);
        posix_spawn_file_actions_addopen(
            &move_actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(
            &move_actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        const int move_spawned = posix_spawnp(
            &move_proc, "hyprctl", &move_actions, nullptr,
            const_cast<char* const*>(move_argv), environ);
        posix_spawn_file_actions_destroy(&move_actions);
        if (move_spawned != 0) return false;
        int move_status = 0;
        if (waitpid(move_proc, &move_status, 0) != move_proc ||
            !WIFEXITED(move_status)) {
            return false;
        }
        usleep(50000);
    }
    return true;
}
#endif

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
        run_hyprctl_move("WhittyArcade - Player 1",
                         layout.first_x, layout.y);
        run_hyprctl_move("WhittyArcade - Player 2",
                         layout.second_x, layout.y);
    }
#else
    (void)first_placed;
    (void)second_placed;
#endif
    SDL_RaiseWindow(player1);
}

} // namespace whitty_window
