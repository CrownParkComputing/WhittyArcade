#include "arcade_session_internal.h"
#include "namco/galaga/galaga_machine.h"
#include "namco/galaxian/galaxian_audio.h"
#include "namco/namco_rom.h"
#include "namco/system1/system1_machine.h"

#include <cstdio>
#include <utility>

namespace {

class galaga_emulator final : public video_emulator_session {
public:
    galaga_emulator(std::shared_ptr<arcade_video_worker> video,
                    std::shared_ptr<arcade_cabinet_state> cabinet)
        : video_emulator_session(arcade_board_type::namco_galaga,
                                 std::move(video), std::move(cabinet)) {}
    ~galaga_emulator() override {
        shutdown_session_devices(m_audio, m_gpu_renderer, m_input);
    }

    bool initialize(const std::string& path, const std::string&,
                    const emulator_settings& settings) override {
        const namco::load_result loaded = namco::rom_loader::load(path);
        if (!loaded || loaded.set != namco::rom_set::galaga) {
            std::fprintf(stderr, "Galaga ROM error: %s\n",
                         loaded.error.c_str());
            return false;
        }
        m_machine = std::make_unique<namco::galaga_machine>();
        m_input = std::make_unique<arcade_input>();
        if (!m_gpu_renderer->initialize(settings) ||
            !m_input->initialize("galaga") ||
            !m_machine->initialize(loaded.galaga))
            return false;
        m_audio = std::make_unique<galaxian_audio_system>(
            make_galaga_sound_synth(loaded.galaga.waveform));
        if (m_audio->initialize()) {
            m_audio->set_mix_levels(settings.master_volume,
                                    settings.music_volume,
                                    settings.effects_volume);
            m_machine->set_sound_write_handler(
                [this](unsigned port, uint8_t value) {
                    if (m_audio) m_audio->write_control(port, value);
                });
            m_audio->start();
        } else {
            std::fprintf(stderr,
                         "Galaga audio disabled; video will continue\n");
            m_audio.reset();
        }
        std::printf("Galaga (Namco 1981) initialized\n");
        return true;
    }

    void run_frame() override {
        m_input->set_suppressed(m_gpu_renderer->settings_visible());
        m_input->update();
        m_machine->set_input(m_input->state());
        m_machine->run_frame();
        m_gpu_renderer->present_rgba_frame(
            reinterpret_cast<const uint8_t*>(m_machine->framebuffer()),
            namco::galaga_machine::width, namco::galaga_machine::height);
        if (++m_frames % 600 == 0 && session_trace_enabled())
            std::printf("Galaga frame %llu pc=%04x audio=%d\n",
                static_cast<unsigned long long>(m_frames),
                m_machine->program_counter(),
                m_audio ? m_audio->peak_sample() : 0);
    }

    double frame_seconds() const override {
        return 1.0 / namco::galaga_machine::refresh_hz;
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
    std::unique_ptr<namco::galaga_machine> m_machine;
    std::unique_ptr<galaxian_audio_system> m_audio;
    std::unique_ptr<arcade_input> m_input;
    uint64_t m_frames{};
};

class system1_emulator final : public video_emulator_session {
public:
    system1_emulator(std::shared_ptr<arcade_video_worker> video,
                      std::shared_ptr<arcade_cabinet_state> cabinet)
        : video_emulator_session(arcade_board_type::namco_system1,
                                 std::move(video), std::move(cabinet)) {}
    ~system1_emulator() override {
        shutdown_session_devices(m_audio, m_gpu_renderer, m_input);
    }

