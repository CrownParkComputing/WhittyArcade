// arcade_renderer.h - shared host renderer and output UI
// Uses OpenGL shaders to rasterize renderer-neutral System 22 polygon records.
#pragma once

#include "system22_types.h"
#include "arcade_settings.h"
#include "model2_gpu_frame.h"
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <utility>
#include <chrono>

// Forward declarations
struct opengl_context;
struct TTF_Font;
class alternate_presenter;

// GPU polygon renderer - processes polygons on GPU instead of CPU
class polygon_renderer_gpu {
private:
    // OpenGL/Vulkan context
    std::unique_ptr<opengl_context> m_ctx;
    std::unique_ptr<alternate_presenter> m_alternate_presenter;
    std::vector<uint8_t> m_present_readback;

    // GPU buffers
    uint32_t m_vertex_buffer{0};    // VBO for vertices
    uint32_t m_index_buffer{0};    // IBO for indices
    uint32_t m_texture_buffer{0};  // Texture data
    uint32_t m_tilemap_texture{0};
    uint32_t m_tileattr_texture{0};
    uint32_t m_palette_texture{0};
    uint32_t m_character_texture{0};
    uint32_t m_textmap_texture{0};
    uint32_t m_gamma_texture{0};
    uint32_t m_output_texture{0};  // RGBA8 scene/priority framebuffer texture
    uint32_t m_external_frame_texture{0}; // Complete RGBA frame from another board
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
    TTF_Font* m_settings_font{nullptr};
    int m_fps_texture_width{0};
    int m_fps_texture_height{0};
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

    // Producer/consumer boundary: CPU/DSP threads only copy neutral polygon
    // records here. All OpenGL calls stay on the render/context thread.
    std::mutex m_queue_mutex;
    std::vector<polygon_object> m_queued_polygons;

    // Thread synchronization
    std::atomic<int> m_pending_polygons{0};
    bool m_headless{false};
    bool m_initialized{false};
    bool m_dip_requested{false};
    bool m_f2_opens_dip{false};
    bool m_settings_visible{false};
    bool m_settings_changed{false};
    bool m_paused{false};
    bool m_settings_texture_dirty{true};
    bool m_ttf_initialized{false};
    int m_settings_selection{0};
    bool m_rom_menu_visible{false};
    int m_rom_selection{0};
    std::vector<rom_choice> m_rom_choices;
    std::string m_selected_rom;
    bool m_rom_selection_pending{false};
    emulator_settings m_display_settings{};
    renderer_backend m_active_backend{renderer_backend::opengl};

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
    arcade_board_type active_rom_board() const;
    void select_rom_within_board(int direction);
    void select_rom_board(arcade_board_type board);
    void cycle_rom_board(int direction);
    void update_settings_texture();
    void update_fps_texture(double fps);
    void draw_settings_overlay();
    void draw_fps_overlay();
    void present_texture(uint32_t texture, int source_width,
                         int source_height, bool apply_board_color,
                         bool flip_y);
    uint32_t m_last_present_texture{0};
    int m_last_present_width{0};
    int m_last_present_height{0};
    bool m_last_present_board_color{false};
    bool m_last_present_flip_y{false};

public:
    polygon_renderer_gpu();
    ~polygon_renderer_gpu();

    bool initialize(const emulator_settings& settings);
    void shutdown();

    // Pump window events. Esc leaves the cabinet; closing the window exits.
    arcade_host_action process_events();
    void set_rom_choices(std::vector<rom_choice> choices);
    bool take_rom_selection(std::string& path);
    bool take_dip_request();
    bool take_settings_change(emulator_settings& settings);
    void set_f2_opens_dip(bool enabled) { m_f2_opens_dip = enabled; }
    bool settings_visible() const { return m_settings_visible; }
    bool paused() const { return m_paused; }
    void refresh_output();
    void apply_display_settings(const emulator_settings& settings);

    // Add polygons to render queue (CPU side)
    void submit_polygons(const polygon_object* polys, int count);

    // Dispatch rendering to GPU
    void render_scene(const view_matrix& view, const rgba_color& fog_color);

    // Present a complete board-native RGBA8 frame through the same scaling,
    // filtering, settings overlay and host-window path as System 22 output.
    void present_rgba_frame(const uint8_t* pixels, int width, int height);
    void present_model2_frame(model2_gpu_frame frame);

    // Get rendered frame
    void read_framebuffer(uint32_t* output);

    // Submit texture data to GPU
    void submit_textures(const uint8_t* texture_rom, size_t texture_size,
                         const uint8_t* tilemap_rom, size_t tilemap_size,
                         size_t region_offset = 0x800000,
                         size_t bank_count = 4,
                         bool tile_high_bit_from_attr = true);
    void submit_gamma(const uint8_t* gamma_proms, size_t size);
    void submit_palette(const uint8_t* palette_ram, size_t size);
    void submit_text_layer(const uint8_t* character_ram, size_t character_size,
                           const uint8_t* text_ram, size_t text_size,
                           const uint8_t* text_attributes,
                           const uint8_t* mixer, size_t mixer_size);
};

// OpenGL context wrapper
struct opengl_context {
    void* window{nullptr};       // Platform window handle
    void* gl_context{nullptr};   // GL context
    int width{0};
    int height{0};

    bool create(void* window, int w, int h);
    void destroy();
    void make_current();
};
