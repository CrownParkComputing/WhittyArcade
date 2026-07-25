// Launching a natively recompiled Xbox 360 title.
//
// Geometry Wars is not emulated by WhittyArcade: it is a native port, a real
// executable produced by the whitty_xenon runtime that owns its own Vulkan
// window, its own pad and its own audio device. WhittyArcade's job for such a
// title is the launcher's job - find the binary, hand it the game, and know when
// the player has finished with it.
//
// The decision of what to run is separated from the running of it, so what a
// launch would be can be checked without starting a process: everything about
// the command line and the environment comes out of plan_xbox360_native_launch.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// What the runtime is handed for a title, and therefore how many arguments the
// launch has.
//
// An extracted title is a default.xex with its data files beside it, so the
// runtime is given both: the image to load and the directory to read from. A
// packaged title is a single signed STFS package holding the executable and
// every data file inside it, so the runtime is given that one path and nothing
// else. Which it is comes from the title, and getting it wrong is invisible
// until the game does not start - hence a value that can be inspected in a test
// rather than a command line assembled at the call site.
struct xbox360_native_content {
    // Extracted titles.
    std::string xex_path;
    std::string game_root;
    // Packaged titles.
    std::string package_path;

    static xbox360_native_content extracted(std::string xex,
                                            std::string game_directory) {
        return {std::move(xex), std::move(game_directory), {}};
    }
    static xbox360_native_content package(std::string package_file) {
        return {{}, {}, std::move(package_file)};
    }

    bool packaged() const { return !package_path.empty(); }

    explicit operator bool() const {
        if (packaged()) return xex_path.empty() && game_root.empty();
        return !xex_path.empty() && !game_root.empty();
    }
};

struct xbox360_native_launch {
    std::string binary;
    // argv, starting with the binary: <runtime> <default.xex> <game directory>
    // for an extracted title, <runtime> <package> for a packaged one. Each
    // element is one argument, passed to the child as such - no shell is
    // involved, so a path containing spaces stays a single argument.
    std::vector<std::string> arguments;
    // Variables to set in the child. Only those the parent has not already
    // set, so a value exported before WhittyArcade started still wins.
    std::vector<std::pair<std::string, std::string>> environment;
    std::string error;

    explicit operator bool() const {
        return error.empty() && !binary.empty();
    }
};

// Where the runtime for a title is looked for, most specific first. Exposed so
// a failure can name every path that was tried.
std::vector<std::string> xbox360_native_runtime_candidates(
    const std::string& short_name);

// Resolves the runtime binary and composes the command line and environment the
// title needs. `error` is set, and `binary` left empty, when no runtime is
// installed for the title.
xbox360_native_launch plan_xbox360_native_launch(
    const std::string& short_name, const xbox360_native_content& content);

// A launched title. Owns the child process: destroying this stops the game.
class xbox360_native_process {
public:
    xbox360_native_process() = default;
    ~xbox360_native_process();

    xbox360_native_process(const xbox360_native_process&) = delete;
    xbox360_native_process& operator=(const xbox360_native_process&) = delete;

    bool start(const xbox360_native_launch& launch, std::string& error);
    // False once the title has exited, or before one has been started. Reaps
    // the child, so a finished game does not linger as a zombie.
    bool running();
    void stop();

private:
    std::int64_t m_process{-1};
};
