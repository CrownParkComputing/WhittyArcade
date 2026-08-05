#include "arcade_session_internal.h"
#include "platform_paths.h"
#include "xbox360_audio.h"
#include "xbox360_rom.h"
#include "arcade_catalog.h"
#include "native_title_library.h"
#include "xbox360_runtime_loader.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::int16_t xbox_axis(std::uint8_t value, bool invert) {
    const int centered = static_cast<int>(value) - 0x7f;
    int scaled = centered < 0 ? centered * 32768 / 127 :
                               centered * 32767 / 128;
    scaled = std::clamp(scaled, -32768, 32767);
    return static_cast<std::int16_t>(invert ? -scaled : scaled);
}

class xbox360_emulator final : public video_emulator_session {
public:
    xbox360_emulator(std::shared_ptr<arcade_video_worker> video,
                     std::shared_ptr<arcade_cabinet_state> cabinet)
        : video_emulator_session(arcade_board_type::xbox360,
                                 std::move(video), std::move(cabinet)) {}

    ~xbox360_emulator() override {
        m_runtime.shutdown();
        m_audio.shutdown();
        if (m_gpu_renderer) m_gpu_renderer->reset_session();
        if (m_input) m_input->shutdown();
    }

    bool initialize(const std::string& rom_path, const std::string&,
                    const emulator_settings& settings) override {
        const xbox360_rom_info game = xbox360_rom_loader::inspect(rom_path);
        if (!game) {
            std::fprintf(stderr, "%s\n", game.error.c_str());
            return false;
        }
        if (!m_gpu_renderer->initialize(settings)) {
            std::fprintf(stderr, "Failed to initialize MANX video\n");
            return false;
        }
        m_input = std::make_unique<arcade_input>();
        if (!m_input->initialize("robotron"))
            std::fprintf(stderr,
                         "Robotron input failed; controls remain neutral\n");

        fs::path config = manx_platform::config_root();
        fs::path data = manx_platform::data_root();
        if (config.empty()) config = fs::current_path();
        if (data.empty()) data = config;
        const fs::path user_root = config / "MANX" / "xbox360";
        const fs::path cache_root = data / "MANX" / "cache" /
                                    "xbox360";
        std::error_code directory_error;
        fs::create_directories(user_root, directory_error);
        directory_error.clear();
        fs::create_directories(cache_root, directory_error);

        std::string runtime_error;
        if (!m_runtime.initialize(game.game_root, user_root.string(),
                                  cache_root.string(), runtime_error)) {
            std::fprintf(stderr, "%s\n", runtime_error.c_str());
            return false;
        }
        m_audio.set_master_volume(settings.master_volume);
        if (!m_audio.initialize())
            std::fprintf(stderr,
                         "Robotron audio output unavailable; video will continue\n");
        std::printf("Robotron: 2084 started offline through the MANX "
                    "renderer\n");
        return true;
    }

    void run_frame() override {
        if (!m_input || !m_runtime.running()) return;
        m_input->set_suppressed(m_gpu_renderer->input_suppressed());
        m_input->update();
        const input_state& state = m_input->state();
        manx_xbox360_input input{};
        input.struct_size = sizeof(input);
        input.packet_number = ++m_input_packet;
        input.left_x = xbox_axis(state.left_stick_x, false);
        input.left_y = xbox_axis(state.left_stick_y, true);
        input.right_x = xbox_axis(state.right_stick_x, false);
        input.right_y = xbox_axis(state.right_stick_y, true);
        if (state.start) input.buttons |= MANX_X360_START;
        if (state.coin1 || state.view) input.buttons |= MANX_X360_BACK;
        if (state.buttons[0]) input.buttons |= MANX_X360_A;
        if (state.buttons[1]) input.buttons |= MANX_X360_B;
        if (state.buttons[2]) input.buttons |= MANX_X360_X;
        if (state.buttons[3]) input.buttons |= MANX_X360_Y;
        if (state.shift_down) input.buttons |= MANX_X360_LEFT_SHOULDER;
        if (state.shift_up) input.buttons |= MANX_X360_RIGHT_SHOULDER;
        m_runtime.set_input(input);
        manx_xbox360_vibration vibration{};
        if (m_runtime.vibration(vibration))
            m_input->set_rumble(vibration.left_motor_speed,
                                vibration.right_motor_speed);

        int width = 0;
        int height = 0;
        std::uint64_t sequence = 0;
        if (m_runtime.capture_frame(m_frame_rgba, width, height, sequence) &&
            sequence != m_presented_sequence) {
            m_gpu_renderer->present_rgba_frame(m_frame_rgba.data(), width,
                                               height, 1280, 720);
            m_presented_sequence = sequence;
        }

        for (;;) {
            const std::size_t frames = m_runtime.read_audio(
                m_audio_scratch.data(), m_audio_scratch.size() / 2);
            if (frames == 0) break;
            m_audio.enqueue(m_audio_scratch.data(), frames);
        }

        xbox360_runtime_achievement unlocked;
        while (m_runtime.take_unlocked_achievement(unlocked)) {
            std::printf("Achievement unlocked: %s (%uG)\n",
                        unlocked.label.c_str(), unlocked.gamerscore);
        }
    }

