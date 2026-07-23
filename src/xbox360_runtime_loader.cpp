#include "xbox360_runtime_loader.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path executable_directory() {
#if defined(_WIN32)
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    return length ? fs::path(path.data()).parent_path() : fs::path{};
#else
    std::array<char, 4096> path{};
    const ssize_t length = readlink("/proc/self/exe", path.data(),
                                    path.size() - 1);
    if (length <= 0) return {};
    path[static_cast<std::size_t>(length)] = '\0';
    return fs::path(path.data()).parent_path();
#endif
}

fs::path runtime_library_path() {
    if (const char* override_path =
            std::getenv("WHITTY_XBOX360_RUNTIME")) {
        if (*override_path) return fs::path(override_path);
    }
#if defined(_WIN32)
    constexpr const char* filename = "whitty_robotron_runtime.dll";
#else
    constexpr const char* filename = "libwhitty_robotron_runtime.so";
#endif
    return executable_directory() / filename;
}

void close_library(void* library) {
    if (!library) return;
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}

void* load_library(const fs::path& path, std::string& error) {
#if defined(_WIN32)
    HMODULE module = LoadLibraryW(path.wstring().c_str());
    if (!module) error = "Could not load " + path.string() + '.';
    return module;
#else
    // ReXGlue contains a private static SDL3. Deep binding keeps its internal
    // SDL calls ahead of WhittyArcade's already-loaded SDL3 symbols, while a
    // normal namespace is required by proprietary Vulkan drivers that load
    // helper DSOs internally and are not safe under dlmopen(LM_ID_NEWLM).
    int flags = RTLD_NOW | RTLD_LOCAL;
#if defined(RTLD_DEEPBIND)
    flags |= RTLD_DEEPBIND;
#endif
#if defined(RTLD_NODELETE)
    // ReXGlue and its Vulkan backend register process-lifetime callbacks in
    // shared dependencies.  Unmapping the module leaves those callbacks
    // pointing at released code and crashes later during libc exit.  Keeping
    // the code mapped still permits Runtime::Shutdown() to release the title,
    // GPU, audio, and input state, and also makes repeated game launches safe.
    flags |= RTLD_NODELETE;
#endif
    void* module = dlopen(path.c_str(), flags);
    if (!module) {
        const char* message = dlerror();
        error = "Could not load " + path.string() +
                (message ? ": " + std::string(message) : ".");
    }
    return module;
#endif
}

