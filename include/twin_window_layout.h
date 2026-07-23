// Cross-desktop placement for the windowed Twin Display mode.
#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
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

    const std::string x_text = std::to_string(x);
    const std::string target = std::to_string(y) + ",title:^(" + title + ")$";
    const char* arguments[]{
        "hyprctl", "dispatch", "movewindowpixel", "exact",
        x_text.c_str(), target.c_str(), nullptr,
    };
    pid_t process = -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(
        &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    const int spawned = posix_spawnp(
        &process, "hyprctl", &actions, nullptr,
        const_cast<char* const*>(arguments), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) return false;
    int status = 0;
    return waitpid(process, &status, 0) == process &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0;
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
