// Shared implementation details for board-specific session modules.
#pragma once

#include "arcade_input.h"
#include "arcade_session.h"
#include "arcade_video_worker.h"

#include <functional>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

inline bool session_trace_enabled() {
    const char* value = std::getenv("WHITTYARCADE_TRACE");
    return value && *value && std::strcmp(value, "0") != 0;
}

template <typename AudioSystem>
void shutdown_session_devices(
        std::unique_ptr<AudioSystem>& audio,
        std::shared_ptr<arcade_video_worker>& video,
        std::unique_ptr<arcade_input>& input) {
    // Stop board-owned producers, discard queued frames, then remove this
    // board's input watch. The process-wide video worker and host contexts
    // survive ROM changes; CPUs, RAM and audio devices do not.
    if (audio) audio->shutdown();
    if (video) video->reset_session();
    if (input) input->shutdown();
}

class video_emulator_session : public emulator_session {
public:
    video_emulator_session(arcade_board_type board,
                           std::shared_ptr<arcade_video_worker> video,
                           std::shared_ptr<arcade_cabinet_state> cabinet)
        : m_gpu_renderer(std::move(video)), m_cabinet(std::move(cabinet)),
          m_board(board) {}

    arcade_board_type board_type() const noexcept final { return m_board; }
    arcade_host_action process_events() final {
        const arcade_host_action result = m_gpu_renderer->process_events();
        operator_menu_action action;
        bool changed = false;
        while (m_gpu_renderer->take_operator_action(action)) {
            apply_operator_action(action);
            changed = true;
        }
        if (changed)
            m_gpu_renderer->set_operator_menu(operator_menu());
        return result;
    }
    void set_rom_choices(const std::vector<rom_choice>& choices) final {
        m_gpu_renderer->set_rom_choices(choices);
        m_gpu_renderer->set_operator_menu(operator_menu());
    }
    bool take_rom_selection(std::string& path) final {
        return m_gpu_renderer->take_rom_selection(path);
    }
    bool take_operator_settings_request() final {
        return false;
    }
    bool take_controls_request() final {
        return m_gpu_renderer->take_controls_request();
    }
    bool take_game_picker_request() final {
        return m_gpu_renderer->take_game_picker_request();
    }
    bool take_settings_change(emulator_settings& settings) final {
        if (!m_gpu_renderer->take_settings_change(settings)) return false;
        apply_audio_settings(settings);
        return true;
    }
    bool paused() const final { return m_gpu_renderer->paused(); }
    void set_paused(bool value) final { set_audio_paused(value); }
    void refresh_output() final { m_gpu_renderer->refresh_output(); }
    void pump_netplay_link() final { arcade_input_netplay_poll(); }
    void open_operator_settings() override {}

protected:
    virtual void apply_audio_settings(const emulator_settings&) {}
    virtual void set_audio_paused(bool) {}
    virtual operator_menu_definition operator_menu() const {
        operator_menu_definition menu;
        menu.description =
            "This game has no verified host-side DIP editor.";
        menu.rows.push_back({0, "Use the original cabinet TEST menu (F2)",
                             {}, 0, false, false});
        return menu;
    }
    virtual void apply_operator_action(const operator_menu_action&) {}
    void show_modal(std::function<void()> action) {
        m_gpu_renderer->run_modal(std::move(action));
    }
    std::shared_ptr<arcade_video_worker> m_gpu_renderer;
    std::shared_ptr<arcade_cabinet_state> m_cabinet;

private:
    arcade_board_type m_board;
};

std::unique_ptr<emulator_session> make_system22_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet);
std::unique_ptr<emulator_session> make_system246_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet);
std::unique_ptr<emulator_session> make_xbox360_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet);
// The Xbox 360 board's session for a title that ships as a native port. It owns
// a child process rather than a machine, so it takes neither the video worker
// nor the cabinet switches - the parameters exist only to match the factories.
std::unique_ptr<emulator_session> make_xbox360_native_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet);
std::unique_ptr<emulator_session> make_xbox_burnout3_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet);
std::unique_ptr<emulator_session> make_model1_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet);
std::unique_ptr<emulator_session> make_model2_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet);
std::unique_ptr<emulator_session> make_galaxian_session(
    arcade_board_type board,
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet);
std::unique_ptr<emulator_session> make_system16b_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet);
std::unique_ptr<emulator_session> make_gng_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet);
std::unique_ptr<emulator_session> make_namco_galaga_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet);
std::unique_ptr<emulator_session> make_namco_system1_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet);
