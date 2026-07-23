// arcade_settings.h - persistent application-wide settings.
#pragma once

#include <string>

enum class renderer_backend {
    opengl,
    vulkan,
    software,
};

// Single: one letterboxed image on the host window. Dual: the drawable is
// split into one pane per physical screen (side by side on an ultrawide or a
// spanned desktop) so two players each get their own aspect-correct view.
enum class output_mode {
    single,
    dual,
};

struct emulator_settings {
    int master_volume{100};
    int music_volume{100};
    int effects_volume{100};
    int window_width{1280};
    bool fullscreen{false};
    bool vsync{true};
    bool integer_scaling{true};
    bool linear_filtering{false};
    bool show_fps{false};
    bool show_renderer{true};
    renderer_backend renderer{renderer_backend::opengl};
    output_mode output{output_mode::single};
    // Runtime-only physical display assignment used by paired cabinet
    // processes. -1 follows the desktop/window-manager default. This is not
    // persisted because a portable install may see a different monitor order.
    int display_index{-1};
    // Runtime-only Twin Screen placement. When true, the primary and mirror
    // windows are explicitly centred on physical displays 0 and 1.
    bool twin_separate_monitors{false};
};

const char* renderer_backend_name(renderer_backend backend);
const char* output_mode_name(output_mode mode);

emulator_settings load_settings();
bool save_settings(const emulator_settings& settings);
std::string settings_path();