    void reload_input_mappings() override {
        if (m_input) m_input->reload_mappings();
    }
    double frame_seconds() const override { return 1.0 / 60.0; }

protected:
    void apply_audio_settings(const emulator_settings& settings) override {
        m_audio.set_master_volume(settings.master_volume);
    }
    void set_audio_paused(bool paused) override {
        m_audio.set_paused(paused);
        m_runtime.set_paused(paused);
    }

private:
    std::unique_ptr<arcade_input> m_input;
    xbox360_runtime_loader m_runtime;
    xbox360_audio_output m_audio;
    std::vector<std::uint8_t> m_frame_rgba;
    std::array<std::int16_t, 8192> m_audio_scratch{};
    std::uint64_t m_presented_sequence{};
    std::uint32_t m_input_packet{};
};

// Which titles are served by a native port rather than by an in-process module.
// The module path below predates the native runtime and is kept only for the
// title it was written for.
bool served_by_native_runtime(xbox360_rom_set set) {
    return set == xbox360_rom_set::geometry_wars ||
           set == xbox360_rom_set::geometry_wars_2 ||
           set == xbox360_rom_set::space_giraffe;
}

// Routing decided by the ROM, not by the factory, because which runtime a title
// needs is a property of the title and the board's factory is only handed the
// board. A session that turns out to be the wrong kind never starts: the
// delegate is built here, from the path, before anything is initialized.
class xbox360_board final : public emulator_session {
public:
    xbox360_board(std::shared_ptr<arcade_video_worker> video,
                  std::shared_ptr<arcade_cabinet_state> cabinet)
        : m_video(std::move(video)), m_cabinet(std::move(cabinet)) {}

    arcade_board_type board_type() const noexcept override {
        return arcade_board_type::xbox360;
    }

    bool initialize(const std::string& rom_path, const std::string& bios_path,
                    const emulator_settings& settings) override {
        // A converted title decides its own routing, before the ROM loader is
        // consulted at all. The loader records which shape a title was dumped
        // in and returns early when a copy is the other shape - leaving the set
        // unidentified, so routing fell back to the in-process emulator for a
        // game that has a working native port installed. Geometry Wars is
        // exactly that case: the loader expects it extracted, this copy is a
        // package, and the native port plays it either way.
        const bool converted = find_native_title_for_game(rom_path) != nullptr;
        const xbox360_rom_set set =
            xbox360_rom_loader::inspect(rom_path, false).set;
        m_delegate = (converted || served_by_native_runtime(set)) ?
            make_xbox360_native_session(m_video, m_cabinet) :
            std::unique_ptr<emulator_session>(
                std::make_unique<xbox360_emulator>(m_video, m_cabinet));
        return m_delegate->initialize(rom_path, bios_path, settings);
    }

    void run_frame() override {
        if (m_delegate) m_delegate->run_frame();
    }
    // Forwarded, like everything else here. Answering for the delegate rather
    // than from it left the launcher's window on screen over a native title:
    // this wrapper does not own a window, but the port behind it does, and the
    // player is then looking at the game while the launcher holds the focus.
    bool owns_its_own_window() const noexcept override {
        return m_delegate && m_delegate->owns_its_own_window();
    }
    arcade_host_action process_events() override {
        return m_delegate ? m_delegate->process_events() :
                            arcade_host_action::return_to_menu;
    }
    void set_rom_choices(const std::vector<rom_choice>& choices) override {
        if (m_delegate) m_delegate->set_rom_choices(choices);
    }
    bool take_rom_selection(std::string& path) override {
        return m_delegate && m_delegate->take_rom_selection(path);
    }
    bool take_operator_settings_request() override {
        return m_delegate && m_delegate->take_operator_settings_request();
    }
    bool take_controls_request() override {
        return m_delegate && m_delegate->take_controls_request();
    }
    bool take_game_picker_request() override {
        return m_delegate && m_delegate->take_game_picker_request();
    }
    void open_operator_settings() override {
        if (m_delegate) m_delegate->open_operator_settings();
    }
    void reload_input_mappings() override {
        if (m_delegate) m_delegate->reload_input_mappings();
    }
    bool take_settings_change(emulator_settings& settings) override {
        return m_delegate && m_delegate->take_settings_change(settings);
    }
    bool paused() const override {
        return m_delegate && m_delegate->paused();
    }
    void set_paused(bool paused) override {
        if (m_delegate) m_delegate->set_paused(paused);
    }
    void refresh_output() override {
        if (m_delegate) m_delegate->refresh_output();
    }
    double frame_seconds() const override {
        return m_delegate ? m_delegate->frame_seconds() : 1.0 / 60.0;
    }
    int active_player() const override {
        return m_delegate ? m_delegate->active_player() : -1;
    }
    bool producer_paced() const override {
        return m_delegate && m_delegate->producer_paced();
    }

private:
    std::shared_ptr<arcade_video_worker> m_video;
    std::shared_ptr<arcade_cabinet_state> m_cabinet;
    std::unique_ptr<emulator_session> m_delegate;
};

} // namespace

std::unique_ptr<emulator_session> make_xbox360_session(
        std::shared_ptr<arcade_video_worker> video,
        std::shared_ptr<arcade_cabinet_state> cabinet) {
    return std::make_unique<xbox360_board>(std::move(video),
                                           std::move(cabinet));
}