    bool initialize(const std::string& path, const std::string&,
                    const emulator_settings& settings) override {
        const namco::load_result loaded = namco::rom_loader::load(path);
        if (!loaded || loaded.set != namco::rom_set::pacmania) {
            std::fprintf(stderr, "Pac-Mania ROM error: %s\n",
                         loaded.error.c_str());
            return false;
        }
        m_machine = std::make_unique<namco::system1_machine>();
        m_input = std::make_unique<arcade_input>();
        if (!m_gpu_renderer->initialize(settings) ||
            !m_input->initialize("pacmania") ||
            !m_machine->initialize(loaded.pacmania))
            return false;
        m_audio = std::make_unique<galaxian_audio_system>(
            make_system1_sound_synth());
        if (m_audio->initialize()) {
            m_audio->set_mix_levels(settings.master_volume,
                                    settings.music_volume,
                                    settings.effects_volume);
            m_audio->start();
        } else {
            std::fprintf(stderr,
                         "Pac-Mania audio disabled; video will continue\n");
            m_audio.reset();
        }
        std::printf("Pac-Mania (Namco System 1, 1987) initialized\n");
        return true;
    }

    void run_frame() override {
        m_input->set_suppressed(m_gpu_renderer->settings_visible());
        m_input->update();
        uint8_t dips = m_cabinet->system1_dip_switches;
        if (m_input->state().test) dips &= ~uint8_t{0x01};
        m_machine->set_input(m_input->state(), dips);
        m_machine->run_frame();
        m_gpu_renderer->present_rgba_frame(
            reinterpret_cast<const uint8_t*>(m_machine->framebuffer()),
            namco::system1_machine::width, namco::system1_machine::height);
        if (++m_frames % 600 == 0 && session_trace_enabled())
            std::printf("Pac-Mania frame %llu pc=%04x fault=%d audio=%d\n",
                static_cast<unsigned long long>(m_frames),
                m_machine->program_counter(), m_machine->fault(),
                m_audio ? m_audio->peak_sample() : 0);
    }

    double frame_seconds() const override {
        return 1.0 / namco::system1_machine::refresh_hz;
    }
    void reload_input_mappings() override {
        if (m_input) m_input->reload_mappings();
    }

protected:
    operator_menu_definition operator_menu() const override {
        const uint8_t dips = m_cabinet->system1_dip_switches;
        operator_menu_definition menu;
        menu.title = "PAC-MANIA DIP SWITCHES";
        menu.description =
            "Namco System 1 physical switches; changes apply immediately.";
        menu.rows = {
            {0, "Service mode", {"OFF", "ON"}, (dips & 0x01) ? 0 : 1},
            {1, "Freeze", {"OFF", "ON"}, (dips & 0x02) ? 0 : 1},
            {2, "Kick watchdog in IRQ", {"NO", "YES"},
                 (dips & 0x04) ? 0 : 1},
            {4, "Auto data sampling", {"OFF", "ON"},
                 (dips & 0x10) ? 0 : 1},
            {100, "Restore factory defaults", {}, 0, true},
        };
        return menu;
    }
    void apply_operator_action(const operator_menu_action& action) override {
        if (action.row_id == 100) {
            m_cabinet->system1_dip_switches = 0xff;
            return;
        }
        if (action.row_id < 0 || action.row_id > 7) return;
        const uint8_t mask =
            static_cast<uint8_t>(1u << action.row_id);
        if (action.selected == 0)
            m_cabinet->system1_dip_switches |= mask;
        else
            m_cabinet->system1_dip_switches &= static_cast<uint8_t>(~mask);
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
    std::unique_ptr<namco::system1_machine> m_machine;
    std::unique_ptr<galaxian_audio_system> m_audio;
    std::unique_ptr<arcade_input> m_input;
    uint64_t m_frames{};
};

} // namespace

std::unique_ptr<emulator_session> make_namco_galaga_session(
        std::shared_ptr<arcade_video_worker> video,
        std::shared_ptr<arcade_cabinet_state> cabinet) {
    return std::make_unique<galaga_emulator>(std::move(video),
                                              std::move(cabinet));
}

std::unique_ptr<emulator_session> make_namco_system1_session(
        std::shared_ptr<arcade_video_worker> video,
        std::shared_ptr<arcade_cabinet_state> cabinet) {
    return std::make_unique<system1_emulator>(std::move(video),
                                                std::move(cabinet));
}
