#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <cwchar>
#endif

namespace whitty_platform {
namespace fs = std::filesystem;

inline fs::path environment_path(const char* name) {
    const char* value = std::getenv(name);
    return value && *value ? fs::path(value) : fs::path{};
}

inline int cabinet_node() {
    const char* value = std::getenv("WHITTY_CABINET_NODE");
    if (!value || !*value) return 0;
    const int node = std::atoi(value);
    return node == 1 || node == 2 ? node : 0;
}

inline std::string cabinet_scoped_name(std::string name) {
    const int node = cabinet_node();
    if (node > 1) name += "-cab" + std::to_string(node);
    return name;
}

#if defined(_WIN32)
inline fs::path windows_environment_path(const wchar_t* name) {
    const wchar_t* value = _wgetenv(name);
    return value && *value ? fs::path(value) : fs::path{};
}
#endif

inline fs::path config_root() {
    if (const fs::path xdg = environment_path("XDG_CONFIG_HOME"); !xdg.empty())
        return xdg;
#if defined(_WIN32)
    if (const fs::path local = windows_environment_path(L"LOCALAPPDATA");
        !local.empty())
        return local;
#endif
    if (const fs::path home = environment_path("HOME"); !home.empty())
        return home / ".config";
    return {};
}

inline fs::path data_root() {
    if (const fs::path xdg = environment_path("XDG_DATA_HOME"); !xdg.empty())
        return xdg;
#if defined(_WIN32)
    if (const fs::path local = windows_environment_path(L"LOCALAPPDATA");
        !local.empty())
        return local;
#endif
    if (const fs::path home = environment_path("HOME"); !home.empty())
        return home / ".local" / "share";
    return {};
}

inline fs::path home_root() {
#if defined(_WIN32)
    if (const fs::path profile = windows_environment_path(L"USERPROFILE");
        !profile.empty())
        return profile;
#endif
    return environment_path("HOME");
}

inline fs::path downloads_root() {
    const fs::path home = home_root();
    return home.empty() ? fs::path{} : home / "Downloads";
}

inline char path_list_separator() {
#if defined(_WIN32)
    return ';';
#else
    return ':';
#endif
}

inline bool video_available() {
    if (const char* headless = std::getenv("WHITTYARCADE_HEADLESS")) {
        if (*headless && std::string(headless) != "0" &&
            std::string(headless) != "false")
            return false;
    }
#if defined(_WIN32)
    return true;
#else
    const char* display = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    return (display && *display) || (wayland && *wayland);
#endif
}

inline std::vector<fs::path> font_paths() {
#if defined(_WIN32)
    const fs::path windows = windows_environment_path(L"WINDIR");
    if (windows.empty()) return {};
    return {
        windows / "Fonts" / "segoeui.ttf",
        windows / "Fonts" / "arial.ttf",
    };
#else
    return {
        "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
#endif
}

} // namespace whitty_platform
