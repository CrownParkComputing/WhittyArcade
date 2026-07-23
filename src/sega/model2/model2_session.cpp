// Sega Model 2 runtime session.

#include "arcade_session_internal.h"
#include "arcade_frontend.h"
#include "sega/model1/model1_audio.h"
#include "sega/model1/model1_rom.h"
#include "sega/model2/model2_audio.h"
#include "sega/model2/model2_machine.h"
#include "sega/model2/model2_rom.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace {

class model2_emulator : public video_emulator_session {
public:
    model2_emulator(std::shared_ptr<arcade_video_worker> video,
                    std::shared_ptr<arcade_cabinet_state> cabinet)
        : video_emulator_session(arcade_board_type::model2, std::move(video),
                                 std::move(cabinet)) {}

    ~model2_emulator() override {
        stop_cpu_worker();
        // The CPU worker is the only caller of the MultiPCM board's
        // signal_frame/enqueue_uart, so it must be joined before the board is
        // torn down. Then fall through to the shared teardown for the SCSP
        // board (null for Virtua Cop), video worker and input watch.
        if (m_pcm_audio) m_pcm_audio->shutdown();
        shutdown_session_devices(m_audio, m_gpu_renderer, m_input);
    }

    bool initialize(const std::string& rom_path, const std::string&,
                    const emulator_settings& settings) override {
        if (!m_gpu_renderer->initialize(settings)) return false;
        m_machine = std::make_unique<model2_machine>();
        if (!m_machine->initialize(rom_path)) return false;
        // Resolve game identity once, here at the composition root; the board
        // hardware reads this profile rather than switching on the rom set.
        const model2_game_profile profile =
            model2_rom_loader::profile_for(m_machine->rom_set());
        // Light-gun cabinets replace the desktop pointer with the on-screen P1
        // gun sight, exactly like Time Crisis.
        m_gpu_renderer->set_lightgun_cursor(profile.lightgun);
        // Model 2 sound runs independently from the i960, geometry and video
        // workers.  It is enabled for normal play; the environment override
        // is retained only as a diagnostic escape hatch.
        const char* model2_audio = std::getenv("WHITTY_MODEL2_AUDIO");
        if (!model2_audio || std::strcmp(model2_audio, "0") != 0) {
            if (profile.sound == model2_game_profile::sound_board::segam1audio)
                initialize_multipcm_audio(settings);
            else
                initialize_scsp_audio(settings);
        } else {
            std::printf("Model 2 audio disabled by WHITTY_MODEL2_AUDIO=0\n");
        }
        m_input = std::make_unique<arcade_input>();
        if (!m_input->initialize(
                model2_rom_loader::set_short_name(m_machine->rom_set())))
            std::fprintf(stderr,
                         "Input initialization failed; controls are neutral\n");
        // Do not assert the cabinet TEST line in the middle of a race. D asks
        // the host to restart Model 2 with TEST held safely at boot.
        m_input->set_test_input_enabled(false);
        m_cpu_running.store(true, std::memory_order_release);
        m_cpu_thread = std::thread(&model2_emulator::cpu_worker_loop, this);
        std::printf("Model 2A-CRX bring-up: %s; i960 firmware running\n",
                    model2_rom_loader::set_display_name(m_machine->rom_set()));
        return true;
    }

    void run_frame() override {
        m_input->set_suppressed(m_gpu_renderer->settings_visible());
        m_input->update();
        input_state next = m_input->state();
        // Sega Model 2 driving cabinets expose one VR/view-change line rather
        // than the four dedicated System 22 view inputs. Accept any face-view
        // binding on that single cabinet switch so keyboard and controllers
        // behave consistently.
        next.view = next.view || next.view2 || next.view3 || next.view4;
        next.view2 = next.view3 = next.view4 = false;
        {
            std::lock_guard<std::mutex> lock(m_cpu_mutex);
            // Bound latency at two emulated frames. If the CPU worker is
            // momentarily late, preserve cabinet edges while replacing stale
            // analog values with the newest steering/pedal position.
            if (m_cpu_inputs.size() < 2) {
                m_cpu_inputs.push_back(next);
            } else {
                input_state& pending = m_cpu_inputs.back();
                next.coin1 |= pending.coin1;
                next.coin2 |= pending.coin2;
                next.start |= pending.start;
                next.service |= pending.service;
                next.test |= pending.test;
                next.shift_down |= pending.shift_down;
                next.shift_up |= pending.shift_up;
                next.view |= pending.view;
                pending = next;
            }
        }
        m_cpu_ready.notify_one();
    }

