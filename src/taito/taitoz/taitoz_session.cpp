// Taito Z System runtime session.
//
// The sound Z80 and TC0140SYT live in the machine; this owns the YM2610
// and its OpenAL worker, and routes the Z80's chip accesses to them.

#include "arcade_session_internal.h"
#include "taito/taitoz/taitoz_audio.h"
#include "taito/taitoz/taitoz_machine.h"
#include "taito/taitoz/taitoz_rom.h"

#include <cstdio>
#include <utility>

namespace {

class taitoz_emulator : public video_emulator_session {
public:
    taitoz_emulator(std::shared_ptr<arcade_video_worker> video,
                    std::shared_ptr<arcade_cabinet_state> cabinet)
        : video_emulator_session(arcade_board_type::taito_z, std::move(video),
                                 std::move(cabinet)) {}
    ~taitoz_emulator() override {
        shutdown_session_devices(m_audio, m_gpu_renderer, m_input);
    }

    bool initialize(const std::string& rom_path, const std::string&,
                    const emulator_settings& settings) override {
        const taitoz::taitoz_rom_set set =
            taitoz::taitoz_rom_loader::identify_set(rom_path);
        if (set == taitoz::taitoz_rom_set::unknown) {
            std::fprintf(stderr,
                         "Selected ROM is not a supported Taito Z set: %s\n",
                         rom_path.c_str());
            return false;
        }
        m_set = set;
        m_input = std::make_unique<arcade_input>();
        if (!m_gpu_renderer->initialize(settings)) return false;
        if (!m_input->initialize(taitoz::taitoz_rom_loader::set_short_name(set)))
            return false;
        m_machine = std::make_unique<taitoz::taitoz_machine>();

        // The ADPCM sample ROMs stay owned by the loader result for the
        // lifetime of the session, since ymfm reads them on demand.
        m_rom_result = taitoz::taitoz_rom_loader::load(rom_path);
        if (!m_rom_result) {
            std::fprintf(stderr, "Taito Z ROM error: %s\n",
                         m_rom_result.error.c_str());
            return false;
        }
        auto synth = make_taitoz_sound_synth();
        synth->set_adpcm_a(m_rom_result.roms.adpcm_a.data(),
                           m_rom_result.roms.adpcm_a.size());
        synth->set_adpcm_b(m_rom_result.roms.adpcm_b.data(),
                           m_rom_result.roms.adpcm_b.size());
        m_audio = std::make_unique<taitoz_audio_system>(std::move(synth));
        if (m_audio->initialize()) {
            m_audio->set_mix_levels(settings.master_volume,
                                    settings.music_volume,
                                    settings.effects_volume);
            taitoz_audio_system* audio = m_audio.get();
            m_machine->set_ym_handlers(
                [audio](unsigned port, uint8_t data) {
                    audio->write_port(port, data);
                },
                [audio](unsigned port) { return audio->read_port(port); },
                [audio](uint32_t clocks) {
                    audio->advance_timer_clocks(clocks);
                },
                [audio] { return audio->irq_asserted(); });
            m_audio->start();
        } else {
            std::fprintf(stderr,
                         "Taito Z audio disabled; video will continue\n");
            m_audio.reset();
        }

        if (!m_machine->load_roms(rom_path)) return false;
        m_machine->reset();
        std::printf("%s initialized\n",
                    taitoz::taitoz_rom_loader::set_display_name(set));
        return true;
    }

    void run_frame() override {
        m_input->set_suppressed(m_gpu_renderer->input_suppressed());
        m_input->update();
        m_machine->set_input(m_input->state());
        m_machine->run_frame();
        m_gpu_renderer->present_rgba_frame(
            reinterpret_cast<const uint8_t*>(m_machine->frame_buffer()),
            m_machine->screen_width(), m_machine->screen_height(), 4, 3);
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

private:
    std::unique_ptr<taitoz::taitoz_machine> m_machine;
    std::unique_ptr<taitoz_audio_system> m_audio;
    taitoz::taitoz_rom_load_result m_rom_result;
    std::unique_ptr<arcade_input> m_input;
    taitoz::taitoz_rom_set m_set{taitoz::taitoz_rom_set::unknown};
};

}  // namespace

std::unique_ptr<emulator_session> make_taitoz_session(
        std::shared_ptr<arcade_video_worker> video,
        std::shared_ptr<arcade_cabinet_state> cabinet) {
    return std::make_unique<taitoz_emulator>(std::move(video),
                                             std::move(cabinet));
}
