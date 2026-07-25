// arcade_presenter.h - Vulkan and software host presentation backends
#pragma once

#include "arcade_settings.h"
#include "system22_vulkan_uniforms.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// RGBA panels composited over a presented board frame. The presenter resolves
// each one's position because it owns the pane split; the caller only supplies
// the bitmap. A null pointer leaves whatever that layer last showed in place,
// which is what lets a paused board redraw without rebuilding its panels.
struct board_overlays {
    // Frame rate and cabinet status, pinned to the top-left of the surface.
    const uint8_t* status{};
    int status_width{};
    int status_height{};
    // Settings or ROM menu, centred against the top edge and scaled to fit.
    const uint8_t* menu{};
    int menu_width{};
    int menu_height{};
    // Player 1 and Player 2 cabinet labels, one per output pane.
    const uint8_t* player_label[2]{};
    int player_label_width[2]{};
    int player_label_height[2]{};
};

// The System 22 source sheets, in the order the shaders sample them. Their
// contents and geometry are the same bytes the OpenGL path uploads, so a board
// hands over what it already builds.
enum class scene_sheet {
    texture_tiles,
    texture_map,
    texture_attr,
    palette,
    sprite_tiles,
    // The character/tilemap layer's own sources. These are distinct from the
    // polygon texture sheets above: character_data is the 512x256 character
    // RAM and text_map the 64x64 tilemap, both plain 2D images, where
    // texture_tiles is a layered array. Binding one where the other belongs
    // is an image-view-type mismatch the shader cannot survive.
    character_data,
    text_map,
    // Model 2. base and foreground are the 2D layers composited under and
    // over the polygons; sheets/luma/colour drive the polygon shader. The
    // colour table is GL_RGB8 on the OpenGL side, but three-byte formats are
    // widely unsupported for Vulkan sampling, so it is expanded to RGBA on
    // upload.
    model2_base,
    model2_foreground,
    model2_sheets,
    model2_luma,
    model2_color,
};

// One frame of Model 2 geometry. Its vertices are model2_draw_vertex records
// as arcade_renderer.cpp builds them.
struct model2_scene {
    int width{};
    int height{};
    const void* vertices{};
    std::size_t vertex_count{};
    // The CRTC offset the vertex stage applies, in board pixels.
    float crtc_offset[2]{};
};

// One frame of System 22 geometry. The vertex array is draw_vertex records
// exactly as arcade_renderer.cpp builds them - the Vulkan pipeline declares
// its attributes from the same offsets, so nothing is repacked.
struct system22_scene {
    int width{};
    int height{};
    const void* vertices{};
    std::size_t vertex_count{};
    system22_uniform_block uniforms{};
    // The character/tilemap layer drawn over the polygons.
    bool draw_text{false};
    system22_text_uniform_block text_uniforms{};
    // Board colour for the presentation pass: the screen fade and the three
    // gamma PROMs the original hardware applies on the way to the monitor.
    // The OpenGL route turns this on for System 22, so the Vulkan one must
    // too or the picture comes out ungraded.
    bool apply_board_color{false};
    bool super_system22{false};
    uint32_t screen_fade[3]{};
};

// A cabinet bezel and the opening the game belongs in, in the bezel's own
// pixels. The presenter scales the artwork to the pane and maps the opening
// through the same transform, so the game lands exactly in the cut-out.
struct bezel_frame {
    const uint8_t* pixels{};
    int width{};
    int height{};
    int cutout_x{};
    int cutout_y{};
    int cutout_width{};
    int cutout_height{};
};

// Visible host-window output used when the board renderer is not presenting
// directly through its OpenGL context. Vulkan is a native swapchain transfer
// path; software uses SDL's CPU renderer. Both consume a finished RGBA frame.
class alternate_presenter {
public:
    alternate_presenter();
    ~alternate_presenter();

    alternate_presenter(const alternate_presenter&) = delete;
    alternate_presenter& operator=(const alternate_presenter&) = delete;

    bool initialize(renderer_backend backend, int width, int height,
                    const emulator_settings& settings);
    void shutdown();
    void apply_settings(const emulator_settings& settings);
    void drawable_size(int& width, int& height) const;
    void* window() const;
    renderer_backend backend() const;

    // A board frame in its own native raster, drawn by the Vulkan graphics
    // pipeline: the pixels become a sampled image and the post-process shader
    // scales and letterboxes them on the GPU. No OpenGL context, no readback
    // and no CPU rescale are involved.
    //
    // Returns false when the native path is unavailable - the software
    // backend, a surface that cannot be a colour attachment, or a pipeline
    // that failed to build - and the caller must then present through OpenGL.
    // flip_y matches the OpenGL post shader's uniform of the same name: set it
    // when row zero of the source is the top of the picture.
    bool render_board_frame(const uint8_t* pixels, int width, int height,
                            bool flip_y, const board_overlays& overlays,
                            bool menu_visible = false, int display_width = 0,
                            int display_height = 0);

    // Redraws the frame already resident on the GPU, for a paused board or a
    // dismissed menu. False when there is nothing retained to redraw.
    bool repeat_board_frame();

    // True once any Vulkan call has returned VK_ERROR_DEVICE_LOST. The device
    // never comes back; the caller must stop presenting through Vulkan and
    // fall over to the OpenGL window instead.
    bool device_lost() const;

    // Copies the rasterised scene image back to the host, for offscreen
    // verification. Diagnostic plumbing: a shipped frame never reads back.
    bool read_scene_image(std::vector<uint8_t>& rgba, int& width,
                          int& height);

    // Installs the cabinet bezel drawn around the game, or clears it when
    // pixels is null. The artwork changes only when the board does, so this
    // uploads once rather than riding a frame.
    bool set_bezel(const bezel_frame& bezel);

    // Replaces one System 22 source sheet. Sheets change rarely - a bank
    // switch, a palette write - so this stages and submits on its own rather
    // than riding a frame.
    bool upload_scene_sheet(scene_sheet sheet, const void* pixels,
                            std::size_t bytes);

    // The board's three 256-entry gamma PROMs, applied by the presentation
    // pass. Without them the grade falls back to the identity ramp seeded at
    // start-up and the picture is close but not right.
    bool upload_gamma_ramp(const void* pixels, std::size_t bytes);

    // Rasterises a System 22 frame into the scene image. The draw is recorded
    // ahead of the next presentation rather than submitted here, so a frame
    // still costs one submit; follow it with render_board_frame(nullptr, ...)
    // to put it on screen. False when the native path is unavailable, which
    // means the caller should fall back to OpenGL.
    bool render_system22_scene(const system22_scene& scene);

    // Rasterises a Model 2 frame - base layer, polygons, foreground layer -
    // into the scene image, on the same terms as render_system22_scene.
    bool render_model2_scene(const model2_scene& scene);

    // Model 2's colour table arrives as packed three-byte RGB, which Vulkan
    // cannot sample in optimal tiling on common hardware. This widens it to
    // RGBA on the way in.
    bool upload_model2_color_table(const void* rgb, std::size_t bytes);

    // OpenGL readback begins at the lower-left. The presenter flips it while
    // copying into its top-left host output.
    bool present_rgba_bottom_up(const uint8_t* pixels, int width, int height,
                                const uint8_t* overlay_pixels = nullptr,
                                int overlay_width = 0,
                                int overlay_height = 0,
                                bool menu_visible = false,
                                int display_width = 0,
                                int display_height = 0);

private:
    struct implementation;
    std::unique_ptr<implementation> m_impl;
};
