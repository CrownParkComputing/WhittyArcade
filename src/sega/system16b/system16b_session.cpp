// Sega System 16B / Shinobi runtime session.

#include "arcade_session_internal.h"
#include "arcade_frontend.h"
#include "sega/system16b/system16b_audio.h"
#include "sega/system16b/system16b_machine.h"
#include "sega/system16b/system16b_rom.h"

#include <cstdio>
#include <utility>

namespace {

class system16b_emulator : public video_emulator_session {
private:
    std::unique_ptr<system16b::system16b_machine_t>  m_machine;
    std::unique_ptr<system16b_audio_system>   m_audio;
    std::unique_ptr<arcade_input>          m_input;
    uint64_t                                m_frame_number{0};

public:
    system16b_emulator(std::shared_ptr<arcade_video_worker> video,
                     std::shared_ptr<arcade_cabinet_state> cabinet)
        : video_emulator_session(arcade_board_type::system16b,
                                 std::move(video), std::move(cabinet)) {}

    ~system16b_emulator() override {
        shutdown_session_devices(m_audio, m_gpu_renderer, m_input);
    }

    bool initialize(const std::string& rom_path, const std::string&,
                    const emulator_settings& settings) override {
        m_machine      = std::make_unique<system16b::system16b_machine_t>();
        m_input        = std::make_unique<arcade_input>();
        if (!m_gpu_renderer->initialize(settings)) return false;
        const system16b::system16b_rom_set set =
            system16b::system16b_rom_loader::identify_set(rom_path);
        const std::string_view short_name =
            set == system16b::system16b_rom_set::unknown ? std::string_view{} :
            system16b::system16b_rom_loader::set_short_name(set);
        if (!m_input->initialize(short_name)) return false;
        if (!m_machine->load_roms(rom_path)) return false;
        m_audio = std::make_unique<system16b_audio_system>(
            make_system16b_sound_synth());
        if (m_audio->initialize()) {
            m_audio->set_mix_levels(settings.master_volume,
                                    settings.music_volume,
                                    settings.effects_volume);
            m_machine->set_sound_write_callback(
                [this](unsigned port, uint8_t data) {
                    if (m_audio) m_audio->write_control(port, data);
                });
            m_machine->set_sound_status_callback(
                [this]() -> uint8_t {
                    return m_audio ? m_audio->read_status() : 0;
                });
            m_machine->set_sound_timer_callback(
                [this](uint32_t clocks) {
                    if (m_audio) m_audio->advance_timer_clocks(clocks);
                });
            m_audio->start();
        } else {
            std::fprintf(stderr, "Shinobi audio disabled; video will continue\n");
            m_audio.reset();
        }
        m_machine->reset();
        apply_dip_switches();
        std::printf("Shinobi (Sega System 16B) initialized\n");
        return true;
    }

    void run_frame() override {
        m_input->set_suppressed(m_gpu_renderer->settings_visible());
        m_input->update();
        m_machine->set_input(m_input->state());
        m_machine->run_frame();
        m_gpu_renderer->present_rgba_frame(
            reinterpret_cast<const uint8_t*>(m_machine->frame_buffer()),
            m_machine->screen_width(), m_machine->screen_height(), 4, 3);
        if (++m_frame_number % 600 == 0 && session_trace_enabled()) {
            std::printf("Shinobi frame %llu audio=%d\n",
                        static_cast<unsigned long long>(m_frame_number),
                        m_audio ? m_audio->peak_sample() : 0);
        }
    }

    double frame_seconds() const override {
        return 1.0 / m_machine->refresh_rate();
    }
    void reload_input_mappings() override {
        if (m_input) m_input->reload_mappings();
    }

protected:
    operator_menu_definition operator_menu() const override {
        return make_system16b_operator_menu(m_cabinet->system16b_dip_switches);
    }
    void apply_operator_action(const operator_menu_action& action) override {
        apply_system16b_operator_action(m_cabinet->system16b_dip_switches, action);
        apply_dip_switches();
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
    void apply_dip_switches() {
        if (!m_machine) return;
        // System 16B uses active-low DIPs: SW1 is the low byte, SW2 high.
        auto* board = m_machine->board_ptr();
        board->dsw1_ = static_cast<uint8_t>(
            m_cabinet->system16b_dip_switches & 0xff);
        board->dsw2_ = static_cast<uint8_t>(
            (m_cabinet->system16b_dip_switches >> 8) & 0xff);
    }
};

} // namespace

std::unique_ptr<emulator_session> make_system16b_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet) {
    return std::make_unique<system16b_emulator>(std::move(video),
                                               std::move(cabinet));
}
