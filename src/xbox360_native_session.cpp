// The Xbox 360 board's session for a natively recompiled title.
//
// Unlike every emulated board in WhittyArcade, this one has no machine to step
// and no frames to present. A native port is a finished executable: it opens its
// own Vulkan window, reads the pad itself and plays its own audio. So this
// session derives straight from emulator_session rather than from
// video_emulator_session - there is deliberately no host window behind the
// game's, because a second empty window is exactly the artefact that makes an
// integration look bolted on.
//
// What is left is the launcher's half of the contract: start the title, keep the
// application alive while it plays, and return to the menu the moment it exits.
#include "arcade_session_internal.h"
#include "xbox360_native_runtime.h"
#include "xbox360_rom.h"

#include <cstdio>
#include <utility>

namespace {

class xbox360_native_emulator final : public emulator_session {
public:
    xbox360_native_emulator() = default;

    arcade_board_type board_type() const noexcept override {
        return arcade_board_type::xbox360;
    }

    bool initialize(const std::string& rom_path, const std::string&,
                    const emulator_settings&) override {
        const xbox360_rom_info game = xbox360_rom_loader::inspect(rom_path);
        if (!game) {
            std::fprintf(stderr, "%s\n", game.error.c_str());
            return false;
        }
        const std::string short_name =
            xbox360_rom_loader::set_short_name(game.set);
        // A packaged title is handed to the runtime whole; an extracted one is
        // handed its XEX and the directory its data sits in.
        const xbox360_native_content content = game.packaged() ?
            xbox360_native_content::package(game.package_path) :
            xbox360_native_content::extracted(game.xex_path, game.game_root);
        const xbox360_native_launch launch =
            plan_xbox360_native_launch(short_name, content);
        if (!launch) {
            std::fprintf(stderr, "%s\n", launch.error.c_str());
            return false;
        }
        std::string error;
        if (!m_process.start(launch, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
        std::printf("%s started as a native port: %s\n",
                    xbox360_rom_loader::set_display_name(game.set),
                    launch.binary.c_str());
        std::printf("The game owns its own window; closing it returns to the "
                    "WhittyArcade menu.\n");
        m_started = true;
        return true;
    }

    // The title advances itself in its own process, at its own rate.
    void run_frame() override {}

    arcade_host_action process_events() override {
        if (!m_started) return arcade_host_action::return_to_menu;
        // Quitting the game is not quitting WhittyArcade: the launcher comes
        // back, exactly as it does when an emulated cabinet is left.
        if (!m_process.running()) {
            m_started = false;
            return arcade_host_action::return_to_menu;
        }
        return arcade_host_action::continue_running;
    }

    // A native port carries its own menus, controls and options, so none of the
    // host-side cabinet furniture applies to it.
    void set_rom_choices(const std::vector<rom_choice>&) override {}
    bool take_rom_selection(std::string&) override { return false; }
    bool take_operator_settings_request() override { return false; }
    bool take_controls_request() override { return false; }
    void open_operator_settings() override {}
    void reload_input_mappings() override {}
    bool take_settings_change(emulator_settings&) override { return false; }
    bool paused() const override { return false; }
    void set_paused(bool) override {}
    void refresh_output() override {}
    // Only used to pace this loop's polling for the child's exit.
    double frame_seconds() const override { return 1.0 / 60.0; }

private:
    xbox360_native_process m_process;
    bool m_started{false};
};

} // namespace

std::unique_ptr<emulator_session> make_xbox360_native_session(
        std::shared_ptr<arcade_video_worker>,
        std::shared_ptr<arcade_cabinet_state>) {
    return std::make_unique<xbox360_native_emulator>();
}