    void enter_service_menu() {
        // 120 frames is long enough for Sega Rally to sample TEST during its
        // reset sequence and reach the operator menu. Releasing it afterward
        // lets F2 act as the original menu's TEST/select button.
        m_service_reset_requested.store(true, std::memory_order_release);
        m_service_boot_frames.store(120, std::memory_order_release);
        m_service_session.store(true, std::memory_order_release);
        m_input->set_test_input_enabled(true);
    }
    void reload_input_mappings() override {
        if (m_input) m_input->reload_mappings();
    }
    double frame_seconds() const override {
        return 1.0 / model2_machine::refresh_rate;
    }

protected:
    operator_menu_definition operator_menu() const override {
        return make_service_operator_menu(
            "SEGA MODEL 2 OPERATOR SETTINGS",
            "The original cabinet service menu will restart the board.");
    }
    void apply_operator_action(const operator_menu_action& action) override {
        if (action.row_id == 0) enter_service_menu();
    }
    void apply_audio_settings(const emulator_settings& settings) override {
        if (m_audio)
            m_audio->set_mix_levels(settings.master_volume,
                                    settings.music_volume,
                                    settings.effects_volume);
        if (m_pcm_audio)
            m_pcm_audio->set_mix_levels(settings.master_volume,
                                        settings.music_volume,
                                        settings.effects_volume);
    }
    void set_audio_paused(bool paused) override {
        // The Model 2 sound board is clocked by the audio device, not the
        // machine thread; freeze it explicitly while menus are open.
        if (m_audio) m_audio->set_paused(paused);
        if (m_pcm_audio) m_pcm_audio->set_paused(paused);
    }

private:
    // Model 2A SCSP sound board (Sega Rally, Virtua Cop 2). Device-clocked on
    // its own thread and answers the i960 over SCSP MIDI-out.
    void initialize_scsp_audio(const emulator_settings& settings) {
        m_audio = std::make_unique<model2_audio_system>();
        if (m_audio->initialize(m_machine->roms())) {
            m_audio->set_mix_levels(settings.master_volume,
                                    settings.music_volume,
                                    settings.effects_volume);
            m_machine->set_sound_uart_callback([this](uint8_t data) {
                if (m_audio) m_audio->enqueue_midi(data);
            });
            m_audio->set_midi_out_callback([this](uint8_t data) {
                if (m_machine) m_machine->sound_midi_receive(data);
            });
            m_audio->start();
        } else {
            std::fprintf(stderr,
                         "Model 2 audio disabled; video will continue\n");
            m_audio.reset();
        }
    }

    // Original Model 2 Sega Model 1 sound board (Virtua Cop): 68000 + YM3438 +
    // dual MultiPCM. It is frame-paced (signal_frame) rather than device
    // clocked, and reads sound commands as MIDI bytes over the same i8251 UART.
    void initialize_multipcm_audio(const emulator_settings& settings) {
        const model2_roms& roms = m_machine->roms();
        model1_roms sound_roms;
        sound_roms.sound_cpu = roms.sound_cpu;
        sound_roms.samples_1 = roms.multipcm_samples_1;
        sound_roms.samples_2 = roms.multipcm_samples_2;
        m_pcm_audio = std::make_unique<model1_audio_system>();
        if (m_pcm_audio->initialize(sound_roms)) {
            m_pcm_audio->set_mix_levels(settings.master_volume,
                                        settings.music_volume,
                                        settings.effects_volume);
            // The i960 sends sound commands as MIDI bytes over the i8251 UART.
            // A zero timestamp makes the board deliver each byte on its next
            // buffer regardless of how far the audio thread has advanced, so
            // the i960 never depends on the audio keeping pace.
            m_machine->set_sound_uart_callback([this](uint8_t data) {
                if (m_pcm_audio) m_pcm_audio->enqueue_uart(data, 0);
            });
            m_pcm_audio->start();
        } else {
            std::fprintf(stderr,
                         "Model 2 audio disabled; video will continue\n");
            m_pcm_audio.reset();
        }
    }

    void stop_cpu_worker() {
        m_cpu_running.store(false, std::memory_order_release);
        m_cpu_ready.notify_all();
        if (m_cpu_thread.joinable()) m_cpu_thread.join();
        if (m_machine && !m_machine->flush_nvram())
            std::fprintf(stderr, "Could not save Model 2 NVRAM on exit\n");
    }

