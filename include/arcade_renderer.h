// arcade_renderer.h - shared host renderer and output UI
// Uses OpenGL shaders to rasterize renderer-neutral System 22 polygon records.
#pragma once

#include "namco/system22/system22_types.h"
#include "arcade_presenter.h"
#include "bezel_library.h"
#include "arcade_settings.h"
#include "sega/model2/model2_gpu_frame.h"
#include "operator_menu.h"
#include <SDL3_ttf/SDL_ttf.h>
#include <array>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <utility>
#include <chrono>

// Forward declarations
struct opengl_context;
struct SDL_Cursor;
class alternate_presenter;
class network_video_link;

// GPU polygon renderer - processes polygons on GPU instead of CPU
class polygon_renderer_gpu {
private:
    // OpenGL/Vulkan context
    std::unique_ptr<opengl_context> m_ctx;
    std::unique_ptr<alternate_presenter> m_alternate_presenter;
    std::vector<uint8_t> m_present_readback;

    // Settings as the alternate presenters should see them: the session's
    // single-screen constraint overrides the user's dual-output choice.
    emulator_settings effective_presenter_settings(
        const emulator_settings& settings) const;

    // GPU buffers
    uint32_t m_vertex_buffer{0};    // VBO for vertices
    uint32_t m_index_buffer{0};    // IBO for indices
    uint32_t m_texture_buffer{0};  // Texture data
    uint32_t m_sprite_texture{0};
    uint32_t m_tilemap_texture{0};
    uint32_t m_tileattr_texture{0};
    uint32_t m_palette_texture{0};
    uint32_t m_character_texture{0};
    uint32_t m_textmap_texture{0};
    uint32_t m_gamma_texture{0};
    uint32_t m_output_texture{0};  // RGBA8 scene/priority framebuffer texture
    uint32_t m_external_frame_texture{0}; // Complete RGBA frame from another board
    uint32_t m_network_frame_texture{0}; // Authoritative P1 frame on network P2
    uint32_t m_model2_scene_texture{0};
    uint32_t m_model2_base_texture{0};
    uint32_t m_model2_foreground_texture{0};
    uint32_t m_model2_sheet_texture{0};
    uint32_t m_model2_luma_texture{0};
    uint32_t m_model2_color_texture{0};
    uint32_t m_scene_framebuffer{0};
    uint32_t m_model2_framebuffer{0};
    uint32_t m_model2_depth_buffer{0};
    uint32_t m_present_texture{0};
    uint32_t m_present_framebuffer{0};
    int m_present_width{0};
    int m_present_height{0};
    int m_external_frame_width{0};
    int m_external_frame_height{0};
    int m_network_frame_width{0};
    int m_network_frame_height{0};
    int m_network_frame_display_width{0};
    int m_network_frame_display_height{0};
    bool m_network_frame_flip_y{false};
    bool m_network_frame_board_color{false};
    bool m_network_frame_valid{false};
    std::unique_ptr<network_video_link> m_network_video;
    uint32_t m_network_video_sequence{0};
    uint8_t m_network_capture_divider{0};

