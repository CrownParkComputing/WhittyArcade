// Tests for launching a natively recompiled Xbox 360 title.
//
// The launch is planned before it is performed precisely so this can be checked
// without a GPU, a window, or the game: everything that decides what runs -
// which binary, which arguments, which environment - is data, and it is the part
// that silently goes wrong. A missing environment default costs the music, a
// swapped argument order hands the runtime a directory where it wants a file,
// and neither shows up as anything but "the game did not start".
//
// The process half is then exercised against a stand-in executable, so the
// spawn/poll/stop path is real without needing a real title - including a
// packaged title whose path contains spaces, where the stand-in reports back how
// many arguments it was actually given.
#include "test_platform.h"
#include "xbox360_native_runtime.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool has_variable(const xbox360_native_launch& launch, const std::string& name,
                  const std::string& value) {
    for (const auto& [key, item] : launch.environment)
        if (key == name) return item == value;
    return false;
}

bool mentions_variable(const xbox360_native_launch& launch,
                       const std::string& name) {
    for (const auto& [key, item] : launch.environment) {
        (void)item;
        if (key == name) return true;
    }
    return false;
}

void write_stub(const fs::path& path, const char* body) {
    std::ofstream script(path);
    script << "#!/bin/sh\n" << body;
    script.close();
    fs::permissions(path, fs::perms::owner_all,
                    fs::perm_options::add);
}

} // namespace

