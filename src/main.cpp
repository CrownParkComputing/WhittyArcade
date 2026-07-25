// WhittyArcade application shell. Board implementations live in
// arcade_session.cpp and are selected through the canonical catalog.

#include "arcade_catalog.h"
#include "arcade_frontend.h"
#include "arcade_input.h"
#include "arcade_session.h"
#include "arcade_video_worker.h"
#include "input_mapper.h"
#include "launcher_menu.h"
#include "multiplayer_lobby.h"
#include "namco/system22/system22_c139_transport.h"
#include "namco/system22/system22_cpu.h"
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
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

struct runtime_options {
    int cabinet_node{};
    uint16_t pair_port_base{35112};
    bool independent_pair{};
    bool twin_screen{};
    bool network_pair{};
    std::vector<std::string> positional;
};

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
        } else if (argument == "--pair-port-base" && index + 1 < argc) {
            const int value = std::atoi(argv[++index]);
            if (value > 1024 && value < 65535)
                options.pair_port_base = static_cast<uint16_t>(value);
        } else if (argument == "--independent-cabinet") {
            options.independent_pair = true;
        } else if (argument == "--twin-screen") {
            options.twin_screen = true;
        } else if (argument == "--network-cabinet") {
            options.network_pair = true;
        } else {
            options.positional.emplace_back(argument);
        }
    }
    if (options.cabinet_node != 1 && options.cabinet_node != 2)
        options.cabinet_node = 0;
    return options;
}