    // Uniform buffer for camera/view matrix
    uint32_t m_camera_ubo{0};
    uint32_t m_vertex_array{0};
    uint32_t m_graphics_program{0};
    uint32_t m_model2_vertex_buffer{0};
    uint32_t m_model2_vertex_array{0};
    uint32_t m_model2_program{0};
    uint32_t m_model2_overlay_array{0};
    uint32_t m_model2_overlay_program{0};
    uint32_t m_text_vertex_array{0};
    uint32_t m_text_program{0};
    uint32_t m_post_vertex_array{0};
    uint32_t m_post_program{0};
    uint32_t m_settings_vertex_array{0};
    uint32_t m_settings_program{0};
    uint32_t m_settings_texture{0};
    uint32_t m_fps_texture{0};
    // Host-sized copy of the FPS/renderer label. Vulkan and software
    // composite this after scaling the native arcade framebuffer, otherwise
    // a 20 px label on a 224-line game becomes enormous in the host window.
    std::vector<uint8_t> m_fps_pixels;
    // Host copies of the composited panels for the Vulkan overlay pipeline,
    // plus a single transparent texel used to clear a slot that should no
    // longer be shown.
    std::vector<uint8_t> m_settings_pixels;
    std::array<std::vector<uint8_t>, 2> m_player_label_pixels;
    std::array<uint8_t, 4> m_blank_overlay{};
    TTF_Font* m_settings_font{nullptr};
    int m_fps_texture_width{0};
    int m_fps_texture_height{0};
    std::string m_cabinet_status;
    // Player 1/2 corner labels for dual output (built once, drawn per pane).
    uint32_t m_player_label_texture[2]{0, 0};
    int m_player_label_w[2]{0, 0};
    int m_player_label_h[2]{0, 0};
    bool m_player_labels_ready{false};
    uint64_t m_fps_frame_count{0};
    double m_last_fps{0.0};
    std::chrono::steady_clock::time_point m_fps_epoch{};
    uint32_t m_texture_address_base{0x800000};
    bool m_texture_tile_high_bit_from_attr{true};
    int m_text_scroll_x{0};
    int m_text_scroll_y{0};
    int m_text_palette_base{0};
    uint32_t m_screen_fade_r{0x100};
    uint32_t m_screen_fade_g{0x100};
    uint32_t m_screen_fade_b{0x100};
    bool m_screen_fade_initialized{false};
    std::vector<polygon_object> m_temporal_shadow_polygons;
    uint8_t m_temporal_shadow_age{2};
    bool m_super_system22_video{false};
    uint8_t m_super_screen_fade_r{0};
    uint8_t m_super_screen_fade_g{0};
    uint8_t m_super_screen_fade_b{0};
    uint8_t m_super_screen_fade_factor{0};
    uint8_t m_super_mixer_flags{0};
    uint8_t m_super_poly_fade_r{0xff};
    uint8_t m_super_poly_fade_g{0xff};
    uint8_t m_super_poly_fade_b{0xff};
    bool m_super_poly_fade_enabled{false};
    uint8_t m_super_poly_alpha_color{0};
    uint8_t m_super_poly_alpha_pen{0};
    uint8_t m_super_poly_alpha_factor{0};
    uint8_t m_system22_layer_mask{0x07};
    std::array<SDL_Cursor*, 2> m_lightgun_cursors{};
    uint8_t m_lightgun_cursor_player{0};
    bool m_lightgun_cursor_enabled{false};

    // Producer/consumer boundary: CPU/DSP threads only copy neutral polygon
    // records here. All OpenGL calls stay on the render/context thread.
    std::mutex m_queue_mutex;
    std::vector<polygon_object> m_queued_polygons;

    // Thread synchronization
    std::atomic<int> m_pending_polygons{0};
    bool m_headless{false};
    // True while the Vulkan native path owns the visible frame, so a redraw
    // must come from its resident image rather than a stale OpenGL texture.
    bool m_native_frame_live{false};
    // Arcade wall: whether this column currently holds keyboard focus, and
    // whether that has changed since the host last asked.
    bool m_wall_column_active{true};
    bool m_wall_audio_changed{false};
    bool m_native_decline_reported{false};
    // Cabinet bezel for the running board. Fetched in the
    // background; absent until it arrives, and absent forever
    // for a board The Bezel Project never published.
    // GL-window wall placement, used after a device-loss failover.
    bool m_gl_wall_placed{};
    int m_gl_wall_countdown{};
    int m_gl_wall_focus{-2};
    int m_gl_wall_x{}, m_gl_wall_width{}, m_gl_wall_height{};
    bezel::library m_bezels;
    std::string m_bezel_board;
    std::vector<uint8_t> m_bezel_pixels;
    // Arcade wall: whether this column currently holds keyboard focus, and a
    // one-shot flag telling the host loop to apply the resulting volume.
    bool m_initialized{false};
    bool m_controls_requested{false};
    bool m_game_picker_requested{false};
    bool m_settings_visible{false};
    bool m_settings_changed{false};
    bool m_paused{false};
    bool m_pause_menu_visible{false};
    bool m_settings_texture_dirty{true};
    bool m_ttf_initialized{false};
    int m_settings_selection{0};
    int m_pause_menu_selection{0};
    int m_menu_axis_x{0};
    int m_menu_axis_y{0};
    bool m_rom_menu_visible{false};
    bool m_operator_menu_visible{false};
    int m_operator_selection{0};
    operator_menu_definition m_operator_menu;
    std::vector<operator_menu_action> m_operator_actions;
    int m_rom_selection{0};
    std::vector<rom_choice> m_rom_choices;
    std::string m_selected_rom;
    bool m_rom_selection_pending{false};
    emulator_settings m_display_settings{};
    renderer_backend m_active_backend{renderer_backend::opengl};
    bool m_single_screen_only{false};

