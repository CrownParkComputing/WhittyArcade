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
        return m_gpu_renderer->process_events();
    }
    void set_rom_choices(const std::vector<rom_choice>& choices) final {
        m_gpu_renderer->set_rom_choices(choices);
    }
    bool take_rom_selection(std::string& path) final {
        return m_gpu_renderer->take_rom_selection(path);
    }
    bool take_operator_settings_request() final {
        return m_gpu_renderer->take_dip_request();
    }
    bool take_controls_request() final {
        return m_gpu_renderer->take_controls_request();
    }
    bool take_settings_change(emulator_settings& settings) final {
        if (!m_gpu_renderer->take_settings_change(settings)) return false;
        apply_audio_settings(settings);
        return true;
    }
    bool paused() const final { return m_gpu_renderer->paused(); }
    void set_paused(bool value) final { set_audio_paused(value); }
    void refresh_output() final { m_gpu_renderer->refresh_output(); }
    void open_operator_settings() override {}

protected:
    virtual void apply_audio_settings(const emulator_settings&) {}
    virtual void set_audio_paused(bool) {}
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
std::unique_ptr<emulator_session> make_shinobi_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet);