void* load_symbol(void* library, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(
        static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

} // namespace

xbox360_runtime_loader::~xbox360_runtime_loader() {
    shutdown();
}

bool xbox360_runtime_loader::initialize(
        const std::string& game_root, const std::string& user_root,
        const std::string& cache_root, std::string& error) {
    shutdown();
    const fs::path library_path = runtime_library_path();
    std::error_code type_error;
    if (!fs::is_regular_file(library_path, type_error)) {
        error = "Robotron runtime module is not installed: " +
                library_path.string() +
                ". Configure WHITTY_ENABLE_XBOX360=ON to build it.";
        return false;
    }
    m_library = load_library(library_path, error);
    if (!m_library) return false;
    const auto get_api = reinterpret_cast<whitty_xbox360_get_api_fn>(
        load_symbol(m_library, "whitty_xbox360_get_api"));
    if (!get_api) {
        error = "Robotron runtime module has no WhittyArcade API entrypoint.";
        shutdown();
        return false;
    }
    m_api = get_api(WHITTY_XBOX360_RUNTIME_ABI);
    if (!m_api || m_api->abi_version != WHITTY_XBOX360_RUNTIME_ABI ||
        m_api->struct_size < sizeof(whitty_xbox360_api) || !m_api->create ||
        !m_api->destroy || !m_api->set_input || !m_api->capture_frame ||
        !m_api->read_audio) {
        error = "Robotron runtime module uses an incompatible API version.";
        shutdown();
        return false;
    }
    const whitty_xbox360_config config{
        sizeof(whitty_xbox360_config), game_root.c_str(), user_root.c_str(),
        cache_root.c_str()};
    std::array<char, 1024> detail{};
    m_runtime = m_api->create(&config, detail.data(), detail.size());
    if (!m_runtime) {
        error = detail[0] ? detail.data() :
            "Robotron runtime could not be created.";
        shutdown();
        return false;
    }
    return true;
}

void xbox360_runtime_loader::shutdown() {
    if (m_runtime && m_api && m_api->destroy) m_api->destroy(m_runtime);
    m_runtime = nullptr;
    m_api = nullptr;
    close_library(m_library);
    m_library = nullptr;
}

void xbox360_runtime_loader::set_input(
        const whitty_xbox360_input& input) {
    if (m_runtime && m_api) m_api->set_input(m_runtime, &input);
}

bool xbox360_runtime_loader::capture_frame(
        std::vector<std::uint8_t>& rgba, int& width, int& height,
        std::uint64_t& sequence) {
    if (!m_runtime || !m_api) return false;
    whitty_xbox360_frame frame{};
    frame.struct_size = sizeof(frame);
    if (!m_api->capture_frame(m_runtime, &frame) || !frame.rgba ||
        frame.width == 0 || frame.height == 0 ||
        frame.width > 8192 || frame.height > 8192 ||
        frame.stride < frame.width * 4) return false;
    const std::size_t row_bytes = static_cast<std::size_t>(frame.width) * 4;
    if (frame.height > std::numeric_limits<std::size_t>::max() / row_bytes)
        return false;
    rgba.resize(row_bytes * frame.height);
    for (std::uint32_t row = 0; row < frame.height; ++row)
        std::memcpy(rgba.data() + static_cast<std::size_t>(row) * row_bytes,
                    frame.rgba + static_cast<std::size_t>(row) * frame.stride,
                    row_bytes);
    width = static_cast<int>(frame.width);
    height = static_cast<int>(frame.height);
    sequence = frame.sequence;
    return true;
}

std::size_t xbox360_runtime_loader::read_audio(
        std::int16_t* stereo, std::size_t frame_capacity) {
    return m_runtime && m_api ?
        m_api->read_audio(m_runtime, stereo, frame_capacity) : 0;
}

void xbox360_runtime_loader::set_paused(bool paused) {
    if (m_runtime && m_api && m_api->set_paused)
        m_api->set_paused(m_runtime, paused ? 1 : 0);
}

bool xbox360_runtime_loader::running() const {
    return m_runtime && m_api &&
           (!m_api->is_running || m_api->is_running(m_runtime));
}

xbox360_runtime_achievement xbox360_runtime_loader::copy_achievement(
        const whitty_xbox360_achievement& value) {
    xbox360_runtime_achievement result;
    result.id = value.id;
    result.gamerscore = value.gamerscore;
    result.unlocked = value.unlocked != 0;
    result.unlocked_at_filetime = value.unlocked_at_filetime;
    if (value.label) result.label = value.label;
    if (value.description) result.description = value.description;
    return result;
}

std::vector<xbox360_runtime_achievement>
xbox360_runtime_loader::achievements() const {
    std::vector<xbox360_runtime_achievement> result;
    if (!m_runtime || !m_api || !m_api->achievement_count ||
        !m_api->achievement_at) return result;
    const std::size_t count = std::min<std::size_t>(
        m_api->achievement_count(m_runtime), 1024);
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        whitty_xbox360_achievement value{};
        value.struct_size = sizeof(value);
        if (m_api->achievement_at(m_runtime, index, &value))
            result.push_back(copy_achievement(value));
    }
    return result;
}

bool xbox360_runtime_loader::take_unlocked_achievement(
        xbox360_runtime_achievement& achievement) {
    if (!m_runtime || !m_api || !m_api->take_unlocked_achievement)
        return false;
    whitty_xbox360_achievement value{};
    value.struct_size = sizeof(value);
    if (!m_api->take_unlocked_achievement(m_runtime, &value)) return false;
    achievement = copy_achievement(value);
    return true;
}