    bool create_graphics_pipeline();
    bool create_model2_pipeline();
    bool create_text_pipeline();
    bool create_post_pipeline();
    bool create_settings_overlay();
    bool configure_output_backend(renderer_backend backend,
                                  const emulator_settings& settings);
    void destroy_graphics_pipeline();
    void destroy_model2_pipeline();
    void destroy_text_pipeline();
    void destroy_post_pipeline();
    void destroy_settings_overlay();
    void adjust_setting(int direction);
    bool activate_pause_menu();
    void select_pause_menu_row(int direction);
    void show_pause_menu();
    void resume_game();
    void adjust_operator_setting(int direction, bool activate);
    void select_operator_row(int direction);
    arcade_board_type active_rom_board() const;
    void select_rom_within_board(int direction);
    void select_rom_board(arcade_board_type board);
    void cycle_rom_board(int direction);
    void update_settings_texture();
    void update_fps_texture(double fps);
    void ensure_player_labels();
    void draw_settings_overlay();
    void draw_fps_overlay();
    void update_lightgun_cursor();
    void present_texture(uint32_t texture, int source_width,
                         int source_height, bool apply_board_color,
                         bool flip_y, int display_width = 0,
                         int display_height = 0);
    uint32_t m_last_present_texture{0};
    int m_picture_rect_x{0};
    int m_picture_rect_y{0};
    int m_picture_rect_width{0};
    int m_picture_rect_height{0};
    int m_picture_drawable_width{0};
    int m_picture_drawable_height{0};
    int m_last_present_width{0};
    int m_last_present_height{0};
    int m_last_present_display_width{0};
    int m_last_present_display_height{0};
    bool m_last_present_board_color{false};
    bool m_last_present_flip_y{false};

public:
    polygon_renderer_gpu();
    ~polygon_renderer_gpu();

    bool initialize(const emulator_settings& settings);
    void shutdown();

    // Pump window events. Esc leaves the cabinet; closing the window exits.
    arcade_host_action process_events();
    void reset_session_ui();
    void set_single_screen_only(bool enabled);
    void set_cabinet_status(std::string status);
    // Names the running board so its cabinet bezel can be
    // fetched. Safe to call on every launch; a name already
    // looked up costs nothing.
    void set_board_name(std::string short_name);
    void set_rom_choices(std::vector<rom_choice> choices);
    void set_operator_menu(operator_menu_definition menu);
    bool take_rom_selection(std::string& path);
    bool take_operator_action(operator_menu_action& action);
    bool take_controls_request();
    bool take_game_picker_request();
    bool take_settings_change(emulator_settings& settings);
    void set_lightgun_cursor(bool enabled, uint8_t player = 0);

