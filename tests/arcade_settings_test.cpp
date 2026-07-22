#include "arcade_settings.h"
#include "test_platform.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("whittyarcade-settings-test-" + std::to_string(test_process_id()));
    fs::create_directories(root / "WhittyArcade");
    if (!test_set_environment("XDG_CONFIG_HOME", root)) return 1;

    {
        std::ofstream output(root / "WhittyArcade" / "settings.ini");
        output << "master_volume=125\n"
               << "renderer=software\n"
               << "show_fps=true\n"
               << "show_renderer=false\n"
               << "window_width=1100\n"
               << "vsync=false\n";
    }
    emulator_settings settings = load_settings();
    if (settings.master_volume != 125 ||
        settings.renderer != renderer_backend::software ||
        !settings.show_fps || settings.show_renderer || settings.vsync ||
        settings.window_width != 1100)
        return 1;
    if (std::string(renderer_backend_name(renderer_backend::opengl)) !=
            "opengl" ||
        std::string(renderer_backend_name(renderer_backend::vulkan)) !=
            "vulkan" ||
        std::string(renderer_backend_name(renderer_backend::software)) !=
            "software")
        return 2;

    settings.renderer = renderer_backend::vulkan;
    settings.show_fps = false;
    if (!save_settings(settings)) return 3;
    const emulator_settings saved = load_settings();
    if (saved.renderer != renderer_backend::vulkan || saved.show_fps)
        return 4;

    fs::remove_all(root);
    return 0;
}
