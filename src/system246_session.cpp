// Namco System 246 session: lifecycle glue around split media, core, video,
// audio and cabinet-control components.

#include "arcade_session_internal.h"
#include "arcade_frontend.h"
#include "system246_rom.h"
#include "rom_library.h"

#include "system246/play_audio.h"
#include "system246/play_core.h"
#include "system246/play_input.h"
#include "system246/play_media_cache.h"
#include "system246/play_video.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class system246_emulator final : public video_emulator_session {
public:
    system246_emulator(std::shared_ptr<arcade_video_worker> video,
                       std::shared_ptr<arcade_cabinet_state> cabinet)
        : video_emulator_session(arcade_board_type::system246,
                                 std::move(video), std::move(cabinet)) {}

    ~system246_emulator() override {
        if (m_core) m_core->shutdown();
        if (m_gpu_renderer) m_gpu_renderer->reset_session();
        if (m_input) m_input->shutdown();
    }

    bool initialize(const std::string& rom_path, const std::string&,
                    const emulator_settings& settings) override {
        std::printf("Initializing Namco System 246 backend (Play!)...\n");
        std::printf("RRV ROM path: %s\n", rom_path.c_str());

        const system246_rom_load_result roms =
            system246_rom_loader::load(rom_path, chd_library_path());
        if (!roms) {
            std::fprintf(stderr, "%s\n", roms.error.c_str());
            return false;
        }
        std::string optical_path;
        std::string error;
        if (!prepare_system246_optical_media(
                roms, optical_path, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
        if (!m_gpu_renderer->initialize(settings)) {
            std::fprintf(stderr, "Failed to initialize shared video output\n");
            return false;
        }
        // A normal RRV cabinet remains single-screen. The launcher's explicit
        // Twin Screen mode deliberately requests a second aspect-correct
        // player window, so do not override that runtime choice.
        m_gpu_renderer->set_single_screen_only(
            settings.output == output_mode::single);

        m_input = std::make_unique<arcade_input>();
        if (!m_input->initialize("rrvac"))
            std::fprintf(stderr,
                         "Input initialization failed; controls are neutral\n");
        // D is owned by the frontend and converted into a timed JVS TEST
        // pulse below. Raw F2 is disabled during normal play.
        m_input->set_test_input_enabled(false);

        m_input_channel = std::make_shared<system246_input_channel>();
        m_audio_control = std::make_shared<system246_audio_control>();
        m_audio_control->set_mix_levels(
            settings.master_volume, settings.music_volume,
            settings.effects_volume);
        m_video_bridge = std::make_shared<system246_video_bridge>();
        m_core = std::make_unique<system246_play_core>();
        if (!m_core->initialize(
                roms, optical_path, m_input_channel, m_audio_control,
                m_video_bridge, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }

        m_fps_epoch = std::chrono::steady_clock::now();
        m_epoch_metrics = m_video_bridge->metrics();
        std::printf("%s initialized\n",
                    system246_rom_loader::set_display_name(roms.set));
        return true;
    }

    void run_frame() override {
        if (!m_input || !m_core || !m_video_bridge) return;
        // The Play! VM already owns the PS2 raster clock. Follow its GS flip
        // instead of sampling it from another free-running 60 Hz deadline.
        m_video_bridge->wait_for_frame(std::chrono::milliseconds(25));
        m_input->set_suppressed(m_gpu_renderer->settings_visible());
        m_input->update();
        if (session_trace_enabled() &&
            (m_input->state().coin1 || m_input->state().start)) {
            std::printf("System 246 frontend input coin=%d start=%d\n",
                        m_input->state().coin1, m_input->state().start);
            std::fflush(stdout);
        }
        m_input_channel->set_state(m_input->state());

        int width = 0;
        int height = 0;
        try {
            if (m_video_bridge->capture_latest(
                    m_core->gs_handler(), m_frame_rgba, width, height)) {
                m_gpu_renderer->present_rgba_frame(
                    m_frame_rgba.data(), width, height, 640, 480);
                m_capture_warning_reported = false;
            }
        } catch (const std::exception& exception) {
            if (!m_capture_warning_reported) {
                std::fprintf(stderr,
                             "System 246 frame capture warning: %s\n",
                             exception.what());
                m_capture_warning_reported = true;
            }
        }

        ++m_host_frames;
        if (m_host_frames % 60 == 0 && session_trace_enabled())
            report_performance();
    }

    void enter_service_menu() {
        if (m_input_channel)
            m_input_channel->hold_test_for_frames(120);
    }

    void reload_input_mappings() override {
        if (m_input) m_input->reload_mappings();
    }

    double frame_seconds() const override { return 1.0 / 60.0; }
    bool producer_paced() const override { return true; }

protected:
    operator_menu_definition operator_menu() const override {
        return make_service_operator_menu(
            "SYSTEM 246 OPERATOR SETTINGS",
            "Open the original JVS service menu without leaving the game.");
    }
    void apply_operator_action(const operator_menu_action& action) override {
        if (action.row_id == 0) enter_service_menu();
    }
    void apply_audio_settings(const emulator_settings& settings) override {
        if (m_audio_control)
            m_audio_control->set_mix_levels(
                settings.master_volume, settings.music_volume,
                settings.effects_volume);
    }

    void set_audio_paused(bool paused) override {
        if (m_audio_control) m_audio_control->set_paused(paused);
        if (m_core) m_core->set_paused(paused);
    }

private:
    void report_performance() {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(
            now - m_fps_epoch).count();
        const system246_video_metrics current = m_video_bridge->metrics();
        const double core_fps = elapsed > 0.0 ?
            static_cast<double>(current.produced_frames -
                                m_epoch_metrics.produced_frames) / elapsed :
            0.0;
        const double capture_fps = elapsed > 0.0 ?
            static_cast<double>(current.captured_frames -
                                m_epoch_metrics.captured_frames) / elapsed :
            0.0;
        std::printf(
            "System 246 host=%llu core=%.2f fps capture=%.2f fps "
            "superseded=%llu readback=%.2f/%.2f ms\n",
            static_cast<unsigned long long>(m_host_frames), core_fps,
            capture_fps,
            static_cast<unsigned long long>(current.superseded_frames),
            current.last_readback_microseconds / 1000.0,
            current.peak_readback_microseconds / 1000.0);
        std::fflush(stdout);
        m_fps_epoch = now;
        m_epoch_metrics = current;
    }

    std::unique_ptr<arcade_input> m_input;
    std::unique_ptr<system246_play_core> m_core;
    std::shared_ptr<system246_input_channel> m_input_channel;
    std::shared_ptr<system246_audio_control> m_audio_control;
    std::shared_ptr<system246_video_bridge> m_video_bridge;
    std::vector<uint8_t> m_frame_rgba;
    bool m_capture_warning_reported{};
    uint64_t m_host_frames{};
    std::chrono::steady_clock::time_point m_fps_epoch{};
    system246_video_metrics m_epoch_metrics{};
};

} // namespace

std::unique_ptr<emulator_session> make_system246_session(
        std::shared_ptr<arcade_video_worker> video,
        std::shared_ptr<arcade_cabinet_state> cabinet) {
    return std::make_unique<system246_emulator>(
        std::move(video), std::move(cabinet));
}