    void cpu_worker_loop() {
        auto epoch = std::chrono::steady_clock::now();
        uint64_t epoch_frame = 0;
        double machine_ms = 0.0;
        double layers_ms = 0.0;
        double packet_ms = 0.0;
        while (m_cpu_running.load(std::memory_order_acquire)) {
            input_state input;
            {
                std::unique_lock<std::mutex> lock(m_cpu_mutex);
                m_cpu_ready.wait(lock, [this] {
                    return !m_cpu_running.load(std::memory_order_acquire) ||
                           !m_cpu_inputs.empty();
                });
                if (!m_cpu_running.load(std::memory_order_relaxed)) break;
                input = m_cpu_inputs.front();
                m_cpu_inputs.pop_front();
            }

            if (m_service_reset_requested.exchange(
                    false, std::memory_order_acq_rel)) {
                m_machine->reset_preserving_nvram();
            }
            const int service_frames = m_service_boot_frames.load(
                std::memory_order_acquire);
            if (service_frames > 0) {
                input.test = true;
                m_service_boot_frames.fetch_sub(1,
                                                 std::memory_order_acq_rel);
            }

            // This worker exclusively owns all Model 2 board state: i960,
            // MB86233 TGP, I/O, geometry decode and immutable GPU snapshots.
            m_machine->set_inputs(input);
            const auto machine_begin = std::chrono::steady_clock::now();
            m_machine->run_frame(false);
            const auto machine_end = std::chrono::steady_clock::now();
            m_machine->render_video_layers();
            const auto layers_end = std::chrono::steady_clock::now();
            model2_gpu_frame gpu_frame = m_machine->make_gpu_frame();
            const auto packet_end = std::chrono::steady_clock::now();
            m_gpu_renderer->present_model2_frame(std::move(gpu_frame));

            // Advance the MultiPCM board (Virtua Cop) by one frame without
            // ever blocking: if the audio thread is behind, this frame is
            // dropped rather than stalling the i960. The SCSP board is
            // device-clocked and needs no signal at all.
            if (m_pcm_audio) m_pcm_audio->signal_frame(false);

            // EXIT from the 2D test menu returns to a 3D attract scene. At
            // that point raw TEST is unsafe again; D remains the clean
            // service-restart hotkey before another race can begin.
            if (m_service_session.load(std::memory_order_acquire) &&
                m_service_boot_frames.load(std::memory_order_acquire) == 0 &&
                m_frame_number > 240 &&
                m_machine->geometry_summary().decoded_polygons > 100) {
                if (!m_machine->flush_nvram())
                    std::fprintf(stderr,
                                 "Could not save Model 2 service settings\n");
                m_input->set_test_input_enabled(false);
                m_service_session.store(false, std::memory_order_release);
            }
            machine_ms += std::chrono::duration<double, std::milli>(
                machine_end - machine_begin).count();
            layers_ms += std::chrono::duration<double, std::milli>(
                layers_end - machine_end).count();
            packet_ms += std::chrono::duration<double, std::milli>(
                packet_end - layers_end).count();

            const uint64_t frame = ++m_frame_number;
            if (frame % 60 == 0 && session_trace_enabled()) {
                const double elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - epoch).count();
                const double cpu_fps = elapsed > 0.0 ?
                    static_cast<double>(frame - epoch_frame) / elapsed : 0.0;
                const double measured_frames =
                    static_cast<double>(frame - epoch_frame);
                const model2_geometry_summary& geometry =
                    m_machine->geometry_summary();
                std::printf("Model 2 frame=%llu cpu=%.2f "
                            "work=%.2f/%.2f/%.2fms IP=%08x cycles=%llu "
                            "sound=%08x/%llu midi=%llu scsp=%llu voices=%d "
                            "peak=%d polys=%u direct=%u zero=%u culled=%u%s "
                            "unmapped=%llu/%llu%s\n",
                            static_cast<unsigned long long>(frame), cpu_fps,
                            machine_ms / measured_frames,
                            layers_ms / measured_frames,
                            packet_ms / measured_frames,
                            m_machine->program_counter(),
                            static_cast<unsigned long long>(
                                m_machine->executed_cycles()),
                            m_audio ? m_audio->program_counter() : 0,
                            static_cast<unsigned long long>(
                                m_audio ? m_audio->executed_cycles() : 0),
                            static_cast<unsigned long long>(
                                m_audio ? m_audio->midi_bytes() : 0),
                            static_cast<unsigned long long>(
                                m_audio ? m_audio->scsp_writes() : 0),
                            m_audio ? m_audio->active_voices() : 0,
                            m_audio ? m_audio->peak_sample() : 0,
                            geometry.decoded_polygons,
                            geometry.direct_polygons,
                            geometry.zero_length_objects,
                            geometry.culled_polygons,
                            geometry.truncated ? " TRUNCATED" : "",
                            static_cast<unsigned long long>(
                                m_machine->unmapped_reads()),
                            static_cast<unsigned long long>(
                                m_machine->unmapped_writes()),
                            m_machine->cpu_faulted() ? " CPU FAULT" : "");
                std::fflush(stdout);
                epoch = std::chrono::steady_clock::now();
                epoch_frame = frame;
                machine_ms = layers_ms = packet_ms = 0.0;
            }
        }
    }

    std::unique_ptr<model2_machine> m_machine;
    std::unique_ptr<model2_audio_system> m_audio;
    // Virtua Cop's Sega Model 1 sound board, used in place of m_audio. Exactly
    // one of the two is non-null.
    std::unique_ptr<model1_audio_system> m_pcm_audio;
    std::unique_ptr<arcade_input> m_input;
    std::thread m_cpu_thread;
    std::mutex m_cpu_mutex;
    std::condition_variable m_cpu_ready;
    std::deque<input_state> m_cpu_inputs;
    std::atomic<bool> m_cpu_running{false};
    std::atomic<bool> m_service_reset_requested{false};
    std::atomic<int> m_service_boot_frames{0};
    std::atomic<bool> m_service_session{false};
    uint64_t m_frame_number{}; // CPU-worker owned.
};

} // namespace

std::unique_ptr<emulator_session> make_model2_session(
    std::shared_ptr<arcade_video_worker> video,
    std::shared_ptr<arcade_cabinet_state> cabinet) {
    return std::make_unique<model2_emulator>(std::move(video),
                                              std::move(cabinet));
}
