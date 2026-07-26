// WhittyArcade application shell. Board implementations live in
// arcade_session.cpp and are selected through the canonical catalog.

#include "arcade_audio_output.h"
#include "arcade_catalog.h"
#include "arcade_frontend.h"
#include "arcade_input.h"
#include "arcade_session.h"
#include "arcade_video_worker.h"
#include "twin_window_layout.h"
#include "wall_log.h"

#include <SDL3/SDL.h>
#include "input_mapper.h"
#include "launcher_menu.h"
#include "wall_capacity.h"
#include "multiplayer_lobby.h"
#include "namco/system22/system22_c139_transport.h"
#include "namco/system22/system22_cpu.h"
#include "persistent_data.h"
#include "play_stats.h"
#include "rom_library.h"

#if defined(_WIN32)
#include <SDL3/SDL_main.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <csignal>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

// Three columns is the practical ceiling: each still gets a third of the
// display wide enough to watch, and each is a whole emulated board.
// How many columns this machine can drive, worked out from the processor and
// the display rather than fixed - see wall_capacity.h. Asked once: the answer
// cannot change while a wall is up, and every column must agree on it.
int max_wall_columns() {
    static const int columns = [] {
        int width = 0;
        if (SDL_WasInit(SDL_INIT_VIDEO)) {
            if (const SDL_DisplayMode* mode =
                    SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay()))
                width = mode->w;
        }
        // Before the display is known, assume an ordinary one: a wall is only
        // ever built after the front end has opened, which needs video.
        if (width <= 0) width = 1920;
        return wall_column_capacity(std::thread::hardware_concurrency(),
                                    width);
    }();
    return columns;
}

struct runtime_options {
    int cabinet_node{};
    int cabinet_count{2};
    int wall_slot{};
    int wall_count{};
    uint16_t pair_port_base{35112};
    bool independent_pair{};
    bool twin_screen{};
    bool network_pair{};
    bool twin_one_screen{};
    bool two_player{};
    std::vector<std::string> positional;
};

// This process and the one that started it. Windows has no getppid, and a
// wall column there has no parent to name in its log - the diagnostic still
// works, it just cannot say who spawned it.
int whitty_process_id() {
#if defined(_WIN32)
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

int whitty_parent_process_id() {
#if defined(_WIN32)
    return 0;
#else
    return static_cast<int>(getppid());
#endif
}

bool set_environment(const char* name, const std::string& value) {
#if defined(_WIN32)
    return _putenv_s(name, value.c_str()) == 0;
#else
    return setenv(name, value.c_str(), 1) == 0;
#endif
}

bool unset_environment(const char* name) {
#if defined(_WIN32)
    return _putenv_s(name, "") == 0;
#else
    return unsetenv(name) == 0;
#endif
}

runtime_options parse_runtime_options(int argc, char* argv[]) {
    runtime_options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--cabinet-node" && index + 1 < argc) {
            options.cabinet_node = std::atoi(argv[++index]);
        } else if (argument == "--cabinet-count" && index + 1 < argc) {
            options.cabinet_count = std::atoi(argv[++index]);
        } else if (argument == "--pair-port-base" && index + 1 < argc) {
            const int value = std::atoi(argv[++index]);
            if (value > 1024 && value < 65535)
                options.pair_port_base = static_cast<uint16_t>(value);
        } else if (argument == "--wall-slot" && index + 1 < argc) {
            options.wall_slot = std::atoi(argv[++index]);
        } else if (argument == "--wall-count" && index + 1 < argc) {
            options.wall_count = std::atoi(argv[++index]);
        } else if (argument == "--independent-cabinet") {
            options.independent_pair = true;
        } else if (argument == "--twin-screen") {
            options.twin_screen = true;
        } else if (argument == "--network-cabinet") {
            options.network_pair = true;
        } else if (argument == "--twin-one-screen") {
            options.twin_one_screen = true;
        } else if (argument == "--two-player") {
            options.two_player = true;
        } else {
            options.positional.emplace_back(argument);
        }
    }
    options.cabinet_count = std::clamp(options.cabinet_count, 2, 8);
    if (options.cabinet_node < 1 || options.cabinet_node > options.cabinet_count)
        options.cabinet_node = 0;
    // Anything outside a real wall means no wall. A column process trusts the
    // count its parent chose - the parent measured the machine, and a child
    // second-guessing it would place itself somewhere the parent is not
    // expecting - so only the absolute ceiling is enforced here.
    if (options.wall_count < wall_min_columns ||
        options.wall_count > wall_max_columns) {
        options.wall_count = 0;
        options.wall_slot = 0;
    }
    options.wall_slot =
        std::clamp(options.wall_slot, 0, std::max(options.wall_count - 1, 0));
    return options;
}

// Process handling shared by the linked-cabinet companion and the arcade
// wall columns. Both spawn the same executable with different arguments and
// both need the same graceful shutdown, which the wall previously skipped.
#if defined(_WIN32)
using child_handle = intptr_t;
constexpr child_handle no_child = -1;
#else
using child_handle = pid_t;
constexpr child_handle no_child = -1;
#endif

child_handle spawn_child(const std::string& executable,
                         std::vector<std::string>& arguments) {
    std::vector<char*> native;
    native.reserve(arguments.size() + 1);
    for (std::string& argument : arguments)
        native.push_back(argument.data());
    native.push_back(nullptr);
#if defined(_WIN32)
    return _spawnv(_P_NOWAIT, executable.c_str(), native.data());
#else
    const pid_t child = fork();
    if (child == 0) {
#if defined(__linux__)
        // The ring dies with its owner: if cabinet 1 goes away for any
        // reason - including a crash that never runs its teardown - the
        // kernel delivers SIGTERM here rather than leaving orphans.
        prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
        execvp(executable.c_str(), native.data());
        std::_Exit(127);
    }
    return child;
#endif
}

// Asks the child to exit, then insists. Returns once it is gone.
void terminate_child(child_handle child) {
    if (child == no_child) return;
#if defined(_WIN32)
    HANDLE handle = reinterpret_cast<HANDLE>(child);
    TerminateProcess(handle, 0);
    WaitForSingleObject(handle, 2000);
    CloseHandle(handle);
#else
    int status = 0;
    if (waitpid(child, &status, WNOHANG) == child) return;
    kill(child, SIGTERM);
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (waitpid(child, &status, WNOHANG) == child) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
#endif
}

// Spawns the other columns of an arcade wall, one process per game, each
// placed over its own third of the display.
//
// A process apiece rather than panes of one window, because a column is a
// whole board: its own texture sheets, palettes, vertex buffers and audio
// device. Three of those cannot share one renderer - they would overwrite
// each other's every frame - and the isolation a process gives is exactly
// what makes three Model 2 games side by side possible at all.
class wall_companions {
public:
    ~wall_companions() { stop(); }