    // Where the emulated picture was last drawn, in drawable pixels with the
    // origin at the top-left, plus the drawable's own size. A light gun maps
    // the pointer onto this rectangle; deriving it from the window size
    // instead misses letterboxing, integer scaling and the twin-screen
    // margins, and aims the gun at the wrong place. Returns false until a
    // frame has been presented.
    bool picture_rect(int& x, int& y, int& width, int& height,
                      int& drawable_width, int& drawable_height) const {
        if (m_picture_rect_width <= 0 || m_picture_rect_height <= 0)
            return false;
        x = m_picture_rect_x;
        y = m_picture_rect_y;
        width = m_picture_rect_width;
        height = m_picture_rect_height;
        drawable_width = m_picture_drawable_width;
        drawable_height = m_picture_drawable_height;
        return true;
    }
    bool lightgun_cursor_enabled() const {
        return m_lightgun_cursor_enabled;
    }
    uint8_t lightgun_cursor_player() const {
        return m_lightgun_cursor_player;
    }
    bool settings_visible() const { return m_settings_visible; }
    bool paused() const { return m_paused; }
    void refresh_output();
    bool tick_status_overlay();
    void poll_bezel();
    void recover_from_device_loss();
    void maintain_gl_wall_placement();
    // Whether the OpenGL textures still have to be kept warm.
    // False once the native path owns the frame, which lets the
    // per-frame uploads skip their OpenGL half. Only data the
    // board resubmits every frame may use this: a decline then
    // repopulates within one frame, whereas one-shot ROM data
    // would have no second chance.
    bool opengl_textures_needed() const {
        return !m_native_frame_live;
    }
    // Consumes a pending arcade-wall focus change, reporting whether this
    // column should currently be audible. Wall columns are separate windows,
    // so SDL's focus events are all the coordination they need.
    bool take_wall_audio_change(bool& audible) {
        if (!m_wall_audio_changed) return false;
        m_wall_audio_changed = false;
        audible = m_wall_column_active;
        return true;
    }
    bool present_native_scene(bool rasterised, const char* board,
                              int width, int height);
    board_overlays collect_board_overlays();
    system22_uniform_block build_system22_uniforms(
        const view_matrix& view) const;
    system22_text_uniform_block build_system22_text_uniforms() const;
    void apply_display_settings(const emulator_settings& settings);

    // Add polygons to render queue (CPU side)
    void submit_polygons(const polygon_object* polys, int count);

    // Dispatch rendering to GPU
    void render_scene(const view_matrix& view, const rgba_color& fog_color);

    // Present a complete board-native RGBA8 frame through the same scaling,
    // filtering, settings overlay and host-window path as System 22 output.
    void present_rgba_frame(const uint8_t* pixels, int width, int height,
                            int display_width = 0,
                            int display_height = 0);
    void present_model2_frame(model2_gpu_frame frame);

    // Get rendered frame
    void read_framebuffer(uint32_t* output);

    // Submit texture data to GPU
    void submit_textures(const uint8_t* texture_rom, size_t texture_size,
                         const uint8_t* tilemap_rom, size_t tilemap_size,
                         size_t region_offset = 0x800000,
                         size_t bank_count = 4,
                         bool tile_high_bit_from_attr = true);
    void submit_sprites(const uint8_t* sprite_rom, size_t sprite_size);
    void set_system22_layer_mask(uint8_t mask) {
        m_system22_layer_mask = mask;
    }
    void submit_gamma(const uint8_t* gamma_proms, size_t size);
    void submit_palette(const uint8_t* palette_ram, size_t size);
    void submit_text_layer(const uint8_t* character_ram, size_t character_size,
                           const uint8_t* text_ram, size_t text_size,
                           const uint8_t* text_attributes,
                           const uint8_t* mixer, size_t mixer_size,
                           bool super_system22);
};

// OpenGL context wrapper
struct opengl_context {
    void* window{nullptr};       // Platform window handle
    void* window2{nullptr};      // Optional 2nd window for dual-player output
    void* gl_context{nullptr};   // GL context
    int width{0};
    int height{0};
    int twin_layout_attempts{0};

    bool create(void* window, int w, int h);
    void destroy();
    bool make_current();
};
