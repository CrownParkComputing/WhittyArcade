#include "xbox360_native_runtime.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
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

std::string environment_value(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : std::string{};
}

#if !defined(_WIN32)
constexpr const char* kRuntimeSuffix = "";
#else
constexpr const char* kRuntimeSuffix = ".exe";
#endif

// The runtime's build tree puts each title's executable in its own directory
// under build/recompiled, named after the title - the same layout play.sh reads.
fs::path build_tree_binary(const fs::path& runtime_root,
                           const std::string& short_name) {
    return runtime_root / "build" / "recompiled" / short_name /
           (short_name + kRuntimeSuffix);
}

// The defaults tools/play.sh supplies: a window, and sixty frames a second
// because the console presented at sixty and the present rate IS the game speed.
//
// Deliberately NOT here: WHITTY_XMA_BYPASS. It plays the decoder's output on a
// stream of its own beside whatever the title mixes, which was how the music was
// first made audible before the hardware handshake worked. The title now reads
// the decoded audio out of the output ring and mixes it itself, so setting it
// plays every sound TWICE, unsynchronised. It is a diagnostic, not a default.
constexpr std::pair<const char*, const char*> kRuntimeDefaults[] = {
    {"WHITTY_WINDOW", "1"},
    {"WHITTY_FPS", "60"},
};

} // namespace

std::vector<std::string> xbox360_native_runtime_candidates(
        const std::string& short_name) {
    std::vector<std::string> candidates;
    if (short_name.empty()) return candidates;
    const auto add = [&candidates](const fs::path& path) {
        if (!path.empty())
            candidates.push_back(path.lexically_normal().string());
    };

    // An explicit binary always wins, so a developer can point WhittyArcade at
    // a build they are working on without moving anything.
    const std::string explicit_binary =
        environment_value("WHITTY_XENON_RUNTIME");
    if (!explicit_binary.empty()) add(fs::path(explicit_binary));

    const std::string runtime_root = environment_value("WHITTY_XENON_ROOT");
    if (!runtime_root.empty())
        add(build_tree_binary(fs::path(runtime_root), short_name));

    // Installed beside WhittyArcade itself.
    const fs::path here = executable_directory();
    if (!here.empty()) {
        add(here / (short_name + kRuntimeSuffix));
        add(here / "whitty_xenon" / (short_name + kRuntimeSuffix));
    }

    // The runtime's own checkout, as it sits during development.
    const std::string home = environment_value("HOME");
    if (!home.empty())
        add(build_tree_binary(fs::path(home) / "development" /
                                  "xenon-native" / "projects" / "whitty_xenon",
                              short_name));
    return candidates;
}

xbox360_native_launch plan_xbox360_native_launch(
        const std::string& short_name,
        const xbox360_native_content& content) {
    xbox360_native_launch launch;
    if (short_name.empty() || !content) {
        launch.error = "No Xbox 360 title was given to launch.";
        return launch;
    }
    const std::vector<std::string> candidates =
        xbox360_native_runtime_candidates(short_name);
    for (const std::string& candidate : candidates) {
        std::error_code error;
        const fs::path path(candidate);
        if (!fs::is_regular_file(path, error)) continue;
#if !defined(_WIN32)
        if (::access(candidate.c_str(), X_OK) != 0) continue;
#endif
        launch.binary = candidate;
        break;
    }
    if (launch.binary.empty()) {
        launch.error = "No native runtime for " + short_name +
                       " is installed. Looked for:";
        for (const std::string& candidate : candidates)
            launch.error += "\n  " + candidate;
        launch.error += "\nBuild it with TITLE=" + short_name +
                        " tools/build_recompiled_cpu.sh in the whitty_xenon "
                        "checkout, or set WHITTY_XENON_RUNTIME to the binary.";
        return launch;
    }

    // A packaged title is one argument: the package is the whole game, and the
    // runtime opens it itself. These paths habitually contain spaces - the
    // console's own layout puts the package under the store listing's name - and
    // nothing here goes through a shell, so the path stays one argument.
    if (content.packaged())
        launch.arguments = {launch.binary, content.package_path};
    else
        launch.arguments = {launch.binary, content.xex_path, content.game_root};
    for (const auto& [name, value] : kRuntimeDefaults) {
        if (std::getenv(name) != nullptr) continue;
        launch.environment.emplace_back(name, value);
    }
    return launch;
}

xbox360_native_process::~xbox360_native_process() {
    stop();
}

bool xbox360_native_process::start(const xbox360_native_launch& launch,
                                  std::string& error) {
    stop();
    if (!launch) {
        error = launch.error.empty() ? "The title has no launch plan."
                                     : launch.error;
        return false;
    }
    std::vector<std::string> arguments = launch.arguments;
    std::vector<char*> native_arguments;
    native_arguments.reserve(arguments.size() + 1);
    for (std::string& argument : arguments)
        native_arguments.push_back(argument.data());
    native_arguments.push_back(nullptr);

#if defined(_WIN32)
    for (const auto& [name, value] : launch.environment)
        _putenv_s(name.c_str(), value.c_str());
    m_process = _spawnv(_P_NOWAIT, launch.binary.c_str(),
                        native_arguments.data());
    if (m_process == -1) {
        error = "Could not start " + launch.binary + '.';
        return false;
    }
    return true;
#else
    const pid_t child = fork();
    if (child < 0) {
        error = "Could not fork to start " + launch.binary + '.';
        return false;
    }
    if (child == 0) {
        // In the child: the environment is set here rather than in the parent
        // so a launch cannot change WhittyArcade's own settings.
        for (const auto& [name, value] : launch.environment)
            ::setenv(name.c_str(), value.c_str(), 1);
        ::execv(launch.binary.c_str(), native_arguments.data());
        std::_Exit(127);
    }
    m_process = child;
    return true;
#endif
}

bool xbox360_native_process::running() {
    if (m_process == -1) return false;
#if defined(_WIN32)
    HANDLE handle = reinterpret_cast<HANDLE>(m_process);
    if (WaitForSingleObject(handle, 0) != WAIT_TIMEOUT) {
        CloseHandle(handle);
        m_process = -1;
        return false;
    }
    return true;
#else
    int status = 0;
    const pid_t child = static_cast<pid_t>(m_process);
    if (::waitpid(child, &status, WNOHANG) == child) {
        m_process = -1;
        return false;
    }
    return true;
#endif
}

void xbox360_native_process::stop() {
    if (m_process == -1) return;
#if defined(_WIN32)
    HANDLE handle = reinterpret_cast<HANDLE>(m_process);
    TerminateProcess(handle, 0);
    WaitForSingleObject(handle, 2000);
    CloseHandle(handle);
#else
    const pid_t child = static_cast<pid_t>(m_process);
    int status = 0;
    if (::waitpid(child, &status, WNOHANG) != child) {
        // Ask first: the runtime closes its window and reports its run on
        // SIGTERM, and only a title that ignores that is killed outright.
        ::kill(child, SIGTERM);
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (::waitpid(child, &status, WNOHANG) == child) {
                m_process = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ::kill(child, SIGKILL);
        ::waitpid(child, &status, 0);
    }
#endif
    m_process = -1;
}