    bool start(const std::string& executable,
               const std::vector<std::string>& rom_paths) {
        stop();
        const std::size_t columns =
            std::min<std::size_t>(rom_paths.size(),
                                  static_cast<std::size_t>(
                                      max_wall_columns()));
        // A wall covers the display, so give it a workspace of its own where
        // one is free rather than burying whatever the user had open. Chosen
        // once, here, and inherited by every column: each choosing for itself
        // would race, since the first window to land makes that workspace no
        // longer spare.
        // A new wall starts with no cabinet claimed, so it opens in equal
        // columns rather than inheriting whichever one was played last time.
        whitty_wall_log::begin(0, static_cast<int>(columns));
        whitty_wall_log::note("wall of %zu columns, this process is column 0",
                              columns);
        whitty_window::forget_wall_focus();
        // Before any column exists, so the first one is never tiled even
        // briefly.
        whitty_window::register_wall_window_rules();
#if defined(__linux__)
        m_workspace = whitty_window::find_spare_workspace();
#endif
        if (m_workspace > 0)
            set_environment("WHITTY_WALL_WORKSPACE",
                            std::to_string(m_workspace));
        for (std::size_t index = 1; index < columns; ++index) {
            std::vector<std::string> arguments{
                executable,
                "--wall-slot", std::to_string(index),
                "--wall-count", std::to_string(columns),
                rom_paths[index],
            };
            const child_handle child = spawn_child(executable, arguments);
            if (child == no_child || child < 0) {
                std::fprintf(stderr,
                             "Could not start arcade wall column %zu (%s)\n",
                             index, rom_paths[index].c_str());
                whitty_wall_log::note("column %zu FAILED TO SPAWN (%s)",
                                      index, rom_paths[index].c_str());
                continue;
            }
            whitty_wall_log::note("spawned column %zu pid %d (%s)", index,
                                  static_cast<int>(child),
                                  rom_paths[index].c_str());
            m_children.push_back(child);
        }
        return !m_children.empty();
    }

    void stop() {
        for (child_handle child : m_children) terminate_child(child);
        m_children.clear();
        whitty_window::forget_wall_focus();
        if (m_workspace > 0) unset_environment("WHITTY_WALL_WORKSPACE");
        m_workspace = 0;
    }

    // Brings the user to the wall once every column has been sent there, so
    // the desktop does not flick between workspaces as each one starts.
    void show() const {
#if defined(__linux__)
        whitty_window::show_workspace(m_workspace);
#endif
    }

private:
    std::vector<child_handle> m_children;
    int m_workspace{0};
};

void configure_cabinet_environment(int node, int count, uint16_t port_base,
                                   bool linked_model2,
                                   bool linked_system22 = false,
                                   bool network_link = false,
                                   bool generic_input_link = false) {
    count = std::clamp(count, 2, 8);
    if (node < 1 || node > count) {
        unset_environment("WHITTY_CABINET_NODE");
        unset_environment("WHITTY_CABINET_COUNT");
        unset_environment("MODEL2_COMM_NODE");
        unset_environment("MODEL2_COMM_COUNT");
        unset_environment("MODEL2_COMM_LOCAL_PORT");
        unset_environment("MODEL2_COMM_PEER_PORT");
        unset_environment("MODEL2_COMM_NETWORK");
        unset_environment("SYSTEM22_C139_NODE");
        unset_environment("SYSTEM22_C139_LOCAL_PORT");
        unset_environment("SYSTEM22_C139_PEER_PORT");
        unset_environment("SYSTEM22_C139_NETWORK");
        unset_environment("WHITTY_INPUT_LINK");
        unset_environment("WHITTY_INPUT_LOCAL_PORT");
        unset_environment("WHITTY_INPUT_PEER_PORT");
        unset_environment("WHITTY_VIDEO_ROLE");
        unset_environment("WHITTY_VIDEO_PORT");
        unset_environment("RRACER_CONTROLLER");
        return;
    }
    const uint16_t local_port =
        static_cast<uint16_t>(port_base + (node == 2 ? 1 : 0));
    const uint16_t peer_port =
        static_cast<uint16_t>(port_base + (node == 1 ? 1 : 0));
    set_environment("WHITTY_CABINET_NODE", std::to_string(node));
    set_environment("WHITTY_CABINET_COUNT", std::to_string(count));
    if (linked_model2) {
        // The comm board is a ring of up to 8 cabinets: node N listens on
        // base+N-1 and transmits to its successor, wrapping back to the
        // master. For two cabinets this reduces to the classic pair.
        set_environment("MODEL2_COMM_NODE", std::to_string(node));
        set_environment("MODEL2_COMM_COUNT", std::to_string(count));
        set_environment("MODEL2_COMM_LOCAL_PORT",
                        std::to_string(port_base + node - 1));
        set_environment("MODEL2_COMM_PEER_PORT",
                        std::to_string(port_base + node % count));
        if (network_link)
            set_environment("MODEL2_COMM_NETWORK", "1");
        else
            unset_environment("MODEL2_COMM_NETWORK");
    } else {
        unset_environment("MODEL2_COMM_NODE");
        unset_environment("MODEL2_COMM_COUNT");
        unset_environment("MODEL2_COMM_LOCAL_PORT");
        unset_environment("MODEL2_COMM_PEER_PORT");
        unset_environment("MODEL2_COMM_NETWORK");
    }
    if (linked_system22) {
        set_environment("SYSTEM22_C139_NODE", std::to_string(node));
        set_environment("SYSTEM22_C139_LOCAL_PORT",
                        std::to_string(local_port));
        set_environment("SYSTEM22_C139_PEER_PORT",
                        std::to_string(peer_port));
        if (network_link)
            set_environment("SYSTEM22_C139_NETWORK", "1");
        else
            unset_environment("SYSTEM22_C139_NETWORK");
    } else {
        unset_environment("SYSTEM22_C139_NODE");
        unset_environment("SYSTEM22_C139_LOCAL_PORT");
        unset_environment("SYSTEM22_C139_PEER_PORT");
        unset_environment("SYSTEM22_C139_NETWORK");
    }
    if (generic_input_link) {
        set_environment("WHITTY_INPUT_LINK", "1");
        set_environment("WHITTY_INPUT_LOCAL_PORT",
                        std::to_string(local_port));
        set_environment("WHITTY_INPUT_PEER_PORT",
                        std::to_string(peer_port));
        // Netplay: both machines simulate the whole board from the same
        // inputs, so no picture is sent. The video link stays in the tree
        // for the streaming path but is not started here.
        set_environment("WHITTY_NETPLAY", "1");
        unset_environment("WHITTY_VIDEO_ROLE");
        unset_environment("WHITTY_VIDEO_PORT");
    } else {
        unset_environment("WHITTY_INPUT_LINK");
        unset_environment("WHITTY_INPUT_LOCAL_PORT");
        unset_environment("WHITTY_INPUT_PEER_PORT");
        unset_environment("WHITTY_NETPLAY");
        unset_environment("WHITTY_VIDEO_ROLE");
        unset_environment("WHITTY_VIDEO_PORT");
    }
    set_environment("RRACER_CONTROLLER", std::to_string(node - 1));
}

