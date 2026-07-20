// arcade_settings.h - persistent application-wide settings.
#pragma once

#include <string>

enum class renderer_backend {
    opengl,
    vulkan,
    software,
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
};

const char* renderer_backend_name(renderer_backend backend);

emulator_settings load_settings();
bool save_settings(const emulator_settings& settings);
std::string settings_path();
