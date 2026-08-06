#include "arcade_settings.h"
#include "platform_paths.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace fs = std::filesystem;

namespace {
fs::path config_root() {
    return manx_platform::config_root();
}

fs::path config_path() {
    const fs::path root = config_root();
    return root.empty() ? fs::path("MANX-settings.ini") :
                          root / "MANX" / "settings.ini";
}

fs::path legacy_config_path() {
    const fs::path root = config_root();
    return root.empty() ? fs::path("MANX-settings.ini") :
                          root / "MANX" / "settings.ini";
}

fs::path oldest_config_path() {
    const fs::path root = config_root();
    return root.empty() ? fs::path("ridge_racer_settings.ini") :
                          root / "ridge_racer_emulator" / "settings.ini";
}

bool parse_integer(std::string_view text, int& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse_boolean(std::string_view text, bool& value) {
    if (text == "1" || text == "true") {
        value = true;
        return true;
    }
    if (text == "0" || text == "false") {
        value = false;
        return true;
    }
    return false;
}
} // namespace

std::string settings_path() {
    return config_path().string();
}

emulator_settings load_settings() {
    emulator_settings settings;
    std::ifstream input(config_path());
    // Preserve established installs on the first MANX launch. New saves use
    // the rebranded application directory above.
    if (!input) input.open(legacy_config_path());
    if (!input) input.open(oldest_config_path());
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) continue;
        const std::string_view key(line.data(), separator);
        const std::string_view value(line.data() + separator + 1,
                                     line.size() - separator - 1);
        if (key == "master_volume") parse_integer(value, settings.master_volume);
        else if (key == "music_volume") parse_integer(value, settings.music_volume);
        else if (key == "effects_volume") parse_integer(value, settings.effects_volume);
        else if (key == "window_width") parse_integer(value, settings.window_width);
        else if (key == "window_scale") {
            // One-time migration from the old 1x/2x/4x selector.
            int legacy_scale = 2;
            if (parse_integer(value, legacy_scale))
                settings.window_width = legacy_scale * 640;
        }
        else if (key == "fullscreen") parse_boolean(value, settings.fullscreen);
        else if (key == "vsync") parse_boolean(value, settings.vsync);
        else if (key == "integer_scaling") parse_boolean(value, settings.integer_scaling);
        else if (key == "linear_filtering") parse_boolean(value, settings.linear_filtering);
        else if (key == "show_fps") parse_boolean(value, settings.show_fps);
        else if (key == "show_renderer")
            parse_boolean(value, settings.show_renderer);
        else if (key == "network_play")
            parse_boolean(value, settings.network_play);
        else if (key == "rom_directory")
            settings.rom_directory = std::string(value);
        else if (key == "chd_directory")
            settings.chd_directory = std::string(value);
        else if (key == "media_directory")
            settings.media_directory = std::string(value);
        else if (key == "media_artwork_category") {
            // Still-image categories plus the two view modes stored in the
            // same key; "coverflow" missing here silently reverted the
            // saved Cover Flow view to the plain card grid on every launch.
            static constexpr std::array<std::string_view, 10> allowed{{
                "box2d", "box3d", "titles", "images", "marquee",
                "thumbnails", "fanarts", "boxback", "cartridges",
                "coverflow",
            }};
            if (std::find(allowed.begin(), allowed.end(), value) !=
                allowed.end())
                settings.media_artwork_category = std::string(value);
        }
        else if (key == "library_setup_complete")
            parse_boolean(value, settings.library_setup_complete);
        else if (key == "renderer") {
            if (value == "vulkan") settings.renderer = renderer_backend::vulkan;
            else if (value == "software")
                settings.renderer = renderer_backend::software;
            else settings.renderer = renderer_backend::opengl;
        }
        else if (key == "output") {
            settings.output = value == "dual" ? output_mode::dual :
                                                output_mode::single;
        }
    }
    settings.master_volume = std::clamp(settings.master_volume, 0, 200);
    settings.music_volume = std::clamp(settings.music_volume, 0, 100);
    settings.effects_volume = std::clamp(settings.effects_volume, 0, 100);
    settings.window_width = std::clamp(settings.window_width, 320, 7680);
    return settings;
}

bool save_settings(const emulator_settings& settings) {
    const fs::path path = config_path();
    std::error_code error;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), error);
    if (error) return false;
    std::ofstream output(path, std::ios::trunc);
    if (!output) return false;
    output << "master_volume=" << settings.master_volume << '\n'
           << "music_volume=" << settings.music_volume << '\n'
           << "effects_volume=" << settings.effects_volume << '\n'
           << "window_width=" << settings.window_width << '\n'
           << "fullscreen=" << settings.fullscreen << '\n'
           << "vsync=" << settings.vsync << '\n'
           << "integer_scaling=" << settings.integer_scaling << '\n'
           << "linear_filtering=" << settings.linear_filtering << '\n'
           << "show_fps=" << settings.show_fps << '\n'
           << "show_renderer=" << settings.show_renderer << '\n'
           << "network_play=" << settings.network_play << '\n'
           << "renderer=" << renderer_backend_name(settings.renderer) << '\n'
           << "output=" << output_mode_name(settings.output) << '\n'
           << "rom_directory=" << settings.rom_directory << '\n'
           << "chd_directory=" << settings.chd_directory << '\n'
           << "media_directory=" << settings.media_directory << '\n'
           << "media_artwork_category="
           << settings.media_artwork_category << '\n'
           << "library_setup_complete="
           << settings.library_setup_complete << '\n';
    return output.good();
}

const char* renderer_backend_name(renderer_backend backend) {
    switch (backend) {
    case renderer_backend::vulkan: return "vulkan";
    case renderer_backend::software: return "software";
    case renderer_backend::opengl: return "opengl";
    }
    return "opengl";
}

const char* output_mode_name(output_mode mode) {
    switch (mode) {
    case output_mode::dual: return "dual";
    case output_mode::single: return "single";
    }
    return "single";
}
