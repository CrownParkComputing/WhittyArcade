// arcade_video_worker.h - board-neutral graphics/presentation worker
#pragma once

#include "arcade_settings.h"
#include "namco/system22/system22_types.h"
#include "sega/model2/model2_gpu_frame.h"
#include "operator_menu.h"

#include <array>
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

    // True when the board must ignore controls. Boards ask this rather than
    // testing the settings menu directly, so what counts as "not now" is
    // decided in one place instead of ten.
    bool input_suppressed() const { return m_settings_visible.load(); }

    // -1 until this arcade-wall column's keyboard focus changes; then 1 when
    // it should be heard and 0 when it should not.
    int wall_audible() const {
        return m_wall_audible.load(std::memory_order_acquire);
    }
    bool paused() const { return m_paused.load(); }
    void refresh_output();
    // Some cabinets expose exactly one video output. Keep the user's global
    // dual-screen preference intact while constraining only that session.
    void set_single_screen_only(bool enabled);
    void set_cabinet_status(std::string status);
    void set_board_name(std::string short_name);

    void set_rom_choices(std::vector<rom_choice> choices);
    void set_operator_menu(operator_menu_definition menu);
    bool take_rom_selection(std::string& path);
    bool take_operator_action(operator_menu_action& action);
    bool take_controls_request();
    bool take_game_picker_request();
    bool take_settings_change(emulator_settings& settings);
    // Arcade wall: whether this column currently has keyboard focus and so
    // should be the one you hear. -1 until the first focus event arrives.
    void set_lightgun_cursor(bool enabled, uint8_t player = 0);

    // Where the emulated picture was last drawn (drawable pixels, origin
    // top-left) and the drawable's size, snapshotted from the renderer after
    // each present. The light gun maps the pointer onto this rather than
    // guessing the layout from the window size. False until a frame is drawn.
    bool picture_rect(int& x, int& y, int& width, int& height,
                      int& drawable_width, int& drawable_height) const;
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
    // display_width/height describe the intended monitor aspect independently
    // of the framebuffer's pixel geometry (PS2 modes use non-square pixels).
    void present_rgba_frame(const uint8_t* pixels, int width, int height,
                            int display_width = 0,
                            int display_height = 0);
    void present_model2_frame(model2_gpu_frame frame);
    void read_framebuffer(uint32_t* output);

private:
    void snapshot_picture_rect(polygon_renderer_gpu& renderer);
    struct picture_rect_snapshot {
        int x{0};
        int y{0};
        int width{0};
        int height{0};
        int drawable_width{0};
        int drawable_height{0};
    };
    mutable std::mutex m_picture_rect_mutex;
    picture_rect_snapshot m_picture_rect;

    using task = std::function<void(polygon_renderer_gpu&)>;
    struct rgba_frame {
        std::vector<uint8_t> pixels;
        int width{};
        int height{};
        int display_width{};
        int display_height{};
    };

    void worker_loop(emulator_settings settings);
    void enqueue(task work);
    void harvest_frontend(polygon_renderer_gpu& renderer);

    std::unique_ptr<polygon_renderer_gpu> m_renderer;
    std::thread m_thread;
    std::mutex m_task_mutex;
    std::condition_variable m_task_ready;
    std::deque<task> m_tasks;
    std::optional<rgba_frame> m_pending_rgba_frame;
    bool m_rgba_task_queued{};
    std::atomic<int> m_wall_audible{-1};
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
    std::deque<operator_menu_action> m_operator_actions;
    bool m_controls_pending{};
    bool m_game_picker_pending{};
    bool m_settings_pending{};
    emulator_settings m_changed_settings{};

    std::mutex m_start_mutex;
    std::condition_variable m_started;
    bool m_start_complete{};
    bool m_start_success{};
};
