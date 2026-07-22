// Phoenix and discrete-sound Galaxian-family runtime sessions.

#include "arcade_session_internal.h"
#include "galaxian_audio.h"
#include "galaxian_machine.h"
#include "galaxian_rom.h"

#include <cstdio>
#include <stdexcept>
#include <utility>

namespace {

class galaxian_emulator : public video_emulator_session {
private:
    std::unique_ptr<galaxian_machine> m_machine;
    std::unique_ptr<galaxian_audio_system> m_audio;
    std::unique_ptr<arcade_input> m_input;
    galaxian_rom_set m_set{galaxian_rom_set::unknown};
    uint64_t m_frame_number{0};

public:
    galaxian_emulator(galaxian_rom_set set,
                      std::shared_ptr<arcade_video_worker> video,
                      std::shared_ptr<arcade_cabinet_state> cabinet)
        : video_emulator_session(
              set == galaxian_rom_set::phoenix ? arcade_board_type::phoenix :
                                                 arcade_board_type::mooncrst,
              std::move(video), std::move(cabinet)),
          m_set(set) {}
    ~galaxian_emulator() override {
        shutdown_session_devices(m_audio, m_gpu_renderer, m_input);
    }

    bool initialize(const std::string& rom_path, const std::string&,
                    const emulator_settings& settings) override {
        const galaxian_rom_set detected =
            galaxian_rom_loader::identify_set(rom_path);
        const bool expected_family =
            (m_set == galaxian_rom_set::phoenix &&
             detected == galaxian_rom_set::phoenix) ||
            (m_set == galaxian_rom_set::mooncrst &&
             (detected == galaxian_rom_set::galaxian ||
              detected == galaxian_rom_set::mooncrst ||
              detected == galaxian_rom_set::uniwars));
        if (!expected_family) {
            std::fprintf(stderr,
                         "Selected ROM is not supported by this Galaxian session: %s\n",
                         rom_path.c_str());
            return false;
        }
        m_set = detected;

        m_input = std::make_unique<arcade_input>();
        if (!m_gpu_renderer->initialize(settings)) return false;
        if (!m_input->initialize(galaxian_rom_loader::set_short_name(m_set)))
            return false;
        std::unique_ptr<galaxian_sound_synth> synth;
        if (m_set == galaxian_rom_set::phoenix) {
            m_machine = std::make_unique<galaxian_machine>(
                make_phoenix_board_interface());
            synth = make_phoenix_sound_synth();
        } else if (m_set == galaxian_rom_set::galaxian) {
            m_machine = std::make_unique<galaxian_machine>(
                make_galaxian_board_interface());
            synth = make_galaxian_sound_synth();
        } else if (m_set == galaxian_rom_set::uniwars) {
            m_machine = std::make_unique<galaxian_machine>(
                make_uniwars_board_interface());
            synth = make_uniwars_sound_synth();
        } else {
            m_machine = std::make_unique<galaxian_machine>(
                make_mooncrst_board_interface());
            synth = make_mooncrst_sound_synth();
        }
        if (!m_machine->load_roms(rom_path)) return false;
        m_audio = std::make_unique<galaxian_audio_system>(std::move(synth));
        if (m_audio->initialize()) {
            m_audio->set_mix_levels(settings.master_volume,
                                    settings.music_volume,
                                    settings.effects_volume);
            m_machine->set_sound_write_handler(
                [this](unsigned port, uint8_t data) {
                    if (m_audio) m_audio->write_control(port, data);
                });
            m_audio->start();
        } else {
            std::fprintf(stderr, "%s audio disabled; video will continue\n",
                         galaxian_rom_loader::set_display_name(m_set));
            m_audio.reset();
        }
        m_machine->reset();
        std::printf("%s initialized\n",
                    galaxian_rom_loader::set_display_name(m_set));
        return true;
    }

    void run_frame() override {
        m_input->set_suppressed(m_gpu_renderer->settings_visible());
        m_input->update();
        m_machine->set_input(m_input->state());
        m_machine->run_frame();
        m_gpu_renderer->present_rgba_frame(
            reinterpret_cast<const uint8_t*>(m_machine->frame_buffer()),
            m_machine->screen_width(), m_machine->screen_height());
        if (++m_frame_number % 600 == 0 && session_trace_enabled()) {
            if (m_set == galaxian_rom_set::phoenix) {
                std::printf("Phoenix frame %llu audio=%d latch=%02x/%02x\n",
                            static_cast<unsigned long long>(m_frame_number),
                            m_audio ? m_audio->peak_sample() : 0,
                            m_audio ? m_audio->control(0) : 0,
                            m_audio ? m_audio->control(1) : 0);
            } else {
                std::printf("%s frame %llu audio=%d\n",
                            galaxian_rom_loader::set_display_name(m_set),
                            static_cast<unsigned long long>(m_frame_number),
                            m_audio ? m_audio->peak_sample() : 0);
            }
        }
    }

    double frame_seconds() const override {
        return 1.0 / m_machine->refresh_rate();
    }
    void reload_input_mappings() override {
        if (m_input) m_input->reload_mappings();
    }

protected:
    void apply_audio_settings(const emulator_settings& settings) override {
        if (m_audio)
            m_audio->set_mix_levels(settings.master_volume,
                                    settings.music_volume,
                                    settings.effects_volume);
    }
    void set_audio_paused(bool paused) override {
        if (m_audio) m_audio->set_paused(paused);
    }
};

// =====================================================================
// shinobi_emulator -- emulator session for Shinobi (Sega System 16-B).
// Audio integrates with the in-tree Shinobi synth through a dedicated
// audio thread (set up analogously to the galaxian_audio_system wiring).
// =====================================================================
} // namespace

std::unique_ptr<emulator_session> make_galaxian_session(
    arcade_board_type board,
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet) {
    const galaxian_rom_set set = board == arcade_board_type::phoenix ?
        galaxian_rom_set::phoenix :
        board == arcade_board_type::mooncrst ? galaxian_rom_set::mooncrst :
                                               galaxian_rom_set::unknown;
    if (set == galaxian_rom_set::unknown)
        throw std::invalid_argument("Not a Galaxian-family board type");
    return std::make_unique<galaxian_emulator>(
        set, std::move(video), std::move(cabinet));
}