uint16_t choose_pair_port_base() {
#if defined(_WIN32)
    const unsigned process_id = GetCurrentProcessId();
#else
    const unsigned process_id = static_cast<unsigned>(getpid());
#endif
    // Eight consecutive ports per launch - one per possible ring cabinet -
    // with bases spaced so two concurrent launches can never overlap.
    return static_cast<uint16_t>(36000 + (process_id % 3000) * 8);
}

class cabinet_companion {
public:
    ~cabinet_companion() { stop(); }

    // Spawns cabinets 2..count; each is a full machine of its own on the
    // comm ring. A classic twin is count == 2.
    bool start(const std::string& executable, const std::string& rom_path,
               const std::string& bios_path, bool explicit_bios_path,
               uint16_t port_base, bool independent_pair,
               bool one_screen = false, bool two_player = false,
               int count = 2) {
        stop();
        bool all_started = true;
        for (int node = 2; node <= std::clamp(count, 2, 8); ++node) {
            std::vector<std::string> arguments{
                executable,
                "--cabinet-node", std::to_string(node),
                "--cabinet-count", std::to_string(std::clamp(count, 2, 8)),
                "--pair-port-base", std::to_string(port_base),
            };
            if (independent_pair)
                arguments.emplace_back("--independent-cabinet");
            // The other cabinets load settings.ini for themselves, so the
            // shared-display choice has to travel on the command line.
            if (one_screen) arguments.emplace_back("--twin-one-screen");
            if (two_player) arguments.emplace_back("--two-player");
            arguments.push_back(rom_path);
            if (explicit_bios_path) arguments.push_back(bios_path);
            const child_handle process = spawn_child(executable, arguments);
            if (process != no_child && process >= 0)
                m_processes.push_back(process);
            else
                all_started = false;
        }
        return all_started && !m_processes.empty();
    }

    void stop() {
        for (child_handle& process : m_processes) terminate_child(process);
        m_processes.clear();
    }

    bool running() const { return !m_processes.empty(); }

    // True once any spawned cabinet has exited. A ring with a missing
    // member is already broken - its comm loop is cut - so the owner
    // tears the whole session down rather than racing ghosts.
    bool any_exited() {
#if defined(_WIN32)
        return false;
#else
        for (child_handle process : m_processes) {
            int status = 0;
            if (process != no_child &&
                waitpid(process, &status, WNOHANG) == process)
                return true;
        }
        return false;
#endif
    }

private:
    std::vector<child_handle> m_processes;
};

int run_video_lifecycle_test() {
    emulator_settings settings;
    auto video = std::make_shared<arcade_video_worker>();
    if (!video->initialize(settings)) return 1;
    constexpr std::size_t test_frame_bytes =
        std::size_t{640} * 480 * 4;
    std::vector<uint8_t> frame(test_frame_bytes, 0x40);
    for (int session = 0; session < 20; ++session) {
        if (!video->initialize(settings)) return 2;
        const int width = (session & 1) ? 320 : 640;
        const int height = (session & 1) ? 224 : 480;
        video->present_rgba_frame(frame.data(), width, height);
        video->reset_session();
        if (video->process_events() !=
            arcade_host_action::continue_running)
            return 3;
    }
    video->shutdown();
    std::puts("Persistent video worker: 20 session boundaries passed");
    return 0;
}

int run_session_factory_test() {
    auto video = std::make_shared<arcade_video_worker>();
    auto cabinet = std::make_shared<arcade_cabinet_state>();
    std::size_t sessions_created = 0;
    constexpr int rounds = 25;
    for (int round = 0; round < rounds; ++round) {
        cabinet->system22_dip_switches = static_cast<uint16_t>(0xff00 | round);
        for (const arcade_board_descriptor& board : arcade_boards()) {
            {
#if defined(WHITTY_SYSTEM246_STUBBED)
                // This build has no PCSX2 core - no prebuilt libraries, or a
                // target it has no backend for - so System 246 is present in
                // the catalog but cannot open a session. Skipping it keeps
                // every other board strictly checked; the alternative is a
                // test that fails on any build without a proprietary core.
                if (board.type == arcade_board_type::system246) continue;
#endif
                std::unique_ptr<emulator_session> session =
                    create_emulator_session(board.type, video, cabinet);
                if (!session || session->board_type() != board.type) return 1;
                ++sessions_created;
            }
            // Destruction must release the process-owned output immediately.
            if (video.use_count() != 1) return 2;
        }
        if (cabinet->system22_dip_switches !=
            static_cast<uint16_t>(0xff00 | round))
            return 3;
    }
    bool rejected_invalid_session = false;
    try {
        create_emulator_session(static_cast<arcade_board_type>(0xff),
                                video, cabinet);
    } catch (const std::invalid_argument&) {
        rejected_invalid_session = true;
    }
    if (!rejected_invalid_session) return 4;

    bool rejected_missing_video = false;
    try {
        create_emulator_session(arcade_board_type::system22, nullptr,
                                cabinet);
    } catch (const std::invalid_argument&) {
        rejected_missing_video = true;
    }
    if (!rejected_missing_video) return 5;
    std::printf("Session factory: %zu construct/destroy boundaries passed\n",
                sessions_created);
#if defined(WHITTY_SYSTEM246_STUBBED)
    constexpr std::size_t buildable_boards = arcade_board_count - 1;
#else
    constexpr std::size_t buildable_boards = arcade_board_count;
#endif
    return sessions_created == buildable_boards * rounds ? 0 : 6;
}

