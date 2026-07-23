// Sega Model 1 runtime session.

#include "arcade_session_internal.h"
#include "arcade_frontend.h"
#include "sega/model1/model1_audio.h"
#include "sega/model1/model1_machine.h"
#include "sega/model1/model1_rom.h"

#include <chrono>
#include <cstdio>
#include <utility>
#include <vector>

namespace {

class model1_emulator : public video_emulator_session {
public:
    model1_emulator(std::shared_ptr<arcade_video_worker> video,
                    std::shared_ptr<arcade_cabinet_state> cabinet)
        : video_emulator_session(arcade_board_type::model1, std::move(video),
                                 std::move(cabinet)) {}

    ~model1_emulator() override {
        shutdown_session_devices(m_audio, m_gpu_renderer, m_input);
    }

    bool initialize(const std::string& rom_path, const std::string&,
                    const emulator_settings& settings) override {
        std::printf("Initializing native Sega Model 1 backend...\n");
        std::printf("ROM path: %s\n", rom_path.c_str());
        if (!m_gpu_renderer->initialize(settings)) {
            std::fprintf(stderr, "Failed to initialize shared video output\n");
            return false;
        }
        m_machine = std::make_unique<model1_machine>();
        std::string error;
        if (!m_machine->initialize(rom_path, &error)) {
            std::fputs(error.c_str(), stderr);
            return false;
        }
        m_audio = std::make_unique<model1_audio_system>();
        if (m_audio->initialize(m_machine->roms())) {
            m_audio->set_mix_levels(settings.master_volume,
                                    settings.music_volume,
                                    settings.effects_volume);
            m_machine->set_sound_uart_handler(
                    [this](uint8_t data, uint64_t timestamp) {
                if (m_audio) m_audio->enqueue_uart(data, timestamp);
            });
            m_audio->start();
        } else {
            std::fprintf(stderr,
                         "Model 1 audio disabled; video will continue\n");
            m_audio.reset();
        }
        m_input = std::make_unique<arcade_input>();
        if (!m_input->initialize(
                model1_rom_loader::set_short_name(m_machine->rom_set())))
            std::fprintf(stderr,
                         "Input initialization failed; controls are neutral\n");
        m_fps_epoch = std::chrono::steady_clock::now();
        std::printf("%s Model 1 machine initialized\n",
                    model1_rom_loader::set_display_name(m_machine->rom_set()));
        return true;
    }

    void run_frame() override {
        m_input->set_suppressed(m_gpu_renderer->settings_visible());
        m_input->update();
        m_machine->set_inputs(m_input->state());
        m_machine->run_frame();
        if (m_audio) m_audio->signal_frame();
        const std::vector<uint8_t>& pixels = m_machine->frame_pixels();
        if (!pixels.empty())
            m_gpu_renderer->present_rgba_frame(
                pixels.data(), model1_machine::native_width(),
                model1_machine::native_height());
        ++m_frame_number;
        if (m_frame_number % 60 == 0 && session_trace_enabled()) {
            const auto now = std::chrono::steady_clock::now();
            const double elapsed = std::chrono::duration<double>(
                now - m_fps_epoch).count();
            const double host_fps = elapsed > 0.0 ?
                static_cast<double>(m_frame_number - m_fps_epoch_frame) /
                    elapsed : 0.0;
            const uint64_t completed = m_machine->completed_video_frames();
            const uint64_t changed = m_machine->changed_video_frames();
            const double graphics_fps = elapsed > 0.0 ?
                static_cast<double>(completed - m_fps_epoch_completed) /
                    elapsed : 0.0;
            const double changed_fps = elapsed > 0.0 ?
                static_cast<double>(changed - m_fps_epoch_changed) /
                    elapsed : 0.0;
            m_fps_epoch = now;
            m_fps_epoch_frame = m_frame_number;
            m_fps_epoch_completed = completed;
            m_fps_epoch_changed = changed;
            std::printf("Model 1 frame=%llu cpu=%.2f gfx=%.2f changed=%.2f "
                        "PC=%08x bank=%u TGP=%llu sound=%08x/%llu "
                        "uart=%llu drop=%llu ym=%llu pcm=%llu/%d peak=%d "
                        "dsb=%d/%llu quads=%zu "
                        "unimplemented=%llu/%02x@%06x\n",
                        static_cast<unsigned long long>(m_frame_number),
                        host_fps, graphics_fps, changed_fps,
                        m_machine->program_counter(), m_machine->rom_o_bank(),
                        static_cast<unsigned long long>(m_machine->tgp_command_count()),
                        m_audio ? m_audio->program_counter() : 0,
                        static_cast<unsigned long long>(
                            m_audio ? m_audio->executed_cycles() : 0),
                        static_cast<unsigned long long>(
                            m_audio ? m_audio->uart_bytes() : 0),
                        static_cast<unsigned long long>(
                            m_audio ? m_audio->dropped_uart_bytes() : 0),
                        static_cast<unsigned long long>(
                            m_audio ? m_audio->ym_writes() : 0),
                        static_cast<unsigned long long>(
                            m_audio ? m_audio->pcm_writes() : 0),
                        m_audio ? m_audio->active_pcm_voices() : 0,
                        m_audio ? m_audio->peak_sample() : 0,
                        m_audio && m_audio->dsb_active() ? 1 : 0,
                        static_cast<unsigned long long>(
                            m_audio ? m_audio->dsb_triggers() : 0),
                        m_machine->rendered_quad_count(),
                        static_cast<unsigned long long>(
                            m_machine->tgp_unimplemented_count()),
                        m_machine->tgp_last_unimplemented_function(),
                        m_machine->tgp_last_unimplemented_pc());
            std::fflush(stdout);
        }
    }

    void reload_input_mappings() override {
        if (m_input) m_input->reload_mappings();
    }
    double frame_seconds() const override {
        return 1.0 / model1_machine::refresh_rate();
    }

protected:
    operator_menu_definition operator_menu() const override {
        if (!m_machine ||
            m_machine->rom_set() != model1_rom_set::virtua_formula)
            return make_unavailable_operator_menu();
        return make_model1_operator_menu(m_machine->attract_sound_enabled());
    }
    void apply_operator_action(const operator_menu_action& action) override {
        if (action.row_id != 0 || !m_machine ||
            m_machine->rom_set() != model1_rom_set::virtua_formula)
            return;
        if (!m_machine->set_attract_sound_enabled(action.selected != 0))
            std::fprintf(stderr,
                         "Could not save Model 1 cabinet settings\n");
    }
    void apply_audio_settings(const emulator_settings& settings) override {
        if (m_audio)
            m_audio->set_mix_levels(settings.master_volume,
                                    settings.music_volume,
                                    settings.effects_volume);
    }
    void set_audio_paused(bool paused) override {
        if (m_audio) m_audio->set_paused(paused);
    }

private:
    std::unique_ptr<model1_machine> m_machine;
    std::unique_ptr<model1_audio_system> m_audio;
    std::unique_ptr<arcade_input> m_input;
    uint64_t m_frame_number{0};
    uint64_t m_fps_epoch_frame{0};
    uint64_t m_fps_epoch_completed{0};
    uint64_t m_fps_epoch_changed{0};
    std::chrono::steady_clock::time_point m_fps_epoch{};
};

} // namespace

std::unique_ptr<emulator_session> make_model1_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet) {
    return std::make_unique<model1_emulator>(std::move(video),
                                              std::move(cabinet));
}
