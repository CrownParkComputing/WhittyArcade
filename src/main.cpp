// WhittyArcade application shell. Board implementations live in
// arcade_session.cpp and are selected through the canonical catalog.

#include "arcade_catalog.h"
#include "arcade_frontend.h"
#include "arcade_session.h"
#include "arcade_video_worker.h"
#include "rom_library.h"

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

namespace fs = std::filesystem;

namespace {

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

int import_roms(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: WhittyArcade --import <zip-or-folder>...\n");
        return 2;
    }
    std::vector<std::string> sources;
    for (int index = 2; index < argc; ++index)
        sources.emplace_back(argv[index]);
    const rom_import_result result = import_rom_paths(sources);
    std::printf("%s\nLibrary: %s\n", result.summary().c_str(),
                rom_library_path().c_str());
    return result.error.empty() && result.games_found != 0 ? 0 : 1;
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
    if (command == "--import") return import_roms(argc, argv);
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

    std::string rom_path = argc > 1 ? argv[1] : "roms/";
    std::string bios_path;
    const bool explicit_bios_path = argc > 2;
    if (explicit_bios_path) {
        bios_path = argv[2];
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
    bool restart = true;
    bool startup_menu = argc <= 1;

    while (restart) {
        restart = false;
        if (startup_menu) {
            startup_menu = false;
            rom_selection_result selection = show_rom_selector(rom_path);
            if (selection.action == rom_selection_action::selected) {
                rom_path = std::move(selection.path);
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

        std::unique_ptr<emulator_session> emu = create_emulator_session(
            identity->board, shared_video, cabinet_state);
        if (!emu->initialize(rom_path, bios_path, settings)) return 1;
        emu->set_rom_choices(discover_rom_choices(rom_path));

        auto deadline = std::chrono::steady_clock::now();
        arcade_host_action host_action =
            arcade_host_action::continue_running;
        while ((host_action = emu->process_events()) ==
               arcade_host_action::continue_running) {
            if (emu->take_operator_settings_request())
                emu->open_operator_settings();

            if (emu->take_settings_change(settings) &&
                !save_settings(settings)) {
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
                break;
            }

            const bool paused = emu->paused();
            emu->set_paused(paused);
            if (paused)
                emu->refresh_output();
            else
                emu->run_frame();

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
            // The launcher owns its own SDL window, so tear down the running
            // cabinet and process-wide presentation window before reopening
            // it. A newly selected board starts with fresh CPU/RAM/audio.
            emu.reset();
            shared_video->shutdown();
            startup_menu = true;
            restart = true;
        }
    }
    return 0;
}