// Standalone end-to-end test for the System 22 C139 cabinet-to-cabinet
// link transport. Spins up two system22_bus + two system22_c139_transport
// instances on real UDP loopback, drives a known frame from cabinet 1's
// TX FIFO, and asserts that cabinet 2's RX FIFO receives it with the
// expected bit-9 sync mark. No ROM load, no GPU, no display — pure
// network + bus path. Run as `WhittyArcade --c139-link-test`.
int run_c139_link_test() {
    system22_bus cabinet1;
    system22_bus cabinet2;
    cabinet1.set_c139_link(true, 1);
    cabinet2.set_c139_link(true, 2);

    // Use non-default UDP ports so the test is repeatable and doesn't
    // conflict with a real Ridge Racer 2 cabinet link that the user
    // might be running. 17512 / 17513 are well above the 1024 floor
    // and unlikely to collide with anything else.
    set_environment("SYSTEM22_C139_LOCAL_PORT", "17512");
    set_environment("SYSTEM22_C139_PEER_PORT",  "17513");
    // Cabinet 1 sits on 17512, sends to 17513. We re-set the env for
    // cabinet 2 so the local/peer roles swap.
    system22_c139_transport a;
    if (!a.initialize(cabinet1, nullptr, /*forced_node=*/1)) {
        std::fprintf(stderr, "c139 link test: transport a init failed\n");
        return 1;
    }
    set_environment("SYSTEM22_C139_LOCAL_PORT", "17513");
    set_environment("SYSTEM22_C139_PEER_PORT",  "17512");
    system22_c139_transport b;
    if (!b.initialize(cabinet2, nullptr, /*forced_node=*/2)) {
        std::fprintf(stderr, "c139 link test: transport b init failed\n");
        return 1;
    }
    if (!a.enabled() || !b.enabled()) {
        std::fprintf(stderr, "c139 link test: transports not enabled "
                              "after forced_node init\n");
        return 2;
    }
    // Drive a known frame into cabinet 1's TX FIFO via the 68020-side
    // register file (mirrors the test in tests/c139_transport_test.cpp).
    cabinet1.write16(0x20020004, 0x0001);
    cabinet1.write16(0x20010000, 0x0011);
    cabinet1.write16(0x20010002, 0x0022);
    cabinet1.write16(0x20010004, 0x0033);
    cabinet1.write16(0x2002000a, 0x0003);
    cabinet1.write16(0x20020004, 0x0003);

    for (int round = 0; round < 5; ++round) {
        a.exchange(cabinet1);
        b.exchange(cabinet2);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!a.linked() || !b.linked()) {
        std::fprintf(stderr, "c139 link test: transports never linked "
                              "(a=%d, b=%d)\n", a.linked(), b.linked());
        return 3;
    }
    if ((cabinet2.read16(0x20020000) & 0x0002) == 0) {
        std::fprintf(stderr, "c139 link test: cabinet 2 RX-ready bit not set\n");
        return 4;
    }
    if (cabinet2.read16(0x2002000c) != 3) {
        std::fprintf(stderr, "c139 link test: cabinet 2 RX word count = %u, "
                              "expected 3\n",
                     cabinet2.read16(0x2002000c));
        return 5;
    }
    // receive_c139_frame takes only the low byte of each u16 word;
    // the bus sets the bit-9 sync mark on the last word. So 0x0011 ->
    // (hi=0x00, lo=0x11), 0x0022 -> (hi=0x00, lo=0x22), 0x0033+sync
    // -> (hi=0x01, lo=0x33).
    if (cabinet2.read16(0x20012000) != 0x0011 ||
        cabinet2.read16(0x20012002) != 0x0022 ||
        cabinet2.read16(0x20012004) != 0x0133) {
        std::fprintf(stderr, "c139 link test: cabinet 2 RX FIFO mismatch\n");
        return 6;
    }
    std::printf("c139 link test: 3-word frame delivered end-to-end\n");
    return 0;
}

int audit_roms(int argc, char* argv[]) {
    std::vector<rom_choice> choices;
    if (argc > 2) {
        for (int index = 2; index < argc; ++index) {
            const auto identity = identify_arcade_game(argv[index]);
            rom_choice choice;
            choice.path = argv[index];
            choice.label = argv[index];
            choice.board = identity ? identity->board
                                    : arcade_board_type::system22;
            choices.push_back(std::move(choice));
        }
    } else {
        choices = discover_library_roms({});
    }
    if (choices.empty()) {
        std::fprintf(stderr, "No supported ROM sets found.\n");
        return 1;
    }
    bool all_valid = true;
    for (const rom_choice& choice : choices) {
        const rom_audit_result audit = audit_rom_path(choice.path);
        std::printf("%s  %s: %s\n", audit.success ? "OK" : "FAIL",
                    audit.set_name.empty() ? choice.path.c_str() :
                                             audit.set_name.c_str(),
                    audit.message.c_str());
        all_valid = all_valid && audit.success;
    }
    return all_valid ? 0 : 1;
}

std::optional<int> run_tool_command(int argc, char* argv[]) {
    if (argc <= 1) return std::nullopt;
    const std::string_view command(argv[1]);
    if (command == "--video-lifecycle-test")
        return run_video_lifecycle_test();
    if (command == "--session-factory-test")
        return run_session_factory_test();
    if (command == "--c139-link-test")
        return run_c139_link_test();
    if (command == "--list-roms") {
        std::printf("%s\n", required_rom_sets_text().c_str());
        return 0;
    }
    if (command == "--audit-roms") return audit_roms(argc, argv);
    return std::nullopt;
}

} // namespace

int main(int argc, char* argv[]) {
    if (const std::optional<int> result = run_tool_command(argc, argv))
        return *result;

    const runtime_options runtime = parse_runtime_options(argc, argv);
    std::string rom_path = runtime.positional.empty() ?
        std::string{} : runtime.positional[0];
    std::string bios_path;
    const bool explicit_bios_path = runtime.positional.size() > 1;
    if (explicit_bios_path) {
        bios_path = runtime.positional[1];
    } else {
        std::error_code error;
        const fs::path candidate(rom_path);
        bios_path = fs::is_regular_file(candidate, error) ?
            candidate.parent_path().string() : rom_path;
    }

    emulator_settings settings = load_settings();
    // Host video survives ROM changes. Machine state does not: the factory
    // creates fresh CPUs, RAM, input and sound devices for every selection.
    auto shared_video = std::make_shared<arcade_video_worker>();
    auto cabinet_state = std::make_shared<arcade_cabinet_state>();
    cabinet_companion companion;
    cabinet_launch_mode launch_mode =
        runtime.twin_screen ? cabinet_launch_mode::independent_pair :
        (runtime.cabinet_node ?
            (runtime.network_pair ? cabinet_launch_mode::linked_network :
                (runtime.independent_pair ?
                    cabinet_launch_mode::independent_pair :
                    cabinet_launch_mode::linked_pair)) :
            cabinet_launch_mode::single);
    int cabinet_node = runtime.cabinet_node;
    int cabinet_count = runtime.cabinet_count;
    bool one_screen_pair = runtime.twin_one_screen;
    // Set by the launch, not by the board: a session begun as two-player
    // must begin a two-player game. The second cabinet process is told on
    // its command line, so both halves of a pair agree.
    bool two_player_session = runtime.two_player;
    wall_companions wall;
    int wall_audible_applied = -1;
    settings.wall_slot = runtime.wall_slot;
    settings.wall_count = runtime.wall_count;
    whitty_wall_log::begin(runtime.wall_slot, runtime.wall_count);
    // A cabinet of a linked pair keeps its own file for the same reason a
    // wall column does - nobody can read the second one's stderr.
    whitty_wall_log::begin_cabinet(runtime.cabinet_node);
    // Whatever this launch turns out to be, it writes somewhere.
    whitty_wall_log::begin_session();
    if (runtime.wall_count > 1)
        whitty_wall_log::note("spawned column, rom=%s pid=%d ppid=%d",
                              runtime.positional.empty()
                                  ? "(none)"
                                  : runtime.positional.front().c_str(),
                              static_cast<int>(whitty_process_id()),
                              static_cast<int>(whitty_parent_process_id()));
#if defined(__linux__)
    // SDL turns SIGINT/SIGTERM into SDL_EVENT_QUIT, which is how a wall
    // column that is signalled looks exactly like one the user closed. Taking
    // the signals first records who sent it, then does what SDL would have.
    if (runtime.wall_count > 1) {
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
        struct sigaction action {};
        action.sa_flags = SA_SIGINFO;
        action.sa_sigaction = [](int number, siginfo_t* info, void*) {
            whitty_wall_log::note("SIGNAL %d from pid %d uid %d", number,
                                  info ? static_cast<int>(info->si_pid) : -1,
                                  info ? static_cast<int>(info->si_uid) : -1);
            std::_Exit(0);
        };
        sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, nullptr);
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGHUP, &action, nullptr);
    }