int main() {
    const fs::path root = fs::temp_directory_path() /
        ("manx-xbox360-native-test-" +
         std::to_string(test_process_id()));
    fs::create_directories(root);
    // The search includes the runtime checkout under the user's home, so point
    // HOME at the fixture: what is installed on the machine running the test
    // must not decide whether it passes.
    ::setenv("HOME", root.string().c_str(), 1);

    // A title with no runtime installed reports it, and names what it looked
    // for - a launcher that just says "failed" leaves nowhere to go.
    ::unsetenv("MANX_XENON_RUNTIME");
    ::unsetenv("MANX_XENON_ROOT");
    const xbox360_native_launch absent = plan_xbox360_native_launch(
        "nosuchtitle", xbox360_native_content::extracted(
                           (root / "default.xex").string(), root.string()));
    assert(!absent);
    assert(absent.binary.empty());
    assert(!absent.error.empty());
    const std::vector<std::string> candidates =
        xbox360_native_runtime_candidates("nosuchtitle");
    assert(!candidates.empty());
    for (const std::string& candidate : candidates)
        assert(absent.error.find(candidate) != std::string::npos);

    // The runtime's build tree layout: build/recompiled/<title>/<title>.
    ::setenv("MANX_XENON_ROOT", (root / "runtime").string().c_str(), 1);
    const std::vector<std::string> rooted =
        xbox360_native_runtime_candidates("geometrywars");
    const std::string expected =
        (root / "runtime" / "build" / "recompiled" / "geometrywars" /
         "geometrywars").string();
    bool found_build_tree = false;
    for (const std::string& candidate : rooted)
        found_build_tree = found_build_tree || candidate == expected;
    assert(found_build_tree);

    // A directory is not a runtime, and neither is a file that cannot be run.
    fs::create_directories(root / "runtime" / "build" / "recompiled" /
                           "geometrywars" / "geometrywars");
    assert(!plan_xbox360_native_launch(
        "geometrywars", xbox360_native_content::extracted(
                            (root / "default.xex").string(), root.string())));
    fs::remove_all(root / "runtime");
    const fs::path unreadable = root / "not-executable";
    std::ofstream(unreadable) << "text";
    ::setenv("MANX_XENON_RUNTIME", unreadable.string().c_str(), 1);
    fs::permissions(unreadable, fs::perms::owner_read,
                    fs::perm_options::replace);
    assert(!plan_xbox360_native_launch(
        "geometrywars", xbox360_native_content::extracted(
                            (root / "default.xex").string(), root.string())));

    // A real runtime: the arguments are <binary> <default.xex> <game dir>, in
    // that order, and the play.sh defaults come with it.
    const fs::path stub = root / "geometrywars";
    write_stub(stub, "sleep 30\n");
    ::setenv("MANX_XENON_RUNTIME", stub.string().c_str(), 1);
    ::unsetenv("MANX_WINDOW");
    ::unsetenv("MANX_FPS");
    ::unsetenv("MANX_XMA_BYPASS");
    const std::string game_root = root.string();
    const std::string xex = (root / "default.xex").string();
    const xbox360_native_launch launch = plan_xbox360_native_launch(
        "geometrywars", xbox360_native_content::extracted(xex, game_root));
    assert(launch);
    assert(launch.binary == stub.string());
    assert(launch.arguments.size() == 3);
    assert(launch.arguments[0] == stub.string());
    assert(launch.arguments[1] == xex);
    assert(launch.arguments[2] == game_root);
    assert(has_variable(launch, "MANX_WINDOW", "1"));
    assert(has_variable(launch, "MANX_FPS", "60"));
    // The XMA bypass plays the decoder's output beside the title's own mix, so
    // supplying it would play every sound twice. It is a diagnostic the runtime
    // keeps, not something a launch may turn on behind the player's back.
    assert(!mentions_variable(launch, "MANX_XMA_BYPASS"));

    // A value the user exported before MANX started is theirs to keep:
    // the plan supplies defaults, it does not impose them.
    ::setenv("MANX_FPS", "30", 1);
    const xbox360_native_launch overridden = plan_xbox360_native_launch(
        "geometrywars", xbox360_native_content::extracted(xex, game_root));
    assert(overridden);
    assert(!mentions_variable(overridden, "MANX_FPS"));
    assert(has_variable(overridden, "MANX_WINDOW", "1"));
    ::unsetenv("MANX_FPS");

    // A packaged title is handed to the runtime whole: <binary> <package>, two
    // arguments rather than three, because the package is both the executable
    // and the data. There is no directory to pass and passing one would put the
    // runtime a file short of what it expects.
    //
    // The path is the shape these really have - the console's layout nests the
    // package under the store listing's name, so it contains spaces, and the
    // leaf is a content hash with no extension.
    const fs::path package_directory =
        root / "Geometry Wars Evolved 2" / "584108FF" / "000D0000";
    fs::create_directories(package_directory);
    const std::string package =
        (package_directory / "834312072F4985F9D33D0B9549FFAA32C505FDAD58")
            .string();
    std::ofstream(package, std::ios::binary) << "LIVE";
    assert(package.find(' ') != std::string::npos &&
           "the spaces are the point of this fixture");
    const xbox360_native_launch packaged = plan_xbox360_native_launch(
        "geometrywars", xbox360_native_content::package(package));
    assert(packaged);
    assert(packaged.arguments.size() == 2);
    assert(packaged.arguments[0] == stub.string());
    assert(packaged.arguments[1] == package);
    assert(has_variable(packaged, "MANX_WINDOW", "1"));
    assert(has_variable(packaged, "MANX_FPS", "60"));

    // One runtime binary, two shapes. Space Giraffe was dumped both ways, so it
    // is the first title where the same runtime is handed either a package or a
    // XEX plus a directory depending only on which copy the player picked - and
    // the argument count is the whole difference between the two.
    {
        const fs::path giraffe = root / "spacegiraffe";
        write_stub(giraffe, "sleep 30\n");
        ::setenv("MANX_XENON_RUNTIME", giraffe.string().c_str(), 1);
        const xbox360_native_launch from_package = plan_xbox360_native_launch(
            "spacegiraffe", xbox360_native_content::package(package));
        assert(from_package);
        assert(from_package.binary == giraffe.string());
        assert(from_package.arguments.size() == 2);
        assert(from_package.arguments[1] == package);
        const xbox360_native_launch from_extraction = plan_xbox360_native_launch(
            "spacegiraffe", xbox360_native_content::extracted(xex, game_root));
        assert(from_extraction);
        assert(from_extraction.arguments.size() == 3);
        assert(from_extraction.arguments[1] == xex);
        assert(from_extraction.arguments[2] == game_root);
        ::setenv("MANX_XENON_RUNTIME", stub.string().c_str(), 1);
    }

    // Content that is neither shape, or claims to be both, is not a title.
    assert(!plan_xbox360_native_launch("geometrywars",
                                       xbox360_native_content{}));
    assert(!plan_xbox360_native_launch(
        "geometrywars", xbox360_native_content{xex, game_root, package}));
    assert(!plan_xbox360_native_launch(
        "geometrywars", xbox360_native_content::extracted(xex, {})));

    // And the argument really does reach the child as ONE argument. A path with
    // spaces split by a shell is the failure this whole shape exists to avoid,
    // and only the child can prove it did not happen, so the stand-in runtime
    // reports how many arguments it was given and what the first one was.
    {
        const fs::path report = root / "argv-report";
        const std::string body = "printf '%s\\n' \"$#\" \"$1\" > '" +
                                 report.string() + "'\n";
        const fs::path reporter = root / "reporter";
        write_stub(reporter, body.c_str());
        ::setenv("MANX_XENON_RUNTIME", reporter.string().c_str(), 1);
        const auto run = [&](const xbox360_native_content& content) {
            std::error_code remove_error;
            fs::remove(report, remove_error);
            const xbox360_native_launch plan =
                plan_xbox360_native_launch("geometrywars", content);
            assert(plan);
            xbox360_native_process process;
            std::string error;
            assert(process.start(plan, error));
            for (int attempt = 0; attempt < 200 && process.running(); ++attempt)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            assert(!process.running());
            std::ifstream input(report);
            std::string count;
            std::string first;
            std::getline(input, count);
            std::getline(input, first);
            return std::pair<std::string, std::string>(count, first);
        };
        const auto [package_count, package_first] =
            run(xbox360_native_content::package(package));
        assert(package_count == "1" &&
               "the package path was split into several arguments");
        assert(package_first == package);
        const auto [extracted_count, extracted_first] =
            run(xbox360_native_content::extracted(xex, game_root));
        assert(extracted_count == "2");
        assert(extracted_first == xex);
        ::setenv("MANX_XENON_RUNTIME", stub.string().c_str(), 1);
    }

    // Starting, noticing, and stopping a title.
    {
        xbox360_native_process process;
        std::string error;
        assert(process.start(launch, error));
        assert(error.empty());
        assert(process.running());
        process.stop();
        assert(!process.running());
    }

    // A title that exits on its own is noticed rather than waited on forever.
    {
        const fs::path quick = root / "quick";
        write_stub(quick, "exit 0\n");
        ::setenv("MANX_XENON_RUNTIME", quick.string().c_str(), 1);
        const xbox360_native_launch brief = plan_xbox360_native_launch(
            "geometrywars", xbox360_native_content::extracted(xex, game_root));
        assert(brief);
        xbox360_native_process process;
        std::string error;
        assert(process.start(brief, error));
        bool ended = false;
        for (int attempt = 0; attempt < 100 && !ended; ++attempt) {
            ended = !process.running();
            if (!ended)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        assert(ended && "a finished title was never reported as finished");
    }

    // A plan with nothing in it cannot start anything.
    {
        xbox360_native_process process;
        std::string error;
        assert(!process.start(xbox360_native_launch{}, error));
        assert(!error.empty());
        assert(!process.running());
    }

    ::unsetenv("MANX_XENON_RUNTIME");
    ::unsetenv("MANX_XENON_ROOT");
    fs::remove_all(root);
    std::puts("Xbox 360 native runtime launch: plan and process paths passed");
    return 0;
}
