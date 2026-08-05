#include "arcade_session_internal.h"
#include "arcade_feedback.h"
#include "namco/galaga/galaga_machine.h"
#include "namco/galaxian/galaxian_audio.h"
#include "namco/namco_rom.h"
#include "namco/system1/system1_machine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
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
        const char* network_link = std::getenv("MANX_INPUT_LINK");
        m_network_two_player = network_link && *network_link &&
            std::strcmp(network_link, "0") != 0;
        const char* node = std::getenv("MANX_CABINET_NODE");
        m_network_host = m_network_two_player && node &&
            std::strcmp(node, "1") == 0;
        m_machine->configure_network_two_player(m_network_two_player);
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
        m_input->set_suppressed(m_gpu_renderer->input_suppressed());
        m_input->update();
        input_state input = m_input->state();

        if (m_network_host && arcade_input_network_peer_seen()) {
            if (!m_two_player_started) {
                // Multiplayer setup owns both start lines. A stray mapped
                // Start 1 must never consume one credit and create a 1P game.
                input.start = false;
                input.p2_start = false;
            }
            if (!m_two_player_started && !m_start_requested) {
                ++m_link_start_frames;
                // Keep inserting clean coin edges until the emulated Namco
                // credit chip itself reports two credits. This accounts for
                // non-default coinage and never assumes a pulse was sampled.
                if (m_link_start_frames >= 240 &&
                    m_machine->credit_count() < 2 &&
                    ((m_link_start_frames - 240) % 30) < 4)
                    input.coin1 = true;
                if (m_link_start_frames >= 300 &&
                    m_machine->credit_count() >= 2) {
                    // Only the original cabinet's 2 PLAYERS line is allowed.
                    // Hold it across several I/O scans, then verify the
                    // emulated Namco credit chip accepted that exact line.
                    m_start_requested = true;
                    m_start2_pulse_frames = 8;
                    m_start_confirmation_frames = 0;
                }
            }
            if (m_start_requested && !m_two_player_started) {
                ++m_start_confirmation_frames;
                if (m_start2_pulse_frames != 0) {
                    input.p2_start = true;
                    --m_start2_pulse_frames;
                } else if (m_machine->credit_count() >= 2 &&
                           (m_start_confirmation_frames % 120) >= 30 &&
                           (m_start_confirmation_frames % 120) < 38) {
                    // Retry only 2 PLAYERS if the game was not polling during
                    // the first pulse window.
                    input.p2_start = true;
                } else if (m_machine->credit_count() < 2 &&
                           (m_start_confirmation_frames % 30) < 4) {
                    // Unusual DIP coinage can require more coin edges.
                    input.coin1 = true;
                }
            }
        } else if (m_network_host) {
            m_link_start_frames = 0;
        }

        m_machine->set_input(input);
        m_machine->run_frame();
        present_frame();

        if (m_network_host && m_start_requested &&
            !m_two_player_started &&
            m_machine->selected_players() == 2) {
            m_two_player_started = true;
            std::printf(
                "Galaga multiplayer: Namco I/O confirmed 2 credits and "
                "2 PLAYERS start\n");
        }
        if (++m_frames % 600 == 0 && session_trace_enabled())
            std::printf("Galaga frame %llu pc=%04x audio=%d\n",
                static_cast<unsigned long long>(m_frames),
                m_machine->program_counter(),
                m_audio ? m_audio->peak_sample() : 0);
    }

    double frame_seconds() const override {
        return 1.0 / namco::galaga_machine::refresh_hz;
    }
    int active_player() const override {
        return m_machine ? m_machine->active_player() : -1;
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
    void present_frame() {
        m_gpu_renderer->present_rgba_frame(
            reinterpret_cast<const uint8_t*>(m_machine->framebuffer()),
            namco::galaga_machine::width, namco::galaga_machine::height,
            3, 4);
    }

    std::unique_ptr<namco::galaga_machine> m_machine;
    std::unique_ptr<galaxian_audio_system> m_audio;
    std::unique_ptr<arcade_input> m_input;
    uint64_t m_frames{};
    uint32_t m_link_start_frames{};
    uint32_t m_start_confirmation_frames{};
    uint8_t m_start2_pulse_frames{};
    bool m_network_two_player{};
    bool m_network_host{};
    bool m_two_player_started{};
    bool m_start_requested{};
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
        // Both Namco System 1 games run on this session; the board, video
        // path and audio are identical and only the ROM set differs.
        const bool is_galaga88 = loaded.set == namco::rom_set::galaga88;
        if (!loaded || (loaded.set != namco::rom_set::pacmania &&
                        !is_galaga88)) {
            std::fprintf(stderr, "Namco System 1 ROM error: %s\n",
                         loaded.error.c_str());
            return false;
        }
        m_machine = std::make_unique<namco::system1_machine>();
        m_input = std::make_unique<arcade_input>();
        if (!m_gpu_renderer->initialize(settings) ||
            !m_input->initialize(is_galaga88 ? "galaga88" : "pacmania") ||
            !(is_galaga88 ? m_machine->initialize(loaded.galaga88)
                          : m_machine->initialize(loaded.pacmania)))
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
        using clock = std::chrono::steady_clock;
        const auto frame_begin = clock::now();
        m_input->set_suppressed(m_gpu_renderer->input_suppressed());
        m_input->update();
        const input_state input = m_input->state();
        const auto input_done = clock::now();
        uint8_t dips = m_cabinet->system1_dip_switches;
        if (input.test) dips &= ~uint8_t{0x01};
        m_machine->set_input(input, dips);
        m_machine->run_frame();
        const namco::pacmania_feedback_signal feedback =
            m_machine->take_feedback_signal();
        switch (feedback.event) {
        case namco::pacmania_feedback_event::pellet:
            arcade_feedback::publish_gameplay_event(
                arcade_feedback::gameplay_event_kind::pellet);
            break;
        case namco::pacmania_feedback_event::power_pellet:
            arcade_feedback::publish_gameplay_event(
                arcade_feedback::gameplay_event_kind::power_pellet);
            break;
        case namco::pacmania_feedback_event::ghost_eaten:
            arcade_feedback::publish_gameplay_event(
                arcade_feedback::gameplay_event_kind::ghost_eaten,
                feedback.level);
            break;
        case namco::pacmania_feedback_event::death:
            arcade_feedback::publish_gameplay_event(
                arcade_feedback::gameplay_event_kind::death);
            break;
        case namco::pacmania_feedback_event::none:
            break;
        }
        const auto machine_done = clock::now();
        m_gpu_renderer->present_rgba_frame(
            reinterpret_cast<const uint8_t*>(m_machine->framebuffer()),
            namco::system1_machine::width, namco::system1_machine::height,
            3, 4);
        const auto present_done = clock::now();
        ++m_frames;
        const bool trace = session_trace_enabled();
        if (trace && (input.coin1 != m_last_coin || input.start != m_last_start)) {
            std::printf("Pac-Mania input frame=%llu coin=%d start=%d dips=%02x\n",
                static_cast<unsigned long long>(m_frames), input.coin1,
                input.start, dips);
            std::fflush(stdout);
        }
        m_last_coin = input.coin1;
        m_last_start = input.start;
        const auto millis = [](clock::duration value) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(value)
                .count();
        };
        const auto total_ms = millis(present_done - frame_begin);
        if (trace && (m_frames % 60 == 0 || total_ms >= 100)) {
            std::printf(
                "Pac-Mania frame=%llu pc=%04x fault=%d audio=%d "
                "phase_ms=%lld/%lld/%lld total=%lld suppressed=%d\n",
                static_cast<unsigned long long>(m_frames),
                m_machine->program_counter(), m_machine->fault(),
                m_audio ? m_audio->peak_sample() : 0,
                static_cast<long long>(millis(input_done - frame_begin)),
                static_cast<long long>(millis(machine_done - input_done)),
                static_cast<long long>(millis(present_done - machine_done)),
                static_cast<long long>(total_ms),
                m_gpu_renderer->input_suppressed());
            std::fflush(stdout);
        }
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
    bool m_last_coin{};
    bool m_last_start{};
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