void configure_cabinet_environment(int node, uint16_t port_base,
                                   bool linked_model2,
                                   bool linked_system22 = false,
                                   bool network_link = false,
                                   bool generic_input_link = false) {
    if (node != 1 && node != 2) {
        unset_environment("WHITTY_CABINET_NODE");
        unset_environment("MODEL2_COMM_NODE");
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
    if (linked_model2) {
        set_environment("MODEL2_COMM_NODE", std::to_string(node));
        set_environment("MODEL2_COMM_LOCAL_PORT", std::to_string(local_port));
        set_environment("MODEL2_COMM_PEER_PORT", std::to_string(peer_port));
        if (network_link)
            set_environment("MODEL2_COMM_NETWORK", "1");
        else
            unset_environment("MODEL2_COMM_NETWORK");
    } else {
        unset_environment("MODEL2_COMM_NODE");
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
        const uint16_t video_port = port_base <= 65533 ?
            static_cast<uint16_t>(port_base + 2) :
            static_cast<uint16_t>(port_base - 2);
        set_environment("WHITTY_VIDEO_ROLE", std::to_string(node));
        set_environment("WHITTY_VIDEO_PORT", std::to_string(video_port));
    } else {
        unset_environment("WHITTY_INPUT_LINK");
        unset_environment("WHITTY_INPUT_LOCAL_PORT");
        unset_environment("WHITTY_INPUT_PEER_PORT");
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
    return static_cast<uint16_t>(36000 + (process_id % 12000) * 2 % 24000);
}

class cabinet_companion {
public:
    ~cabinet_companion() { stop(); }

    bool start(const std::string& executable, const std::string& rom_path,
               const std::string& bios_path, bool explicit_bios_path,
               uint16_t port_base, bool independent_pair) {
        stop();
        std::vector<std::string> arguments{
            executable,
            "--cabinet-node", "2",
            "--pair-port-base", std::to_string(port_base),
        };
        if (independent_pair)
            arguments.emplace_back("--independent-cabinet");
        arguments.push_back(rom_path);
        if (explicit_bios_path) arguments.push_back(bios_path);
        std::vector<char*> native_arguments;
        native_arguments.reserve(arguments.size() + 1);
        for (std::string& argument : arguments)
            native_arguments.push_back(argument.data());
        native_arguments.push_back(nullptr);
#if defined(_WIN32)
        m_process = _spawnv(_P_NOWAIT, executable.c_str(),
                            native_arguments.data());
        return m_process != -1;
#else
        const pid_t child = fork();
        if (child < 0) return false;
        if (child == 0) {
            execvp(executable.c_str(), native_arguments.data());
            std::_Exit(127);
        }
        m_process = child;
        return true;
#endif
    }

    void stop() {
        if (m_process == -1) return;
#if defined(_WIN32)
        HANDLE handle = reinterpret_cast<HANDLE>(m_process);
        TerminateProcess(handle, 0);
        WaitForSingleObject(handle, 2000);
        CloseHandle(handle);
#else
        const pid_t child = static_cast<pid_t>(m_process);
        int status = 0;
        const pid_t ended = waitpid(child, &status, WNOHANG);
        if (ended == 0) {
            kill(child, SIGTERM);
            for (int attempt = 0; attempt < 20; ++attempt) {
                if (waitpid(child, &status, WNOHANG) == child) {
                    m_process = -1;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
        }
#endif
        m_process = -1;
    }

    bool running() const { return m_process != -1; }

private:
    std::intptr_t m_process{-1};
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
    return sessions_created == arcade_board_count * rounds ? 0 : 6;
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
    ::setenv("SYSTEM22_C139_LOCAL_PORT", "17512", 1);
    ::setenv("SYSTEM22_C139_PEER_PORT",  "17513", 1);
    // Cabinet 1 sits on 17512, sends to 17513. We re-set the env for
    // cabinet 2 so the local/peer roles swap.
    system22_c139_transport a;
    if (!a.initialize(cabinet1, nullptr, /*forced_node=*/1)) {
        std::fprintf(stderr, "c139 link test: transport a init failed\n");
        return 1;
    }
    ::setenv("SYSTEM22_C139_LOCAL_PORT", "17513", 1);
    ::setenv("SYSTEM22_C139_PEER_PORT",  "17512", 1);
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
            choices.push_back({
                argv[index], argv[index],
                identity ? identity->board : arcade_board_type::system22});
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
                if (selection.fullscreen_override >= 0)
                    settings.fullscreen =
                        selection.fullscreen_override != 0;
                // A linked-cabinet launch (either via the menu's "Two
                // Windows" path or via --network-cabinet) must open two
                // windows side-by-side, never a single fullscreen
                // surface. Force windowed here so a stale
                // fullscreen=1 in settings.ini cannot override the
                // user's intent.
                if (launch_mode == cabinet_launch_mode::linked_pair ||
                    launch_mode == cabinet_launch_mode::linked_network) {
                    settings.fullscreen = false;
                }
                settings.twin_separate_monitors =
                    selection.twin_separate_monitors;
                if (launch_mode == cabinet_launch_mode::linked_network)
                    pair_port_base = 35112;
                if (!explicit_bios_path)
                    bios_path = fs::path(rom_path).parent_path().string();
            } else if (selection.action ==
                       rom_selection_action::exit_requested) {
                return 0;
            }
        }

        const std::optional<arcade_game_identity> identity =
            identify_arcade_game(rom_path);
        if (!identity) {
            std::fprintf(stderr,
                         "Unsupported or incomplete ROM archive: %s\n",
                         rom_path.c_str());
            return 1;
        }

        const bool native_model2_link =
            std::string_view(identity->short_name) == "srallyc";
        // System 22 racing titles (Ridge Racer 1/2, Rave Racer, Ace Driver,
        // Victory Lap) all share the same Namco C139 SCI cabinet-to-cabinet
        // link cable; the per-game firmware just sets the role / heartbeat
        // pattern. Drive the predicate from the catalog's multiplayer mode so
        // adding a new linked System 22 title is a one-line change there.
        const rom_set_manifest* launch_manifest =
            find_supported_rom_set(identity->short_name);
        const bool native_system22_link =
            launch_manifest &&
            launch_manifest->multiplayer ==
                arcade_multiplayer_mode::native_link;
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
        if (cabinet_node == 0 && local_pair) {
            pair_port_base = choose_pair_port_base();
            if (!companion.start(argv[0], rom_path, bios_path,
                                 explicit_bios_path, pair_port_base,
                                 false)) {
                std::fprintf(stderr,
                             "Could not start the second cabinet process (execv"
                             "p failed). WhittyArcade was launched as \"%s\". "
                             "Make sure the executable is found via PATH, or "
                             "use an absolute path (e.g. ./WhittyArcade).\n",
                             argv[0] ? argv[0] : "(null)");
                launch_mode = cabinet_launch_mode::single;
            } else {
                cabinet_node = 1;
            }
        }
        configure_cabinet_environment(
            cabinet_node, pair_port_base,
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
        if (cabinet_node) {
            // linked_network spreads the two cabinets across different
            // physical displays (cabinet 1 → display 0, cabinet 2 →
            // display 1) so a single ultrawide or two side-by-side
            // monitors each get their own window without the user
            // needing to drag one out from under the other. The earlier
            // code pinned both to display 0, which made the two
            // processes stack on top of each other on a single-screen
            // setup.
            settings.display_index = cabinet_node - 1;
            std::printf("Starting cabinet %d on display %d\n",
                        cabinet_node, settings.display_index + 1);
        } else {
            settings.display_index = -1;
        }

        emulator_settings session_settings = settings;
        // Player 2 contributes controls to Player 1 and presents Player 1's
        // authoritative picture. Silence the hidden local board so two
        // unsynchronised audio timelines can never be heard together.
        if (cabinet_node == 2 &&
            launch_mode == cabinet_launch_mode::linked_network &&
            !native_hardware_link) {
            session_settings.master_volume = 0;
            session_settings.music_volume = 0;
            session_settings.effects_volume = 0;
        }
        std::unique_ptr<emulator_session> emu = create_emulator_session(
            identity->board, shared_video, cabinet_state);
        if (!emu->initialize(rom_path, bios_path, session_settings)) return 1;
        if (launch_mode == cabinet_launch_mode::independent_pair) {
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

            if (emu->take_settings_change(settings) &&
                cabinet_node != 2 && !save_settings(settings)) {
                std::fprintf(stderr, "Could not save settings to %s\n",
                             settings_path().c_str());
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

            const bool paused = emu->paused();
            emu->set_paused(paused);
            if (paused)
                emu->refresh_output();
            else
                emu->run_frame();

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
        }

        if (host_action == arcade_host_action::return_to_menu) {
            if (cabinet_node == 2 &&
                launch_mode != cabinet_launch_mode::linked_network)
                return 0;
            // The launcher owns its own SDL window, so tear down the running
            // cabinet and process-wide presentation window before reopening
            // it. A newly selected board starts with fresh CPU/RAM/audio.
            emu.reset();
            shared_video->shutdown();
            companion.stop();
            cabinet_node = 0;
            launch_mode = cabinet_launch_mode::single;
            configure_cabinet_environment(0, pair_port_base, false);
            settings = load_settings();
            startup_menu = true;
            restart = true;
        }
    }
    return 0;
}