#endif
    uint16_t pair_port_base = runtime.pair_port_base;
    bool restart = true;
    bool startup_menu = runtime.positional.empty();
    std::unique_ptr<multiplayer_lobby> lobby;
    if (startup_menu)
        lobby = std::make_unique<multiplayer_lobby>();

    while (restart) {
        restart = false;
        if (startup_menu) {
            startup_menu = false;
            rom_selection_result selection =
                show_rom_selector(rom_path, lobby.get());
            if (selection.action == rom_selection_action::selected) {
                // The first-run/settings UI may have changed the persisted
                // library paths. Reload the full settings record before
                // applying this launch's runtime-only display choices so a
                // later volume/display save cannot overwrite those paths.
                settings = load_settings();
                rom_path = std::move(selection.path);
                launch_mode = selection.launch_mode;
                cabinet_node = selection.cabinet_node;
                cabinet_count = std::clamp(selection.cabinet_count, 2, 8);
                settings.twin_separate_monitors =
                    selection.twin_separate_monitors;
                one_screen_pair = selection.twin_one_screen;
                two_player_session = selection.two_player;
                // An arcade wall runs a different game per column. This
                // process takes column 0 and spawns one plain single-cabinet
                // launch per remaining column; nothing is shared between
                // them, so they need no link, port or node.
                // An arcade wall runs a different game per column. This
                // process takes column 0 and spawns one plain single-cabinet
                // launch per remaining column; nothing is shared between
                // them, so they need no link, port or node.
                if (selection.wall_games.size() > 1) {
                    settings.wall_count = static_cast<int>(
                        std::min<std::size_t>(
                            selection.wall_games.size(),
                            static_cast<std::size_t>(max_wall_columns())));
                    settings.wall_slot = 0;
                    rom_path = selection.wall_games[0];
                    wall.start(argv[0], selection.wall_games);
                    // Every column has been told where to go; bring the user
                    // there once rather than following each one as it starts.
                    wall.show();
                    if (!explicit_bios_path)
                        bios_path = fs::path(rom_path).parent_path().string();
                }
                if (launch_mode == cabinet_launch_mode::linked_network)
                    pair_port_base = 35112;
                if (!explicit_bios_path)
                    bios_path = fs::path(rom_path).parent_path().string();
            } else if (selection.action ==
                       rom_selection_action::exit_requested) {
                return 0;
            }
        }

        whitty_wall_log::note("identifying %s", rom_path.c_str());
        const std::optional<arcade_game_identity> identity =
            identify_arcade_game(rom_path);
        if (!identity) {
            std::fprintf(stderr,
                         "Unsupported or incomplete ROM archive: %s\n",
                         rom_path.c_str());
            return 1;
        }

        // Which cabinet link a game uses is decided by its board, not by its
        // name: System 22 racers share Namco's C139 SCI cable, Model 2 games
        // use Sega's comm board. The catalog says a game is linkable; the
        // board says which cable that means.
        //
        // Both were previously derived independently - one from a hardcoded
        // short name, the other from the catalog flag alone - so Sega Rally
        // matched both and every linked launch configured the C139 as well as
        // the comm board, on the same pair of UDP ports.
        const rom_set_manifest* launch_manifest =
            find_supported_rom_set(identity->short_name);
        const bool catalog_linkable =
            launch_manifest &&
            launch_manifest->multiplayer ==
                arcade_multiplayer_mode::native_link;
        const bool native_model2_link =
            catalog_linkable && identity->board == arcade_board_type::model2;
        const bool native_system22_link =
            catalog_linkable && identity->board == arcade_board_type::system22;
        const bool native_hardware_link =
            native_model2_link || native_system22_link;
        // launch_manifest is also used below for the two-player input check;
        // keep the original declaration shape so the rest of the function is
        // untouched.
        const bool supports_two_player = launch_manifest &&
            launch_manifest->multiplayer != arcade_multiplayer_mode::none;
        if (launch_mode == cabinet_launch_mode::linked_network &&
            !supports_two_player) {
            std::fprintf(stderr,
                         "%s has no supported Player 2 input path; "
                         "starting single-screen mode instead\n",
                         identity->short_name.c_str());
            launch_mode = cabinet_launch_mode::single;
            cabinet_node = 0;
        }
        const bool local_pair =
            launch_mode == cabinet_launch_mode::linked_pair;
        // Check the machine before promising each cabinet a display: a
        // two-screen linked launch on a one-monitor host would stack two
        // fullscreen windows on top of each other. With fewer displays
        // than cabinets, fall back to the shared-screen layout - windowed
        // halves for a pair, compositor tiling beyond that. Decided before
        // the spawn so both processes agree.
        if (cabinet_node == 0 && !one_screen_pair && cabinet_count <= 2 &&
            (local_pair ||
             launch_mode == cabinet_launch_mode::linked_network)) {
            int display_count = 0;
            if (SDL_DisplayID* displays = SDL_GetDisplays(&display_count))
                SDL_free(displays);
            if (display_count > 0 && display_count < cabinet_count) {
                whitty_wall_log::note(
                    "only %d display(s) for %d cabinets: sharing one screen",
                    display_count, cabinet_count);
                one_screen_pair = true;
            }
        }
        if (cabinet_node == 0 && local_pair) {
            pair_port_base = choose_pair_port_base();
            // Opened before the spawn, not after it: a pair that fails to
            // start is exactly the case worth having a file for, and if this
            // file is missing afterwards then this process never took the
            // linked-pair path at all - which is an answer in itself.
            whitty_wall_log::begin_cabinet(1);
            whitty_wall_log::note("linked launch: spawning cabinets 2-%d, "
                                  "port base %u, one screen %d",
                                  cabinet_count, pair_port_base,
                                  one_screen_pair ? 1 : 0);
            if (!companion.start(argv[0], rom_path, bios_path,
                                 explicit_bios_path, pair_port_base,
                                 false, one_screen_pair, true,
                                 cabinet_count)) {
                std::fprintf(stderr,
                             "Could not start the second cabinet process (execv"
                             "p failed). WhittyArcade was launched as \"%s\". "
                             "Make sure the executable is found via PATH, or "
                             "use an absolute path (e.g. ./WhittyArcade).\n",
                             argv[0] ? argv[0] : "(null)");
                launch_mode = cabinet_launch_mode::single;
                one_screen_pair = false;
            } else {
                cabinet_node = 1;
                whitty_wall_log::note("cabinet 2 started");
            }
        }
        // Every launch that began as two-player says so, whether the two
        // players share this cabinet, a split screen, two monitors or two
        // machines. The board's start button then starts a two-player game.
        if (two_player_session ||
            launch_mode == cabinet_launch_mode::independent_pair ||
            launch_mode == cabinet_launch_mode::linked_pair ||
            launch_mode == cabinet_launch_mode::linked_network)
            set_environment("WHITTY_TWO_PLAYER", "1");
        else
            unset_environment("WHITTY_TWO_PLAYER");
        configure_cabinet_environment(
            cabinet_node, cabinet_count, pair_port_base,
            native_model2_link &&
                (launch_mode == cabinet_launch_mode::linked_pair ||
                 launch_mode == cabinet_launch_mode::linked_network),
            native_system22_link &&
                (launch_mode == cabinet_launch_mode::linked_pair ||
                 launch_mode == cabinet_launch_mode::linked_network),
            launch_mode == cabinet_launch_mode::linked_network,
            !native_hardware_link &&
                launch_mode == cabinet_launch_mode::linked_network);
        settings.output =
            launch_mode == cabinet_launch_mode::independent_pair ?
                output_mode::dual : output_mode::single;
        // Fullscreen is decided by the launch, not by settings.ini: a game
        // always takes the whole screen. The two exceptions are launches
        // where several windows must share one display - a wall column
        // (one of several tiled across the display) and linked cabinets
        // that were asked to share a single screen, where each window owns
        // half the desktop (a pair) or the compositor tiles them (a bigger
        // ring). A linked cabinet with its own display goes fullscreen on
        // it like any single game. Forcing the value here also stops a
        // fullscreen=0 left behind by the in-game toggle from carrying
        // into the next launch.
        // Linked cabinets sharing one display are laid out as one
        // borderless surface split into cells - halves for a pair, a grid
        // for a ring - so together they read as a single fullscreen wall
        // rather than stacked windows or floating tiles. A cabinet with a
        // display of its own goes ordinarily fullscreen on it.
        const bool cabinets_share_display =
            cabinet_node != 0 &&
            (launch_mode == cabinet_launch_mode::linked_pair ||
             launch_mode == cabinet_launch_mode::linked_network) &&
            (one_screen_pair || cabinet_count > 2);
        settings.fullscreen =
            settings.wall_count <= 1 && !cabinets_share_display;
        // Escape hatch for bring-up: a windowed presenter window starts
        // hidden and is only shown after its first successful present, so a
        // presentation fault and a window that never appears look the same.
        // Forcing fullscreen maps the window up front and tells the two
        // apart.
        if (const char* force = std::getenv("WHITTY_FORCE_FULLSCREEN"))
            if (*force == '1' && settings.wall_count <= 1)
                settings.fullscreen = true;
        // Two linked cabinets sharing one display, half the desktop each.
        // The half-desktop split only makes sense for a pair; a bigger ring
        // runs every cabinet as an ordinary window on the main display and
        // the compositor tiles them.
        settings.twin_one_screen = cabinets_share_display;
        if (cabinet_node) {
            // linked_network spreads the two cabinets across different
            // physical displays (cabinet 1 → display 0, cabinet 2 →
            // display 1) so a single ultrawide or two side-by-side
            // monitors each get their own window without the user
            // needing to drag one out from under the other. The earlier
            // code pinned both to display 0, which made the two
            // processes stack on top of each other on a single-screen
            // setup.
            // One Screen instead keeps both cabinets on display 0 and lets
            // the half-desktop placement separate them.
            //
            // A cabinet must never target a display that does not exist:
            // a window sent there stalls that cabinet, which cuts the comm
            // ring, leaving only the adjacent survivors linked. Cabinets
            // spread across the displays the machine actually has and wrap
            // beyond that - on one monitor a ring simply stacks its
            // fullscreen cabinets.
            int display_count = 0;
            if (SDL_DisplayID* displays = SDL_GetDisplays(&display_count))
                SDL_free(displays);
            if (display_count < 1) display_count = 1;
            settings.display_index = settings.twin_one_screen ?
                0 : (cabinet_node - 1) % display_count;
            if (settings.twin_one_screen) {
                std::printf("Starting cabinet %d on the %s half of display 1\n",
                            cabinet_node,
                            cabinet_node == 1 ? "left" : "right");
            } else {
                std::printf("Starting cabinet %d on display %d\n",
                            cabinet_node, settings.display_index + 1);
            }
        } else {
            settings.display_index = -1;
        }

        emulator_settings session_settings = settings;
        // Player 2 contributes controls to Player 1 and presents Player 1's
        // authoritative picture. Silence the hidden local board so two
        // unsynchronised audio timelines can never be heard together.
        // Under netplay both cabinets run the whole board and are heard;
        // silencing Player 2 belonged to the streaming era, when its local
        // board was a puppet behind a received picture.

        std::unique_ptr<emulator_session> emu = create_emulator_session(
            identity->board, shared_video, cabinet_state);
        whitty_wall_log::note("initialising board for %s",
                              identity->short_name.c_str());
        if (!emu->initialize(rom_path, bios_path, session_settings)) {
            whitty_wall_log::note("initialise FAILED");
            return 1;
        }
        whitty_wall_log::note("board running");
        if (launch_mode == cabinet_launch_mode::linked_network &&
            !native_hardware_link) {
            // Netplay starts from identical memory or not at all. The 2D
            // boards keep nothing between sessions and hash to zero, so
            // Galaxian and its kin simply never disagree; the boards that do
            // keep operator data (System 22, Model 1/2, System 246) report
            // it here and a mismatch is named before the first frame.
            const std::uint64_t persistent =
                persistent_state_hash(identity->short_name);
            if (persistent != 0)
                arcade_input_netplay_publish_state(0, persistent);
        }
        if (cabinet_node && native_hardware_link) {
            // A linked cabinet is black for the first few seconds while its
            // board boots and the two look for each other, and a black window
            // that says nothing is indistinguishable from a hung one. Say
            // which cabinet this is and what it is doing; the comm board
            // prints on stderr if the four-second search finds nobody.
            shared_video->set_cabinet_status(
                cabinet_count > 2 ?
                    "CABINET " + std::to_string(cabinet_node) +
                        "  |  STARTING UP" :
                cabinet_node == 2 ?
                    std::string("PLAYER 2 CABINET  |  STARTING UP - "
                                "INSERT COIN TO JOIN") :
                    std::string("PLAYER 1 CABINET  |  STARTING UP - "
                                "INSERT COIN TO PLAY"));
        } else if (launch_mode == cabinet_launch_mode::independent_pair) {
            shared_video->set_cabinet_status(
                "TWIN SCREEN  |  PLAYER 1 + PLAYER 2");
        } else if (cabinet_node &&
                   launch_mode == cabinet_launch_mode::linked_network &&
                   !native_hardware_link) {
            shared_video->set_cabinet_status(
                "PLAYER " + std::to_string(cabinet_node) +
                "  |  WAITING FOR NETWORK PLAYER...");
        }
        const std::vector<rom_choice> installed_games =
            cabinet_node == 2 ? std::vector<rom_choice>{} :
                                discover_rom_choices(rom_path);
        emu->set_rom_choices(installed_games);
        const std::string current_game_short_name(identity->short_name);
        // Names the board so its cabinet bezel can be fetched in the
        // background. Decoration only: the launch never waits on it.
        shared_video->set_board_name(current_game_short_name);
        // The launcher's icon for this game is a frame the session itself
        // presents, captured once, half a minute in. Cabinet 1 owns it -
        // a spawned partner cabinet would capture the same game twice.
        if (cabinet_node <= 1)
            shared_video->arm_title_capture(current_game_short_name);
        // Feeds the launcher's Most Played view. After initialize succeeds,
        // so an unbootable set is never counted as a play.
        record_play(current_game_short_name);

        long long wall_frames = 0;
        // Speed sampling for a wall column: counted over a window, because a
        // single slow frame is a scheduling hiccup and only a sustained
        // shortfall means the wall is wider than the machine.
        // Cleared once the board has been running long enough to be past
        // the comm board's four-second discovery window - after which the
        // status is either wrong or has been replaced by something truer.
        bool startup_status_shown = cabinet_node && native_hardware_link;
        const auto session_began = std::chrono::steady_clock::now();
        int wall_window_frames = 0;
        bool wall_reported_slow = false;
        auto wall_rate_since = std::chrono::steady_clock::now();
        std::string last_netplay_status;
        bool netplay_muted = false;
        auto deadline = std::chrono::steady_clock::now();
        arcade_host_action host_action =
            arcade_host_action::continue_running;
        bool generic_peer_visible = false;
        int displayed_turn = -2;
        while ((host_action = emu->process_events()) ==
               arcade_host_action::continue_running) {
            if (cabinet_node &&
                launch_mode == cabinet_launch_mode::linked_network &&
                !native_hardware_link) {
                const bool connected = arcade_input_network_peer_seen();
                int active_turn = -1;
                if (launch_manifest &&
                    launch_manifest->multiplayer ==
                        arcade_multiplayer_mode::simultaneous) {
                    active_turn = 0;
                } else if (launch_manifest &&
                           launch_manifest->multiplayer ==
                               arcade_multiplayer_mode::alternating) {
                    if (cabinet_node == 1) {
                        active_turn = emu->active_player();
                        arcade_input_set_authoritative_player(
                            active_turn == 1 || active_turn == 2 ?
                                static_cast<uint8_t>(active_turn) : 0);
                    } else {
                        active_turn = static_cast<int>(
                            arcade_input_network_authoritative_player());
                    }
                }
                if (connected != generic_peer_visible ||
                    active_turn != displayed_turn) {
                    generic_peer_visible = connected;
                    displayed_turn = active_turn;
                    std::string status =
                        "CABINET " + std::to_string(cabinet_node) +
                        (connected ?
                            "  |  NETWORK CONNECTED" :
                            "  |  WAITING FOR NETWORK PLAYER...");
                    if (connected) {
                        if (launch_manifest &&
                            launch_manifest->multiplayer ==
                                arcade_multiplayer_mode::simultaneous)
                            status += "  |  BOTH PLAYERS ACTIVE";
                        else if (active_turn == 1 || active_turn == 2)
                            status += "  |  PLAYER " +
                                std::to_string(active_turn) + " TURN";
                        else
                            status += "  |  ALTERNATING TURNS";
                    }
                    shared_video->set_cabinet_status(std::move(status));
                }
            }
            // The two cabinets are one session: when the partner app closes,
            // this one has nothing to play against and follows it out rather
            // than sitting on a frozen board waiting for input.
            if (arcade_input_netplay_active() &&
                arcade_input_netplay_peer_lost()) {
                std::printf("Netplay partner closed - closing this cabinet "
                            "too\n");
                arcade_audio_output::set_output_muted(true);
                // The whole app closes, not just the game: the two cabinets
                // are one session, and a launcher left open on the second
                // machine is not "closed" in any sense the player means.
                host_action = arcade_host_action::exit_application;
                break;
            }
            if (arcade_input_netplay_active()) {
                // One line that says whether the two machines are actually
                // in step: a divergence is reported rather than left to look
                // like two players seeing different games.
                const uint32_t desync = arcade_input_netplay_desync_frame();
                std::string netplay_status =
                    "NETPLAY  |  PLAYER " + std::to_string(cabinet_node);
                if (desync) {
                    netplay_status += "  |  DESYNC AT FRAME " +
                                      std::to_string(desync) +
                                      " - RESTART THE SESSION";
                } else if (arcade_input_netplay_stalled()) {
                    netplay_status += "  |  WAITING FOR PEER";
                } else {
                    netplay_status += "  |  " +
                        std::to_string(arcade_input_netplay_lead()) +
                        " FRAMES BUFFERED";
                }
                if (netplay_status != last_netplay_status) {
                    last_netplay_status = netplay_status;
                    shared_video->set_cabinet_status(netplay_status);
                }
            }

            if (emu->take_operator_settings_request())
                emu->open_operator_settings();

            if (emu->take_controls_request()) {
                const bool was_paused = emu->paused();
                emu->set_paused(true);
                shared_video->run_modal([
                    &installed_games, &current_game_short_name] {
                    // A software launcher avoids stealing the running
                    // cabinet's OpenGL context while controls are edited.
                    launcher_menu menu(true);
                    show_game_input_mapper(menu, installed_games,
                                           current_game_short_name);
                });
                emu->reload_input_mappings();
                emu->set_paused(was_paused);
                emu->refresh_output();
                deadline = std::chrono::steady_clock::now();
            }

            // Arcade wall: only the focused column is heard. Its volume
            // tracks focus rather than being saved, so leaving the wall
            // restores whatever the user actually configured.
            if (settings.wall_count > 1) {
                const int audible = shared_video->wall_audible();
                if (audible >= 0 && audible != wall_audible_applied) {
                    wall_audible_applied = audible;
                    arcade_audio_output::set_output_muted(audible == 0);
                }
            }

            if (emu->take_settings_change(settings) &&
                cabinet_node != 2 && !save_settings(settings)) {
                std::fprintf(stderr, "Could not save settings to %s\n",
                             settings_path().c_str());
            }

            // In-game game switch: the full library browser over the
            // paused cabinet. A choice reloads this process's board in
            // place - a wall column swaps its game while the other columns
            // keep running.
            if (emu->take_game_picker_request()) {
                const bool was_paused = emu->paused();
                emu->set_paused(true);
                std::string switched;
                shared_video->run_modal([&installed_games, &switched] {
                    switched = show_in_game_game_browser(installed_games);
                });
                if (!switched.empty()) {
                    std::printf("Switching ROM board to: %s\n",
                                switched.c_str());
                    rom_path = std::move(switched);
                    if (!explicit_bios_path)
                        bios_path =
                            fs::path(rom_path).parent_path().string();
                    restart = true;
                    if (companion.running()) {
                        companion.stop();
                        cabinet_node = 0;
                    }
                    break;
                }
                emu->set_paused(was_paused);
                emu->refresh_output();
                deadline = std::chrono::steady_clock::now();
            }

            std::string selected_rom;
            if (emu->take_rom_selection(selected_rom)) {
                std::printf("Switching ROM board to: %s\n",
                            selected_rom.c_str());
                rom_path = std::move(selected_rom);
                if (!explicit_bios_path)
                    bios_path = fs::path(rom_path).parent_path().string();
                restart = true;
                if (companion.running()) {
                    companion.stop();
                    cabinet_node = 0;
                }
                break;
            }

            if (++wall_frames <= 3 || wall_frames % 600 == 0)
                whitty_wall_log::note("frame %lld", wall_frames);
            // Closing any cabinet of a linked session closes the session:
            // the ring is cut the moment one member dies, so the rest are
            // ghosts. Checked at a walking pace - it is a waitpid per
            // child.
            if (companion.running() && wall_frames % 30 == 0 &&
                companion.any_exited()) {
                whitty_wall_log::note(
                    "a linked cabinet exited: closing the session");
                host_action = arcade_host_action::return_to_menu;
                break;
            }
            // Lockstep netplay: the board may only advance when the peer's
            // input for the next frame has arrived. Running ahead on a guess
            // is exactly the divergence the whole scheme exists to prevent,
            // so a stalled frame redraws rather than simulates.
            // A cabinet must not run a single frame before its partner is
            // there. The two boards have to start from the same frame, and
            // the app that opens first would otherwise run for however long
            // it takes the second to appear - the drift that desynced every
            // early session.
            const bool netplay_waiting =
                arcade_input_netplay_active() &&
                (arcade_input_netplay_stalled() ||
                 !arcade_input_network_peer_seen());
            const bool paused = emu->paused();
            emu->set_paused(paused);
            if (paused || netplay_waiting) {
                // A stalled cabinet still has to watch the link, or it can
                // never notice its partner leaving and would wait for ever.
                if (netplay_waiting) emu->pump_netplay_link();
                // Silence while waiting. The board is not advancing, so the
                // audio device would otherwise keep looping whatever it last
                // held - the stuck note heard when a partner disappears.
                if (netplay_waiting && !netplay_muted) {
                    netplay_muted = true;
                    arcade_audio_output::set_output_muted(true);
                }
                emu->refresh_output();
            }
            else {
                if (netplay_muted) {
                    netplay_muted = false;
                    arcade_audio_output::set_output_muted(false);
                }
                emu->run_frame();
            }

            // System 246 waits directly on Play!'s completed GS flips. A
            // second 60 Hz sleep here creates two free-running clocks; their
            // phase drift periodically skips a field and makes an interlaced
            // image jump.
            if (!paused && emu->producer_paced()) {
                deadline = std::chrono::steady_clock::now();
                continue;
            }

            // Cap host presentation at 60 Hz while respecting boards whose
            // native refresh is slower.
            const double paced_seconds =
                std::max(emu->frame_seconds(), 1.0 / 60.0);
            const auto frame_time =
                std::chrono::duration<double>(paced_seconds);
            const auto frame_ticks =
                std::chrono::duration_cast<
                    std::chrono::steady_clock::duration>(frame_time);
            deadline += frame_ticks;
            const auto now = std::chrono::steady_clock::now();
            if (now - deadline > frame_ticks) deadline = now;
            std::this_thread::sleep_until(deadline);

            if (startup_status_shown &&
                std::chrono::steady_clock::now() - session_began >
                    std::chrono::seconds(6)) {
                startup_status_shown = false;
                // Past the discovery window the invitation is the useful
                // thing to leave on screen: a second cabinet is worth
                // nothing until somebody puts a coin in it.
                shared_video->set_cabinet_status(
                    cabinet_node == 2 ?
                        std::string("PLAYER 2 CABINET  |  INSERT COIN TO "
                                    "JOIN") :
                        std::string("PLAYER 1 CABINET  |  INSERT COIN TO "
                                    "PLAY"));
            }

            // A wall column watches its own speed. The capacity rule budgets
            // a machine's threads before the wall starts, but what a board
            // actually costs depends on the board - three System 22 racers
            // are not three Galaxians - so the column that cannot keep up
            // says so instead of quietly running slow, and says it where a
            // wall of several cabinets can be read back afterwards.
            if (settings.wall_count > 1) {
                ++wall_window_frames;
                const auto since = now - wall_rate_since;
                if (since >= std::chrono::seconds(4)) {
                    const double seconds =
                        std::chrono::duration<double>(since).count();
                    const double measured =
                        wall_window_frames / seconds;
                    const double target = 1.0 / paced_seconds;
                    whitty_wall_log::note("%.1f fps (target %.1f)%s",
                                          measured, target,
                                          wall_column_is_struggling(
                                              measured, target) ?
                                              "  SLOW" : "");
                    if (wall_column_is_struggling(measured, target) &&
                        !wall_reported_slow) {
                        wall_reported_slow = true;
                        shared_video->set_cabinet_status(
                            "TOO MANY CABINETS FOR THIS MACHINE  |  " +
                            std::to_string(static_cast<int>(measured + 0.5)) +
                            " FPS");
                    }
                    wall_window_frames = 0;
                    wall_rate_since = now;
                }
            }
        }

        whitty_wall_log::note("event loop ended, action=%d",
                              static_cast<int>(host_action));
        if (host_action == arcade_host_action::return_to_menu) {
            // A spawned cabinet has no launcher to go back to: leaving the
            // game means leaving the process. Only cabinet 1 owns the menu.
            if (cabinet_node >= 2 &&
                launch_mode != cabinet_launch_mode::linked_network)
                return 0;
            // The launcher owns its own SDL window, so tear down the running
            // cabinet and process-wide presentation window before reopening
            // it. A newly selected board starts with fresh CPU/RAM/audio.
            emu.reset();
            shared_video->shutdown();
            companion.stop();
            wall.stop();
            arcade_audio_output::set_output_muted(false);
            settings.wall_count = 0;
            settings.wall_slot = 0;
            cabinet_node = 0;
            launch_mode = cabinet_launch_mode::single;
            configure_cabinet_environment(0, 2, pair_port_base, false);
            settings = load_settings();
            startup_menu = true;
            restart = true;
        }
    }
    return 0;
}
