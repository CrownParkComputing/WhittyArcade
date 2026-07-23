#include "arcade_session_internal.h"
#include "arcade_frontend.h"
#include "capcom/gng/gng_audio.h"
#include "capcom/gng/gng_controls.h"
#include "capcom/gng/gng_machine.h"
#include "capcom/gng/gng_rom.h"

#include <cstdio>
#include <utility>

namespace {

class gng_emulator final : public video_emulator_session {
public:
    gng_emulator(std::shared_ptr<arcade_video_worker> video,
                 std::shared_ptr<arcade_cabinet_state> cabinet)
        : video_emulator_session(arcade_board_type::capcom_gng,
                                 std::move(video), std::move(cabinet)) {}
    ~gng_emulator() override {
        shutdown_session_devices(m_audio, m_gpu_renderer, m_input);
    }

    bool initialize(const std::string& rom_path, const std::string&,
                    const emulator_settings& settings) override {
        const gng::rom_load_result loaded = gng::rom_loader::load(rom_path);
        if (!loaded) {
            std::fprintf(stderr, "Ghosts'n Goblins ROM error: %s\n",
                         loaded.error.c_str());
            return false;
        }
        m_machine = std::make_unique<gng::machine>();
        m_input = std::make_unique<arcade_input>();
        if (!m_gpu_renderer->initialize(settings) ||
            !m_input->initialize("gng") ||
            !m_machine->initialize(loaded.data))
            return false;

        m_audio = std::make_unique<gng::audio_system>();
        if (m_audio->initialize(loaded.data)) {
            m_audio->set_mix_levels(settings.master_volume,
                                    settings.music_volume,
                                    settings.effects_volume);
            m_machine->set_sound_callbacks(
                [this](uint8_t value) {
                    if (m_audio) m_audio->write_command(value);
                },
                [this](bool asserted) {
                    if (m_audio) m_audio->set_reset_line(asserted);
                });
            m_audio->start();
        } else {
            std::fprintf(stderr,
                         "Ghosts'n Goblins audio disabled; video continues\n");
            m_audio.reset();
        }
        std::printf("Ghosts'n Goblins (Capcom 1985) initialized\n");
        return true;
    }

    void run_frame() override {
        m_input->set_suppressed(m_gpu_renderer->settings_visible());
        m_input->update();
        apply_inputs(m_input->state());
        m_machine->run_frame();
        m_gpu_renderer->present_rgba_frame(
            reinterpret_cast<const uint8_t*>(
                m_machine->framebuffer().data()),
            gng::machine::width, gng::machine::height, 4, 3);
        if (++m_frames % 600 == 0 && session_trace_enabled()) {
            std::printf("GnG frame %llu pc=%04x audio=%d\n",
                        static_cast<unsigned long long>(m_frames),
                        m_machine->program_counter(),
                        m_audio ? m_audio->peak_sample() : 0);
        }
    }

    void reload_input_mappings() override {
        if (m_input) m_input->reload_mappings();
    }
    double frame_seconds() const override {
        return 1.0 / gng::machine::refresh_hz;
    }

protected:
    operator_menu_definition operator_menu() const override {
        return make_gng_operator_menu(m_cabinet->gng_dip_switches);
    }
    void apply_operator_action(const operator_menu_action& action) override {
        apply_gng_operator_action(m_cabinet->gng_dip_switches, action);
    }
    void apply_audio_settings(const emulator_settings& settings) override {
        if (m_audio) m_audio->set_mix_levels(settings.master_volume,
                                             settings.music_volume,
                                             settings.effects_volume);
    }
    void set_audio_paused(bool paused) override {
        if (m_audio) m_audio->set_paused(paused);
    }

private:
    void apply_inputs(const input_state& state) {
        uint8_t system = 0xff;
        if (state.start) system &= ~0x01;
        if (state.p2_start) system &= ~0x02;
        if (state.service) system &= ~0x20;
        if (state.coin1) system &= ~0x40;
        if (state.coin2) system &= ~0x80;
        const uint16_t dips = m_cabinet->gng_dip_switches;
        m_machine->set_inputs(
            system,
            gng::encode_player_port(state.left_stick_x, state.left_stick_y,
                                    state.buttons[0], state.buttons[1]),
            gng::encode_player_port(state.p2_stick_x, state.p2_stick_y,
                                    state.p2_buttons[0], state.p2_buttons[1]),
            static_cast<uint8_t>(dips),
            static_cast<uint8_t>(dips >> 8));
    }

    std::unique_ptr<gng::machine> m_machine;
    std::unique_ptr<gng::audio_system> m_audio;
    std::unique_ptr<arcade_input> m_input;
    uint64_t m_frames{};
};

} // namespace

std::unique_ptr<emulator_session> make_gng_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet) {
    return std::make_unique<gng_emulator>(std::move(video),
                                          std::move(cabinet));
}
