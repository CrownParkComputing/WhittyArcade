// arcade_video_worker.h - board-neutral graphics/presentation worker
#pragma once

#include "arcade_settings.h"
#include "system22_types.h"
#include "model2_gpu_frame.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class polygon_renderer_gpu;

// Board-neutral presentation worker. It exclusively owns SDL, OpenGL and the
// selected host output backend, so neither System 22 nor Model 1 emulation can
// be stalled inside graphics API calls. Board threads submit immutable data.
class arcade_video_worker {
public:
    arcade_video_worker();
    ~arcade_video_worker();

    arcade_video_worker(const arcade_video_worker&) = delete;
    arcade_video_worker& operator=(const arcade_video_worker&) = delete;

    bool initialize(const emulator_settings& settings);
    void shutdown();
    // Drop presentation work from the outgoing board and wait until the
    // renderer reaches a clean session boundary. The host window/context is
    // deliberately retained across ROM changes.
    void reset_session();
    // Execute an SDL modal on the video owner thread and wait for it to close.
    void run_modal(std::function<void()> action);
    arcade_host_action process_events() const;
    bool settings_visible() const { return m_settings_visible.load(); }
    bool paused() const { return m_paused.load(); }
    void refresh_output();

    void set_rom_choices(std::vector<rom_choice> choices);
    bool take_rom_selection(std::string& path);
    bool take_dip_request();
    bool take_controls_request();
    bool take_settings_change(emulator_settings& settings);
    void set_f2_opens_dip(bool enabled);
    void set_lightgun_cursor(bool enabled, uint8_t player = 0);
    void apply_display_settings(const emulator_settings& settings);

    void submit_polygons(const polygon_object* polygons, int count);
    void submit_textures(const uint8_t* texture_rom, std::size_t texture_size,
                         const uint8_t* tilemap_rom, std::size_t tilemap_size,
                         std::size_t region_offset = 0x800000,
                         std::size_t bank_count = 4,
                         bool tile_high_bit_from_attr = true);
    void submit_gamma(const uint8_t* gamma_proms, std::size_t size);
    void submit_sprites(const uint8_t* sprite_rom, std::size_t size);
    void set_system22_layer_mask(uint8_t mask);
    void submit_palette(const uint8_t* palette_ram, std::size_t size);
    void submit_text_layer(const uint8_t* character_ram,
                           std::size_t character_size,
                           const uint8_t* text_ram, std::size_t text_size,
                           const uint8_t* text_attributes,
                           const uint8_t* mixer, std::size_t mixer_size,
                           bool super_system22);
    void render_scene(const view_matrix& view, const rgba_color& fog_color);
    void present_rgba_frame(const uint8_t* pixels, int width, int height);
    void present_model2_frame(model2_gpu_frame frame);
    void read_framebuffer(uint32_t* output);

private:
    using task = std::function<void(polygon_renderer_gpu&)>;
    void worker_loop(emulator_settings settings);
    void enqueue(task work);
    void harvest_frontend(polygon_renderer_gpu& renderer);

    std::unique_ptr<polygon_renderer_gpu> m_renderer;
    std::thread m_thread;
    std::mutex m_task_mutex;
    std::condition_variable m_task_ready;
    std::deque<task> m_tasks;
    std::optional<model2_gpu_frame> m_pending_model2_frame;
    bool m_model2_task_queued{};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_alive{false};
    std::atomic<arcade_host_action> m_host_action{
        arcade_host_action::continue_running};
    std::atomic<bool> m_settings_visible{false};
    std::atomic<bool> m_paused{false};

    std::mutex m_frontend_mutex;
    std::string m_selected_rom;
    bool m_rom_pending{};
    bool m_dip_pending{};
    bool m_controls_pending{};
    bool m_settings_pending{};
    emulator_settings m_changed_settings{};

    std::mutex m_start_mutex;
    std::condition_variable m_started;
    bool m_start_complete{};
    bool m_start_success{};
};
