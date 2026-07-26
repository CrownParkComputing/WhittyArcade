#include "arcade_presenter.h"
#include "title_capture.h"
#include "wall_layout.h"
#include "wall_log.h"

#include "arcade_config.h"
#include "system22_vulkan_uniforms.h"
#include "twin_window_layout.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <cstdint>

// SPIR-V for the native render path, compiled from shaders/*.vert|frag by the
// whitty_shaders build target.
#include "arcade_post_vert.h"
#include "arcade_post_frag.h"
#include "arcade_overlay_vert.h"
#include "arcade_overlay_frag.h"
#include "fullscreen_quad_vert.h"
#include "arcade_solid_frag.h"
#include "system22_polygon_vert.h"
#include "system22_polygon_frag.h"
#include "system22_text_frag.h"
#include "model2_polygon_vert.h"
#include "model2_polygon_frag.h"
#include "model2_overlay_frag.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace {
struct fitted_viewport {
    int x{};
    int y{};
    int w{};
    int h{};
};

fitted_viewport fit_arcade_viewport(
        int pane_x, int pane_width, int surface_height,
        int aspect_width, int aspect_height,
        int source_width, int source_height,
        bool integer_scaling, bool fullscreen, bool framed) {
    const int frame_margin = framed ?
        std::clamp(std::min(pane_width, surface_height) / 32, 12, 30) : 0;
    const int available_width = std::max(pane_width - frame_margin * 2, 1);
    const int available_height =
        std::max(surface_height - frame_margin * 2, 1);
    int width = available_width;
    int height = available_height;
    // Twin Screen is a pixel-accurate cabinet view. Preserve the source
    // raster (for example Galaga's 224x288) with one uniform integer scale on
    // both axes, including fullscreen. The border absorbs all unused space.
    if (framed) {
        const int scale = std::min(available_width / source_width,
                                   available_height / source_height);
        if (scale >= 1) {
            width = source_width * scale;
            height = source_height * scale;
        } else if (width * source_height > height * source_width) {
            width = height * source_width / source_height;
        } else {
            height = width * source_height / source_width;
        }
    } else if (integer_scaling && !fullscreen) {
        const int scale = std::min(available_width / source_width,
                                   available_height / source_height);
        if (scale >= 1) {
            width = source_width * scale;
            height = source_height * scale;
        }
        if (width * aspect_height > height * aspect_width)
            width = height * aspect_width / aspect_height;
        else
            height = width * aspect_height / aspect_width;
    } else {
        if (width * aspect_height > height * aspect_width)
            width = height * aspect_width / aspect_height;
        else
            height = width * aspect_height / aspect_width;
    }
    return {
        pane_x + (pane_width - width) / 2,
        (surface_height - height) / 2,
        std::max(width, 1), std::max(height, 1),
    };
}

void fill_host_rect(uint8_t* pixels, int surface_width, int surface_height,
                    std::size_t pitch, fitted_viewport rect, bool bgra,
                    uint8_t red, uint8_t green, uint8_t blue) {
    rect.x = std::clamp(rect.x, 0, surface_width);
    rect.y = std::clamp(rect.y, 0, surface_height);
    rect.w = std::clamp(rect.w, 0, surface_width - rect.x);
    rect.h = std::clamp(rect.h, 0, surface_height - rect.y);
    for (int y = 0; y < rect.h; ++y) {
        uint8_t* row = pixels +
            static_cast<std::size_t>(rect.y + y) * pitch +
            static_cast<std::size_t>(rect.x) * 4;
        for (int x = 0; x < rect.w; ++x) {
            row[x * 4 + 0] = bgra ? blue : red;
            row[x * 4 + 1] = green;
            row[x * 4 + 2] = bgra ? red : blue;
            row[x * 4 + 3] = 255;
        }
    }
}

fitted_viewport expanded(fitted_viewport rect, int amount) {
    return {rect.x - amount, rect.y - amount,
            rect.w + amount * 2, rect.h + amount * 2};
}

std::array<int, 2> bounded_window_size(SDL_Window* window,
                                       int requested_width) {
    SDL_Rect usable{0, 0, 1920, 1080};
    SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) :
                                     SDL_GetPrimaryDisplay();
    if (!display || !SDL_GetDisplayUsableBounds(display, &usable)) {
        display = SDL_GetPrimaryDisplay();
        if (display) SDL_GetDisplayBounds(display, &usable);
    }
    int top = 0, left = 0, bottom = 0, right = 0;
    SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right);
    const int maximum_width = std::max(320, std::min(
        usable.w - left - right, (usable.h - top - bottom) * 4 / 3));
    const int width = std::clamp(requested_width, 320, maximum_width);
    return {width, width * 3 / 4};
}

void set_fixed_window_size(SDL_Window* window, int requested_width) {
    const auto size = bounded_window_size(window, requested_width);
    const int width = size[0];
    const int height = size[1];
    SDL_SetWindowMinimumSize(window, width, height);
    SDL_SetWindowMaximumSize(window, width, height);
    SDL_SetWindowSize(window, width, height);
    SDL_RaiseWindow(window);
}

bool place_window_on_display(SDL_Window* window, int display_index,
                             bool center) {
    if (!window || display_index < 0) return false;
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    if (!displays || display_index >= count) {
        if (displays) SDL_free(displays);
        return false;
    }
    SDL_Rect bounds{};
    const bool found = SDL_GetDisplayBounds(displays[display_index], &bounds);
    SDL_free(displays);
    if (!found) return false;
    int x = bounds.x;
    int y = bounds.y;
    if (center) {
        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window, &width, &height);
        x += std::max((bounds.w - width) / 2, 0);
        y += std::max((bounds.h - height) / 2, 0);
    }
    return SDL_SetWindowPosition(window, x, y);
}

template <typename T>
T vk_structure(VkStructureType type) {
    T value{};
    value.sType = type;
    return value;
}

// One presenter per process, so a process-wide flag is exact: once the
// device is lost every queue in this process is dead and the renderer must
// stop presenting through Vulkan entirely.
std::atomic<bool> g_device_lost{false};

bool vk_ok(VkResult result, const char* operation) {
    if (result == VK_SUCCESS) return true;
    if (result == VK_ERROR_DEVICE_LOST) g_device_lost.store(true);
    std::fprintf(stderr, "%s failed (Vulkan result %d)\n", operation,
                 static_cast<int>(result));
    // Also into the wall column's own log: a spawned column's stderr lands on
    // the parent's terminal, where a failure is invisible.
    whitty_wall_log::note("%s failed (Vulkan result %d)", operation,
                          static_cast<int>(result));
    return false;
}

// Composite a top-left, straight-alpha RGBA status panel after the arcade
// image has been scaled to the host drawable. The Vulkan swapchain can be
// RGBA or BGRA, so destination channel order is selected explicitly.
void composite_status_overlay(uint8_t* destination, int destination_width,
                              int destination_height,
                              std::size_t destination_pitch, bool bgra,
                              const uint8_t* overlay, int overlay_width,
                              int overlay_height) {
    if (!destination || !overlay || overlay_width <= 0 ||
        overlay_height <= 0)
        return;
    constexpr int margin = 12;
    const int copy_width = std::min(
        overlay_width, std::max(destination_width - margin, 0));
    const int copy_height = std::min(
        overlay_height, std::max(destination_height - margin, 0));
    for (int y = 0; y < copy_height; ++y) {
        uint8_t* output = destination +
            static_cast<std::size_t>(y + margin) * destination_pitch +
            static_cast<std::size_t>(margin) * 4;
        const uint8_t* input = overlay +
            static_cast<std::size_t>(y) * overlay_width * 4;
        for (int x = 0; x < copy_width; ++x) {
            const uint8_t alpha = input[x * 4 + 3];
            if (alpha == 0) continue;
            const uint8_t red = input[x * 4 + 0];
            const uint8_t green = input[x * 4 + 1];
            const uint8_t blue = input[x * 4 + 2];
            const uint8_t source[3]{bgra ? blue : red, green,
                                    bgra ? red : blue};
            for (int channel = 0; channel < 3; ++channel) {
                output[x * 4 + channel] = static_cast<uint8_t>(
                    (static_cast<unsigned>(source[channel]) * alpha +
                     static_cast<unsigned>(output[x * 4 + channel]) *
                         (255u - alpha) +
                     127u) /
                    255u);
            }
            output[x * 4 + 3] = 255;
        }
    }
}

VkSurfaceFormatKHR select_surface_format(
    const std::vector<VkSurfaceFormatKHR>& formats) {
    // An UNORM surface, specifically. The boards produce pixels that are
    // already display-ready - the System 22 gamma PROMs are part of the
    // emulated hardware - so nothing here should be re-encoded on the way
    // out. That used to be moot because the frame was written with a
    // transfer copy, which performs no conversion whatever the format is;
    // presenting through a render pass does convert, and an _SRGB attachment
    // would silently lift every value and wash the picture out.
    for (const VkSurfaceFormatKHR& format : formats) {
        if ((format.format == VK_FORMAT_B8G8R8A8_UNORM ||
             format.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return format;
    }
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM ||
            format.format == VK_FORMAT_R8G8B8A8_UNORM)
            return format;
    }
    std::fprintf(stderr,
                 "No UNORM swapchain format available; the picture may look "
                 "brighter than the board intended\n");
    return formats.front();
}

VkCompositeAlphaFlagBitsKHR select_composite_alpha(VkCompositeAlphaFlagsKHR flags) {
    constexpr VkCompositeAlphaFlagBitsKHR choices[]{
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };
    for (VkCompositeAlphaFlagBitsKHR choice : choices)
        if (flags & choice) return choice;
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}
// Horizontal extent of one output pane. Fullscreen dual output splits the
// surface into two; everything else is a single full-width pane.
// Mirrors the push_constant block in shaders/arcade_post.frag. GLSL packs a
// uvec3 on a 16-byte boundary, so the first scalar after it lands in the
// vacant fourth slot rather than starting a new one.
struct post_push_constants {
    uint32_t screen_fade[3]{};
    uint32_t super_system22{};
    uint32_t apply_board_color{};
    uint32_t flip_y{};
    uint32_t linear_filtering{};
    // Pane extent in output pixels; the sharp-bilinear ramp needs to know how
    // many output pixels one source texel covers.
    float pane_width{};
    float pane_height{};
};

// Offsets taken from the compiled module's OpMemberDecorate list. A member
// added or reordered in the shader without matching this struct would
// otherwise scramble every value silently at runtime.
static_assert(offsetof(post_push_constants, screen_fade) == 0);
static_assert(offsetof(post_push_constants, super_system22) == 12);
static_assert(offsetof(post_push_constants, apply_board_color) == 16);
static_assert(offsetof(post_push_constants, flip_y) == 20);
static_assert(offsetof(post_push_constants, linear_filtering) == 24);
static_assert(offsetof(post_push_constants, pane_width) == 28);
static_assert(offsetof(post_push_constants, pane_height) == 32);
static_assert(sizeof(post_push_constants) == 36);

// A sampled image plus everything needed to keep it alive and rebuild it when
// the board changes raster size.
struct device_image {
    VkImage image{};
    VkDeviceMemory memory{};
    VkImageView view{};
    int width{};
    int height{};
    int layers{};
    VkFormat format{VK_FORMAT_UNDEFINED};
};

} // namespace

struct alternate_presenter::implementation {
    renderer_backend backend{renderer_backend::software};
    emulator_settings settings{};
    SDL_Window* window{};
    SDL_Renderer* software_renderer{};
    SDL_Texture* software_texture{};
    SDL_Texture* software_overlay_texture{};
    SDL_Window* mirror_window{};
    SDL_Renderer* mirror_renderer{};
    SDL_Texture* mirror_texture{};
    int mirror_texture_width{};
    int mirror_texture_height{};
    int mirror_layout_attempts{};
    std::vector<uint8_t> mirror_pixels;
    // Per-pane nearest-neighbour source columns, kept across frames so the
    // present path never allocates.
    std::vector<int> column_offsets;
    int texture_width{};
    int texture_height{};
    int overlay_width{};
    int overlay_height{};
    std::vector<uint8_t> flipped_pixels;

    VkInstance instance{};
    VkDebugUtilsMessengerEXT debug_messenger{};
    VkSurfaceKHR surface{};
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    uint32_t queue_family{std::numeric_limits<uint32_t>::max()};
    VkQueue queue{};
    VkSwapchainKHR swapchain{};
    VkFormat swapchain_format{VK_FORMAT_UNDEFINED};
    VkExtent2D swapchain_extent{};
    std::vector<VkImage> swapchain_images;
    VkCommandPool command_pool{};
    // One-shot upload bookkeeping: the fence this submission signals, and the
    // command buffer that must outlive it.
    VkFence upload_fence{};
    VkCommandBuffer upload_commands{};
    bool upload_in_flight{};
    VkCommandBuffer command_buffer{};
    VkSemaphore image_available{};
    // One per swapchain image, not one overall. A present waits on this
    // semaphore, and presentation completion is not covered by the frame
    // fence, so a single shared semaphore can be signalled again while an
    // earlier present is still using it - which the validation layers
    // reject as VUID-vkQueueSubmit-pSignalSemaphores-00067.
    std::vector<VkSemaphore> render_finished;
    VkFence frame_fence{};
    VkBuffer staging_buffer{};
    VkDeviceMemory staging_memory{};
    VkDeviceSize staging_size{};
    void* staging_mapped{};

    // --- Native graphics path -------------------------------------------
    // Board pixels go straight into a sampled image and are drawn by the
    // post-process pipeline, so no OpenGL context, readback or CPU rescale is
    // involved. Every handle below is optional: if any of it fails to build,
    // native_ready stays false and presentation falls back to the transfer
    // blit above.
    // Wall placement is only real once the compositor has mapped the
    // window, which has not happened when settings are first applied.
    // Retried from the frame path until it takes.
    bool wall_placed{};
    int wall_place_countdown{};
    // The focused column this window was last laid out for.
    int wall_focus_applied{-2};
    // Where this column was last successfully placed, so
    // drift caused by the compositor can be noticed.
    int placed_x{}, placed_y{}, placed_width{}, placed_height{};
    bool swapchain_attachable{};
    // Whether the device supports the second blend source the System 22
    // polygon pipeline needs.
    bool dual_source_blend{};
    bool native_ready{};
    bool native_failed{};
    VkRenderPass render_pass{};
    std::vector<VkImageView> swapchain_views;
    std::vector<VkFramebuffer> framebuffers;
    VkDescriptorSetLayout post_set_layout{};
    VkDescriptorSetLayout overlay_set_layout{};
    VkPipelineLayout post_pipeline_layout{};
    VkPipelineLayout overlay_pipeline_layout{};
    VkPipelineLayout solid_pipeline_layout{};
    VkPipeline post_pipeline{};
    VkPipeline overlay_pipeline{};
    VkPipeline solid_pipeline{};
    VkDescriptorPool descriptor_pool{};
    // One per pane: an arcade wall draws a different game in each, so
    // each needs its own sampled image and its own descriptor set.
    // Two panes at most: fullscreen dual output, one player each. An arcade
    // wall is separate processes with a window apiece, not panes of one
    // surface, because three boards cannot share one renderer's sheets and
    // vertex buffers.
    static constexpr int max_panes = 2;
    VkDescriptorSet post_set{};
    VkSampler nearest_sampler{};
    VkSampler linear_sampler{};
    // One board's picture, sampled by every pane: a twin cabinet shows the
    // same frame to both players.
    device_image scene_image;
    // One-shot launcher-icon capture (see arm_title_capture).
    std::string title_capture_name;
    int title_capture_countdown{-1};
    bool title_capture_due{false};
    // Shared-screen cabinet cell placement, maintained like a wall column.
    bool cabinet_placed{};
    int cabinet_place_countdown{};
    device_image scene_depth;
    VkFramebuffer scene_framebuffer{};
    // Depth for the polygon boards' offscreen pass. The RGBA path never
    // touches it, but the scene image is shared so both produce the same
    // input for the post pass.
    // Cabinet bezel geometry per column, in the artwork's own pixels. A
    // width of 0 means that column has no bezel, in which case it letterboxes
    // exactly as it did before bezels existed. One per column because a wall
    // frames each game in its own cabinet.
    struct bezel_geometry {
        int width{};
        int height{};
        fitted_viewport cutout{};
    };
    bezel_geometry bezel;

    VkRenderPass scene_render_pass{};
    VkFormat scene_depth_format{VK_FORMAT_UNDEFINED};

    // System 22 source data. The sheets keep the same geometry and element
    // types as the OpenGL textures in arcade_renderer.cpp so the renderer can
    // hand over the bytes it already builds, unmodified.
    enum scene_sheet_index {
        sheet_texture_tiles, sheet_texture_map, sheet_texture_attr,
        sheet_palette, sheet_sprite_tiles, sheet_character_data,
        sheet_text_map, sheet_model2_base, sheet_model2_foreground,
        sheet_model2_sheets, sheet_model2_luma, sheet_model2_color,
        scene_sheet_count
    };

    std::array<device_image, scene_sheet_count> scene_sheets;
    VkDescriptorSetLayout polygon_set_layout{};
    VkDescriptorSetLayout text_set_layout{};
    VkPipelineLayout polygon_pipeline_layout{};
    VkPipelineLayout text_pipeline_layout{};
    VkPipeline polygon_pipeline{};
    VkPipeline text_pipeline{};
    VkDescriptorPool scene_descriptor_pool{};
    VkDescriptorSet polygon_set{};
    VkDescriptorSet text_set{};
    VkBuffer polygon_uniform_buffer{};
    VkDeviceMemory polygon_uniform_memory{};
    void* polygon_uniform_mapped{};
    VkBuffer text_uniform_buffer{};
    VkDeviceMemory text_uniform_memory{};
    void* text_uniform_mapped{};
    VkBuffer vertex_buffer{};
    VkDeviceMemory vertex_memory{};
    void* vertex_mapped{};
    VkDeviceSize vertex_capacity{};
    bool scene_pipelines_ready{};
    bool scene_descriptors_valid{};
    // Model 2. Two overlay sets rather than one because the base and
    // foreground layers are different images drawn in the same pass.
    VkDescriptorSetLayout model2_set_layout{};
    VkDescriptorSetLayout model2_layer_set_layout{};
    VkPipelineLayout model2_pipeline_layout{};
    VkPipelineLayout model2_layer_pipeline_layout{};
    VkPipeline model2_pipeline{};
    VkPipeline model2_base_pipeline{};
    VkPipeline model2_foreground_pipeline{};
    VkDescriptorPool model2_descriptor_pool{};
    VkDescriptorSet model2_set{};
    VkDescriptorSet model2_base_set{};
    VkDescriptorSet model2_foreground_set{};
    bool model2_pipelines_ready{};
    bool model2_descriptors_valid{};
    std::size_t model2_vertex_count{};
    bool model2_pending{};
    float model2_crtc_offset[2]{};
    // Geometry staged by render_system22_scene, drawn by the next
    // presentation so the pair costs a single submit.
    std::size_t pending_vertex_count{};
    bool pending_draw_text{};
    bool pending_scene{};
    // Staging for the sheets the board rewrites while it runs.
    struct sheet_stream {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        void* mapped{};
        bool pending{};
    };
    std::array<sheet_stream, scene_sheet_count> sheet_streaming;

    // Board colour retained from the scene so the presentation pass grades
    // the picture the same way the OpenGL route does.
    bool board_apply_color{};
    bool board_super_system22{};
    uint32_t board_screen_fade[3]{};
    device_image gamma_image;
    // Last ramp uploaded. System 22 resubmits gamma every frame whether
    // or not it changed, and the upload ends in a queue wait, so an
    // unchanged ramp must not reach the GPU at all.
    std::vector<uint8_t> gamma_resident;
    // One slot per composited panel: status, menu, and a cabinet label for
    // each output pane. Each owns its image and descriptor set so a frame can
    // draw all of them without rebinding a shared one mid-pass.
    enum overlay_index { overlay_status, overlay_menu, overlay_label_one,
                         overlay_label_two, overlay_bezel, overlay_count };
    struct overlay_slot {
        device_image image;
        VkDescriptorSet set{};
        bool resident{};
    };
    std::array<overlay_slot, overlay_count> overlays;
    bool scene_uses_linear{};
    bool descriptors_valid{};
    VkBuffer upload_buffer{};
    VkDeviceMemory upload_memory{};
    void* upload_mapped{};
    VkDeviceSize upload_size{};
    // Retained so a paused board can be redrawn from the resident image.
    bool have_board_frame{};
    bool board_flip_y{};
    bool board_menu_visible{};
    int board_display_width{};
    int board_display_height{};
    bool swapchain_dirty{true};
    // Paces the swapchain-recovery attempts in create_native_path: a
    // surface that stays unusable is retried every 30 calls, not at 60Hz.
    int native_retry_countdown{};
    // Last reported wall geometry, so the report is one line per change.
    VkExtent2D reported_extent{};
    int reported_drawable[2]{};
    bool first_present{true};
    // Wall columns force their window visible after ~2s of present attempts
    // that never succeed: a black column can be seen and reported, an
    // invisible one cannot. Single-cabinet windows keep the polished
    // behaviour of appearing only with their first picture.
    int present_attempts{};

    void count_present_attempt() {
        if (!first_present || settings.wall_count <= 1) return;
        if (++present_attempts < 120) return;
        whitty_wall_log::note(
            "forcing window visible without a successful present");
        SDL_ShowWindow(window);
        first_present = false;
    }
    VkFormat previous_format{VK_FORMAT_UNDEFINED};

    void destroy_mirror() {
        if (mirror_texture) SDL_DestroyTexture(mirror_texture);
        if (mirror_renderer) SDL_DestroyRenderer(mirror_renderer);
        if (mirror_window) SDL_DestroyWindow(mirror_window);
        mirror_texture = nullptr;
        mirror_renderer = nullptr;
        mirror_window = nullptr;
        mirror_texture_width = mirror_texture_height = 0;
        mirror_layout_attempts = 0;
        mirror_pixels.clear();
        if (window) SDL_SetWindowTitle(window, "WhittyArcade");
    }

    bool ensure_mirror() {
        if (mirror_window && mirror_renderer) return true;
        mirror_window = SDL_CreateWindow(
            "WhittyArcade - Player 2", settings.window_width,
            settings.window_width * 3 / 4,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!mirror_window) return false;
        mirror_renderer = SDL_CreateRenderer(mirror_window, nullptr);
        if (!mirror_renderer)
            mirror_renderer = SDL_CreateRenderer(mirror_window, "software");
        if (!mirror_renderer) {
            destroy_mirror();
            return false;
        }
        if (settings.twin_separate_monitors) {
            // Player 2's screen: fullscreen on the second display rather than
            // a window sitting on someone's desktop.
            SDL_SetWindowMinimumSize(mirror_window, 1, 1);
            SDL_SetWindowMaximumSize(mirror_window, 16384, 16384);
            place_window_on_display(mirror_window, 1, false);
            SDL_SetWindowFullscreen(mirror_window, true);
            SDL_SetWindowTitle(window, "WhittyArcade - Player 1");
            return true;
        }
        set_fixed_window_size(mirror_window, settings.window_width);
        if (!settings.twin_separate_monitors) {
            SDL_SetWindowTitle(window, "WhittyArcade - Player 1");
            mirror_layout_attempts = 0;
        } else if (!place_window_on_display(mirror_window, 1, true)) {
            int x = SDL_WINDOWPOS_CENTERED;
            int y = SDL_WINDOWPOS_CENTERED;
            SDL_GetWindowPosition(window, &x, &y);
            SDL_SetWindowPosition(mirror_window, x + 48, y + 48);
        }
        if (settings.twin_separate_monitors)
            SDL_SetWindowTitle(window, "WhittyArcade - Player 1");
        return true;
    }

    // top_down describes the incoming rows: OpenGL readback arrives
    // bottom-up and needs the flip below, a native board frame does not.
    void present_mirror(const uint8_t* pixels, int width, int height,
                        int display_width, int display_height,
                        bool menu_visible, bool top_down = false) {
        // The mirror is wanted for any two-pane layout that is not a
        // single split surface, which now includes the fullscreen
        // per-display pair.
        const bool wanted = settings.output == output_mode::dual &&
            (!settings.fullscreen || settings.twin_separate_monitors) &&
            !menu_visible;
        if (!wanted) {
            if (mirror_window) destroy_mirror();
            return;
        }
        if (!ensure_mirror()) return;
        // Wayland maps a newly-created top-level asynchronously. Retry the
        // side-by-side compositor placement for the first few presented
        // frames so both title selectors exist before Hyprland receives it.
        if (!settings.twin_separate_monitors &&
            mirror_layout_attempts < 6) {
            SDL_PumpEvents();
            whitty_window::arrange_twin_windows(
                window, mirror_window, settings.window_width);
            ++mirror_layout_attempts;
        }
        if (!mirror_texture || mirror_texture_width != width ||
            mirror_texture_height != height) {
            if (mirror_texture) SDL_DestroyTexture(mirror_texture);
            mirror_texture = SDL_CreateTexture(
                mirror_renderer, SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_STREAMING, width, height);
            mirror_texture_width = width;
            mirror_texture_height = height;
        }
        if (!mirror_texture) return;
        mirror_pixels.resize(static_cast<std::size_t>(width) * height * 4);
        const std::size_t pitch = static_cast<std::size_t>(width) * 4;
        if (top_down) {
            std::memcpy(mirror_pixels.data(), pixels, pitch * height);
        } else {
            for (int y = 0; y < height; ++y)
                std::memcpy(mirror_pixels.data() + y * pitch,
                            pixels + (height - 1 - y) * pitch, pitch);
        }
        SDL_UpdateTexture(mirror_texture, nullptr, mirror_pixels.data(),
                          static_cast<int>(pitch));
        int output_width = 1;
        int output_height = 1;
        SDL_GetCurrentRenderOutputSize(mirror_renderer,
                                       &output_width, &output_height);
        const fitted_viewport fit = fit_arcade_viewport(
            0, std::max(output_width, 1), std::max(output_height, 1),
            display_width > 0 ? display_width : width,
            display_height > 0 ? display_height : height,
            width, height, settings.integer_scaling, false, true);
        SDL_SetRenderDrawColor(mirror_renderer, 3, 7, 11, 255);
        SDL_RenderClear(mirror_renderer);
        SDL_SetRenderDrawColor(mirror_renderer, 13, 29, 39, 255);
        SDL_FRect outer{static_cast<float>(fit.x - 10),
                        static_cast<float>(fit.y - 10),
                        static_cast<float>(fit.w + 20),
                        static_cast<float>(fit.h + 20)};
        SDL_RenderFillRect(mirror_renderer, &outer);
        SDL_SetRenderDrawColor(mirror_renderer, 56, 198, 255, 255);
        SDL_FRect accent{static_cast<float>(fit.x - 3),
                         static_cast<float>(fit.y - 3),
                         static_cast<float>(fit.w + 6),
                         static_cast<float>(fit.h + 6)};
        SDL_RenderFillRect(mirror_renderer, &accent);
        const SDL_FRect destination{
            static_cast<float>(fit.x), static_cast<float>(fit.y),
            static_cast<float>(fit.w), static_cast<float>(fit.h)};
        SDL_RenderTexture(mirror_renderer, mirror_texture, nullptr,
                          &destination);
        float old_scale_x = 1.0f;
        float old_scale_y = 1.0f;
        SDL_GetRenderScale(mirror_renderer, &old_scale_x, &old_scale_y);
        SDL_SetRenderScale(mirror_renderer, 2.0f, 2.0f);
        SDL_SetRenderDrawColor(mirror_renderer, 8, 13, 20, 220);
        const SDL_FRect badge{8.0f, 8.0f, 48.0f, 13.0f};
        SDL_RenderFillRect(mirror_renderer, &badge);
        SDL_SetRenderDrawColor(mirror_renderer, 255, 92, 140, 255);
        SDL_RenderDebugText(mirror_renderer, 12.0f, 10.0f, "PLAYER 2");
        SDL_SetRenderScale(mirror_renderer, old_scale_x, old_scale_y);
        SDL_RenderPresent(mirror_renderer);
    }

    uint32_t memory_type(uint32_t allowed, VkMemoryPropertyFlags properties) const {
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
        for (uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
            if ((allowed & (1u << index)) &&
                (memory.memoryTypes[index].propertyFlags & properties) == properties)
                return index;
        }
        return std::numeric_limits<uint32_t>::max();
    }

    void destroy_staging() {
        if (staging_memory && staging_mapped)
            vkUnmapMemory(device, staging_memory);
        staging_mapped = nullptr;
        if (staging_buffer) vkDestroyBuffer(device, staging_buffer, nullptr);
        if (staging_memory) vkFreeMemory(device, staging_memory, nullptr);
        staging_buffer = VK_NULL_HANDLE;
        staging_memory = VK_NULL_HANDLE;
        staging_size = 0;
    }

    bool create_staging(VkDeviceSize bytes) {
        if (staging_size >= bytes) return true;
        if (device) vkDeviceWaitIdle(device);
        destroy_staging();
        auto buffer_info = vk_structure<VkBufferCreateInfo>(
            VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        buffer_info.size = bytes;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!vk_ok(vkCreateBuffer(device, &buffer_info, nullptr, &staging_buffer),
                   "vkCreateBuffer"))
            return false;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, staging_buffer, &requirements);
        const uint32_t type = memory_type(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == std::numeric_limits<uint32_t>::max()) return false;
        auto allocation = vk_structure<VkMemoryAllocateInfo>(
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (!vk_ok(vkAllocateMemory(device, &allocation, nullptr, &staging_memory),
                   "vkAllocateMemory") ||
            !vk_ok(vkBindBufferMemory(device, staging_buffer, staging_memory, 0),
                   "vkBindBufferMemory"))
            return false;
        // Host-coherent memory needs no flush, so hold the mapping for the
        // buffer's lifetime rather than paying a map/unmap pair every frame.
        if (!vk_ok(vkMapMemory(device, staging_memory, 0, requirements.size, 0,
                               &staging_mapped), "vkMapMemory")) {
            staging_mapped = nullptr;
            return false;
        }
        staging_size = bytes;
        return true;
    }


    // Five seconds is far beyond any legitimate frame, and far shorter than
    // forever. A timeout abandons the frame and reports itself; the caller
    // presents again next frame and recovers when the queue drains.
    static constexpr uint64_t frame_wait_ns = 5ull * 1000 * 1000 * 1000;

    bool wait_frame_fence(const char* where) {
        const VkResult result =
            vkWaitForFences(device, 1, &frame_fence, VK_TRUE, frame_wait_ns);
        if (result == VK_SUCCESS) return true;
        whitty_wall_log::note("%s: frame fence %s", where,
                              result == VK_TIMEOUT ? "timed out" : "failed");
        if (result != VK_TIMEOUT) vk_ok(result, "vkWaitForFences");
        return false;
    }

    bool create_swapchain() {
        int drawable_width = 0;
        int drawable_height = 0;
        SDL_GetWindowSizeInPixels(window, &drawable_width, &drawable_height);
        if (drawable_width <= 0 || drawable_height <= 0) {
            static int noted = 0;
            if (whitty_wall_log::first(noted))
                whitty_wall_log::note("swapchain: window has no size yet");
            return false;
        }
        // A compositor resize can arrive after the previous image has been
        // submitted. Ensure no staging buffer or swapchain image is still in
        // use before replacing the surface-sized resources below.
        if (swapchain) vkDeviceWaitIdle(device);

        VkSurfaceCapabilitiesKHR capabilities{};
        if (!vk_ok(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                       physical_device, surface, &capabilities),
                   "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
            return false;
        uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface,
                                             &format_count, nullptr);
        if (!format_count) return false;
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface,
                                             &format_count, formats.data());
        const VkSurfaceFormatKHR selected = select_surface_format(formats);

        uint32_t mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface,
                                                  &mode_count, nullptr);
        std::vector<VkPresentModeKHR> modes(mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface,
                                                  &mode_count, modes.data());
        VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
        // An arcade wall must never let one cabinet stall another. FIFO
        // blocks until the compositor asks for a frame, and a compositor
        // throttles surfaces it considers occluded or idle - which freezes
        // that column's picture while its game carries on running. Mailbox
        // keeps every column advancing whatever the compositor thinks.
        const bool wall = settings.wall_count > 1;
        if (!settings.vsync || wall) {
            for (VkPresentModeKHR mode : modes) {
                if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
                    present_mode = mode;
                    break;
                }
                if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
                    present_mode = mode;
            }
        }

        VkExtent2D extent{};
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            extent = capabilities.currentExtent;
        } else {
            extent.width = std::clamp(static_cast<uint32_t>(drawable_width),
                                      capabilities.minImageExtent.width,
                                      capabilities.maxImageExtent.width);
            extent.height = std::clamp(static_cast<uint32_t>(drawable_height),
                                       capabilities.minImageExtent.height,
                                       capabilities.maxImageExtent.height);
        }
        uint32_t image_count = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount && image_count > capabilities.maxImageCount)
            image_count = capabilities.maxImageCount;
        if (extent.width == 0 || extent.height == 0) {
            // A hidden or still-mapping Wayland window reports a degenerate
            // extent. This is a retry, not a failure: the caller comes back
            // next frame once the compositor has given the surface a size.
            static int noted = 0;
            if (whitty_wall_log::first(noted))
                whitty_wall_log::note("swapchain: degenerate extent %ux%u",
                                      extent.width, extent.height);
            return false;
        }
        if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            whitty_wall_log::note("swapchain: surface lacks TRANSFER_DST");
            std::fprintf(stderr, "Vulkan surface cannot accept transfer output\n");
            return false;
        }
        // The native graphics path draws into the swapchain through a render
        // pass, so ask for colour-attachment usage as well. A surface that
        // cannot supply it still works: the transfer-only blit path remains.
        VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const bool attachable = (capabilities.supportedUsageFlags &
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        if (attachable) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        auto create = vk_structure<VkSwapchainCreateInfoKHR>(
            VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
        create.surface = surface;
        create.minImageCount = image_count;
        create.imageFormat = selected.format;
        create.imageColorSpace = selected.colorSpace;
        create.imageExtent = extent;
        create.imageArrayLayers = 1;
        create.imageUsage = usage;
        create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create.preTransform = capabilities.currentTransform;
        create.compositeAlpha = select_composite_alpha(capabilities.supportedCompositeAlpha);
        create.presentMode = present_mode;
        create.clipped = VK_TRUE;
        create.oldSwapchain = swapchain;
        VkSwapchainKHR replacement{};
        if (!vk_ok(vkCreateSwapchainKHR(device, &create, nullptr, &replacement),
                   "vkCreateSwapchainKHR"))
            return false;
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = replacement;
        swapchain_format = selected.format;
        swapchain_extent = extent;
        // Worth stating: an _SRGB surface silently re-encodes everything the
        // presentation pass writes, which looks like a brightness bug rather
        // than a format one.
        if (swapchain_format != previous_format) {
            previous_format = swapchain_format;
            std::printf("Vulkan swapchain format: %s\n",
                        swapchain_format == VK_FORMAT_B8G8R8A8_UNORM ?
                            "B8G8R8A8_UNORM" :
                        swapchain_format == VK_FORMAT_R8G8B8A8_UNORM ?
                            "R8G8B8A8_UNORM" :
                        swapchain_format == VK_FORMAT_B8G8R8A8_SRGB ?
                            "B8G8R8A8_SRGB (will look washed out)" :
                        swapchain_format == VK_FORMAT_R8G8B8A8_SRGB ?
                            "R8G8B8A8_SRGB (will look washed out)" :
                            "other");
        }
        vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
        swapchain_images.resize(image_count);
        vkGetSwapchainImagesKHR(device, swapchain, &image_count,
                                swapchain_images.data());
        swapchain_dirty = false;
        swapchain_attachable = attachable;
        // Rebuilt with the swapchain: the set is indexed by image, so it has
        // to match the new image count.
        if (!create_render_semaphores()) return false;
        if (!create_swapchain_targets()) return false;
        return create_staging(static_cast<VkDeviceSize>(extent.width) *
                              extent.height * 4);
    }

    void destroy_render_semaphores() {
        for (VkSemaphore semaphore : render_finished)
            if (semaphore) vkDestroySemaphore(device, semaphore, nullptr);
        render_finished.clear();
    }

    bool create_render_semaphores() {
        destroy_render_semaphores();
        auto info = vk_structure<VkSemaphoreCreateInfo>(
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        render_finished.resize(swapchain_images.size());
        for (VkSemaphore& semaphore : render_finished) {
            if (!vk_ok(vkCreateSemaphore(device, &info, nullptr, &semaphore),
                       "vkCreateSemaphore")) {
                destroy_render_semaphores();
                return false;
            }
        }
        return true;
    }

    void destroy_swapchain_targets() {
        for (VkFramebuffer buffer : framebuffers)
            if (buffer) vkDestroyFramebuffer(device, buffer, nullptr);
        for (VkImageView view : swapchain_views)
            if (view) vkDestroyImageView(device, view, nullptr);
        framebuffers.clear();
        swapchain_views.clear();
    }

    // Per-swapchain-image views and framebuffers for the native graphics
    // path. Failing here is not fatal: the transfer blit path needs neither.
    bool create_swapchain_targets() {
        destroy_swapchain_targets();
        if (!swapchain_attachable) return true;
        if (!render_pass && !create_render_pass()) return true;
        swapchain_views.reserve(swapchain_images.size());
        framebuffers.reserve(swapchain_images.size());
        for (VkImage image : swapchain_images) {
            auto view_info = vk_structure<VkImageViewCreateInfo>(
                VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
            view_info.image = image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = swapchain_format;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view_info.subresourceRange.levelCount = 1;
            view_info.subresourceRange.layerCount = 1;
            VkImageView view{};
            if (!vk_ok(vkCreateImageView(device, &view_info, nullptr, &view),
                       "vkCreateImageView")) {
                destroy_swapchain_targets();
                return true;
            }
            swapchain_views.push_back(view);
            auto buffer_info = vk_structure<VkFramebufferCreateInfo>(
                VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
            buffer_info.renderPass = render_pass;
            buffer_info.attachmentCount = 1;
            buffer_info.pAttachments = &view;
            buffer_info.width = swapchain_extent.width;
            buffer_info.height = swapchain_extent.height;
            buffer_info.layers = 1;
            VkFramebuffer buffer{};
            if (!vk_ok(vkCreateFramebuffer(device, &buffer_info, nullptr,
                                           &buffer),
                       "vkCreateFramebuffer")) {
                destroy_swapchain_targets();
                return true;
            }
            framebuffers.push_back(buffer);
        }
        return true;
    }

    // ---- Native graphics path construction ----------------------------

    VkShaderModule create_shader(const uint32_t* code, std::size_t bytes) {
        auto info = vk_structure<VkShaderModuleCreateInfo>(
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        info.codeSize = bytes;
        info.pCode = code;
        VkShaderModule module{};
        if (!vk_ok(vkCreateShaderModule(device, &info, nullptr, &module),
                   "vkCreateShaderModule"))
            return VK_NULL_HANDLE;
        return module;
    }

    void destroy_image(device_image& target) {
        if (target.view) vkDestroyImageView(device, target.view, nullptr);
        if (target.image) vkDestroyImage(device, target.image, nullptr);
        if (target.memory) vkFreeMemory(device, target.memory, nullptr);
        target = device_image{};
    }

    bool create_image(device_image& target, int width, int height, int layers,
                      VkFormat format,
                      VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                VK_IMAGE_USAGE_SAMPLED_BIT,
                      VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        destroy_image(target);
        auto info = vk_structure<VkImageCreateInfo>(
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {static_cast<uint32_t>(width),
                       static_cast<uint32_t>(height), 1};
        info.mipLevels = 1;
        info.arrayLayers = static_cast<uint32_t>(layers);
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!vk_ok(vkCreateImage(device, &info, nullptr, &target.image),
                   "vkCreateImage"))
            return false;
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, target.image, &requirements);
        const uint32_t type = memory_type(requirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == std::numeric_limits<uint32_t>::max()) return false;
        auto allocation = vk_structure<VkMemoryAllocateInfo>(
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (!vk_ok(vkAllocateMemory(device, &allocation, nullptr,
                                    &target.memory), "vkAllocateMemory") ||
            !vk_ok(vkBindImageMemory(device, target.image, target.memory, 0),
                   "vkBindImageMemory"))
            return false;
        auto view_info = vk_structure<VkImageViewCreateInfo>(
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        view_info.image = target.image;
        view_info.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY :
                                          VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange.aspectMask = aspect;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount =
            static_cast<uint32_t>(layers);
        if (!vk_ok(vkCreateImageView(device, &view_info, nullptr,
                                     &target.view), "vkCreateImageView"))
            return false;
        target.width = width;
        target.height = height;
        target.layers = layers;
        target.format = format;
        return true;
    }

    bool create_upload_buffer(VkDeviceSize bytes) {
        if (upload_size >= bytes) return true;
        vkDeviceWaitIdle(device);
        if (upload_mapped) vkUnmapMemory(device, upload_memory);
        upload_mapped = nullptr;
        if (upload_buffer) vkDestroyBuffer(device, upload_buffer, nullptr);
        if (upload_memory) vkFreeMemory(device, upload_memory, nullptr);
        upload_buffer = VK_NULL_HANDLE;
        upload_memory = VK_NULL_HANDLE;
        upload_size = 0;
        auto info = vk_structure<VkBufferCreateInfo>(
            VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!vk_ok(vkCreateBuffer(device, &info, nullptr, &upload_buffer),
                   "vkCreateBuffer"))
            return false;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, upload_buffer, &requirements);
        const uint32_t type = memory_type(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == std::numeric_limits<uint32_t>::max()) return false;
        auto allocation = vk_structure<VkMemoryAllocateInfo>(
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        if (!vk_ok(vkAllocateMemory(device, &allocation, nullptr,
                                    &upload_memory), "vkAllocateMemory") ||
            !vk_ok(vkBindBufferMemory(device, upload_buffer, upload_memory, 0),
                   "vkBindBufferMemory") ||
            !vk_ok(vkMapMemory(device, upload_memory, 0, requirements.size, 0,
                               &upload_mapped), "vkMapMemory"))
            return false;
        upload_size = bytes;
        return true;
    }

    bool create_pipeline(VkPipelineLayout layout, VkShaderModule vertex,
                         VkShaderModule fragment, bool blend,
                         VkPipeline& pipeline) {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0] = vk_structure<VkPipelineShaderStageCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1] = vk_structure<VkPipelineShaderStageCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";

        // Every quad is generated from gl_VertexIndex, so there is no vertex
        // buffer and no input state to describe.
        auto vertex_input = vk_structure<VkPipelineVertexInputStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        auto assembly = vk_structure<VkPipelineInputAssemblyStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        auto viewport_state = vk_structure<VkPipelineViewportStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;
        auto raster = vk_structure<VkPipelineRasterizationStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        auto multisample = vk_structure<VkPipelineMultisampleStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.blendEnable = blend ? VK_TRUE : VK_FALSE;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        auto blend_state = vk_structure<VkPipelineColorBlendStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
        blend_state.attachmentCount = 1;
        blend_state.pAttachments = &attachment;
        const std::array<VkDynamicState, 2> dynamic_states{
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        auto dynamic = vk_structure<VkPipelineDynamicStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        dynamic.dynamicStateCount =
            static_cast<uint32_t>(dynamic_states.size());
        dynamic.pDynamicStates = dynamic_states.data();

        auto info = vk_structure<VkGraphicsPipelineCreateInfo>(
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport_state;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pColorBlendState = &blend_state;
        info.pDynamicState = &dynamic;
        info.layout = layout;
        info.renderPass = render_pass;
        info.subpass = 0;
        return vk_ok(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                               &info, nullptr, &pipeline),
                     "vkCreateGraphicsPipelines");
    }

    bool create_native_path() {
        if (native_ready) return true;
        if (native_failed) {
            static int noted = 0;
            if (whitty_wall_log::first(noted))
                whitty_wall_log::note("native path: latched off");
            return false;
        }
        // Missing swapchain targets are a startup race, not a verdict: the
        // initial swapchain can fail or come out degenerate while a wall
        // column's window is still being mapped and shuffled by the
        // compositor, and nothing else in the rasterise path ever retried -
        // so the column stayed picture-less forever while its game ran.
        if (!swapchain_attachable || !render_pass || framebuffers.empty()) {
            if (--native_retry_countdown > 0) return false;
            native_retry_countdown = 30;
            static int noted = 0;
            if (whitty_wall_log::first(noted, 6))
                whitty_wall_log::note(
                    "native path: swapchain not usable (attachable=%d "
                    "render_pass=%d framebuffers=%zu) - retrying",
                    swapchain_attachable ? 1 : 0, render_pass ? 1 : 0,
                    framebuffers.size());
            swapchain_dirty = true;
            if (!create_swapchain() || !swapchain_attachable || !render_pass ||
                framebuffers.empty()) {
                static int failed_note = 0;
                if (whitty_wall_log::first(failed_note, 6))
                    whitty_wall_log::note("native path: swapchain retry "
                                          "failed");
                return false;
            }
            whitty_wall_log::note("native path: swapchain recovered");
        }

        auto sampler_info = vk_structure<VkSamplerCreateInfo>(
            VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        if (!vk_ok(vkCreateSampler(device, &sampler_info, nullptr,
                                   &nearest_sampler), "vkCreateSampler"))
            return native_give_up();
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        if (!vk_ok(vkCreateSampler(device, &sampler_info, nullptr,
                                   &linear_sampler), "vkCreateSampler"))
            return native_give_up();

        std::array<VkDescriptorSetLayoutBinding, 2> post_bindings{};
        for (uint32_t index = 0; index < post_bindings.size(); ++index) {
            post_bindings[index].binding = index;
            post_bindings[index].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            post_bindings[index].descriptorCount = 1;
            post_bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        auto set_info = vk_structure<VkDescriptorSetLayoutCreateInfo>(
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        set_info.bindingCount = static_cast<uint32_t>(post_bindings.size());
        set_info.pBindings = post_bindings.data();
        if (!vk_ok(vkCreateDescriptorSetLayout(device, &set_info, nullptr,
                                               &post_set_layout),
                   "vkCreateDescriptorSetLayout"))
            return native_give_up();
        set_info.bindingCount = 1;
        if (!vk_ok(vkCreateDescriptorSetLayout(device, &set_info, nullptr,
                                               &overlay_set_layout),
                   "vkCreateDescriptorSetLayout"))
            return native_give_up();

        VkPushConstantRange post_range{};
        post_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        post_range.size = sizeof(post_push_constants);
        auto layout_info = vk_structure<VkPipelineLayoutCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &post_set_layout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &post_range;
        if (!vk_ok(vkCreatePipelineLayout(device, &layout_info, nullptr,
                                          &post_pipeline_layout),
                   "vkCreatePipelineLayout"))
            return native_give_up();
        layout_info.pSetLayouts = &overlay_set_layout;
        layout_info.pushConstantRangeCount = 0;
        layout_info.pPushConstantRanges = nullptr;
        if (!vk_ok(vkCreatePipelineLayout(device, &layout_info, nullptr,
                                          &overlay_pipeline_layout),
                   "vkCreatePipelineLayout"))
            return native_give_up();
        VkPushConstantRange solid_range{};
        solid_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        solid_range.size = sizeof(float) * 4;
        layout_info.setLayoutCount = 0;
        layout_info.pSetLayouts = nullptr;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &solid_range;
        if (!vk_ok(vkCreatePipelineLayout(device, &layout_info, nullptr,
                                          &solid_pipeline_layout),
                   "vkCreatePipelineLayout"))
            return native_give_up();

        const VkShaderModule post_vertex =
            create_shader(arcade_post_vert_spirv, sizeof(arcade_post_vert_spirv));
        const VkShaderModule post_fragment =
            create_shader(arcade_post_frag_spirv, sizeof(arcade_post_frag_spirv));
        const VkShaderModule overlay_vertex = create_shader(
            arcade_overlay_vert_spirv, sizeof(arcade_overlay_vert_spirv));
        const VkShaderModule overlay_fragment = create_shader(
            arcade_overlay_frag_spirv, sizeof(arcade_overlay_frag_spirv));
        const VkShaderModule solid_vertex = create_shader(
            fullscreen_quad_vert_spirv, sizeof(fullscreen_quad_vert_spirv));
        const VkShaderModule solid_fragment = create_shader(
            arcade_solid_frag_spirv, sizeof(arcade_solid_frag_spirv));
        const bool modules_ready = post_vertex && post_fragment &&
            overlay_vertex && overlay_fragment && solid_vertex &&
            solid_fragment;
        const bool pipelines_ready = modules_ready &&
            create_pipeline(post_pipeline_layout, post_vertex, post_fragment,
                            false, post_pipeline) &&
            create_pipeline(overlay_pipeline_layout, overlay_vertex,
                            overlay_fragment, true, overlay_pipeline) &&
            create_pipeline(solid_pipeline_layout, solid_vertex,
                            solid_fragment, false, solid_pipeline);
        for (VkShaderModule module : {post_vertex, post_fragment,
                                      overlay_vertex, overlay_fragment,
                                      solid_vertex, solid_fragment})
            if (module) vkDestroyShaderModule(device, module, nullptr);
        if (!pipelines_ready) return native_give_up();

        // Two images for the post set, one for each overlay slot.
        std::array<VkDescriptorPoolSize, 1> sizes{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[0].descriptorCount = 2 * max_panes + overlay_count;
        auto pool_info = vk_structure<VkDescriptorPoolCreateInfo>(
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        pool_info.maxSets = max_panes + overlay_count;
        pool_info.poolSizeCount = static_cast<uint32_t>(sizes.size());
        pool_info.pPoolSizes = sizes.data();
        if (!vk_ok(vkCreateDescriptorPool(device, &pool_info, nullptr,
                                          &descriptor_pool),
                   "vkCreateDescriptorPool"))
            return native_give_up();
        std::array<VkDescriptorSetLayout, max_panes + overlay_count> layouts{};
        for (int index = 0; index < max_panes; ++index)
            layouts[static_cast<std::size_t>(index)] = post_set_layout;
        for (int index = 0; index < overlay_count; ++index)
            layouts[static_cast<std::size_t>(index) + max_panes] =
                overlay_set_layout;
        std::array<VkDescriptorSet, max_panes + overlay_count> sets{};
        auto allocate = vk_structure<VkDescriptorSetAllocateInfo>(
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        allocate.descriptorPool = descriptor_pool;
        allocate.descriptorSetCount = static_cast<uint32_t>(layouts.size());
        allocate.pSetLayouts = layouts.data();
        if (!vk_ok(vkAllocateDescriptorSets(device, &allocate, sets.data()),
                   "vkAllocateDescriptorSets"))
            return native_give_up();
        for (int index = 0; index < max_panes; ++index)
            post_set =
                sets[static_cast<std::size_t>(index)];
        for (int index = 0; index < overlay_count; ++index)
            overlays[static_cast<std::size_t>(index)].set =
                sets[static_cast<std::size_t>(index) + max_panes];

        // The gamma ramp is only consulted for System 22 board colour, but the
        // descriptor must always point at a real image. Seed an identity ramp
        // so an RGBA board that never uploads one still validates.
        if (!create_image(gamma_image, 256, 1, 3, VK_FORMAT_R8_UINT))
            return native_give_up();
        std::array<uint8_t, 256 * 3> identity{};
        for (int channel = 0; channel < 3; ++channel)
            for (int level = 0; level < 256; ++level)
                identity[static_cast<std::size_t>(channel) * 256 + level] =
                    static_cast<uint8_t>(level);
        if (!upload_image(gamma_image, identity.data(), identity.size()))
            return native_give_up();
        // Same reasoning for each overlay slot: a 1x1 transparent texel keeps
        // its set valid on frames where that panel is not shown.
        const std::array<uint8_t, 4> blank{};
        for (overlay_slot& slot : overlays) {
            if (!create_image(slot.image, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM) ||
                !upload_image(slot.image, blank.data(), blank.size()))
                return native_give_up();
        }

        native_ready = true;
        return true;
    }

    // Reports a refusal once and returns false. A silent `return false` here
    // is indistinguishable from a hang, because the window is only shown
    // after the first successful present - which has cost several debugging
    // rounds on this port already.
    bool decline(const char* reason) {
        static const char* reported = nullptr;
        if (reported != reason) {
            reported = reason;
            std::fprintf(stderr, "Vulkan frame declined: %s\n", reason);
        }
        return false;
    }

    bool native_give_up() {
        native_failed = true;
        native_ready = false;
        whitty_wall_log::note("native path gave up permanently");
        std::fprintf(stderr,
                     "Vulkan native render path unavailable; falling back to "
                     "the transfer blit path\n");
        return false;
    }

    void destroy_native_path() {
        destroy_scene_resources();
        if (scene_framebuffer)
            vkDestroyFramebuffer(device, scene_framebuffer, nullptr);
        scene_framebuffer = VK_NULL_HANDLE;
        if (scene_render_pass)
            vkDestroyRenderPass(device, scene_render_pass, nullptr);
        scene_render_pass = VK_NULL_HANDLE;
        destroy_image(scene_depth);
        destroy_image(scene_image);
        destroy_image(gamma_image);
        for (overlay_slot& slot : overlays) {
            destroy_image(slot.image);
            slot.set = VK_NULL_HANDLE;
            slot.resident = false;
        }
        if (upload_mapped) vkUnmapMemory(device, upload_memory);
        upload_mapped = nullptr;
        if (upload_buffer) vkDestroyBuffer(device, upload_buffer, nullptr);
        if (upload_memory) vkFreeMemory(device, upload_memory, nullptr);
        upload_buffer = VK_NULL_HANDLE;
        upload_memory = VK_NULL_HANDLE;
        upload_size = 0;
        if (descriptor_pool)
            vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        descriptor_pool = VK_NULL_HANDLE;
        post_set = VK_NULL_HANDLE;
        for (VkPipeline* pipeline :
             {&post_pipeline, &overlay_pipeline, &solid_pipeline}) {
            if (*pipeline) vkDestroyPipeline(device, *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
        }
        for (VkPipelineLayout* layout :
             {&post_pipeline_layout, &overlay_pipeline_layout,
              &solid_pipeline_layout}) {
            if (*layout) vkDestroyPipelineLayout(device, *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
        for (VkDescriptorSetLayout* layout :
             {&post_set_layout, &overlay_set_layout}) {
            if (*layout)
                vkDestroyDescriptorSetLayout(device, *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
        for (VkSampler* sampler : {&nearest_sampler, &linear_sampler}) {
            if (*sampler) vkDestroySampler(device, *sampler, nullptr);
            *sampler = VK_NULL_HANDLE;
        }
        destroy_swapchain_targets();
        if (render_pass) vkDestroyRenderPass(device, render_pass, nullptr);
        render_pass = VK_NULL_HANDLE;
        native_ready = false;
        descriptors_valid = false;
    }

    bool create_render_pass() {
        VkAttachmentDescription attachment{};
        attachment.format = swapchain_format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference reference{};
        reference.attachment = 0;
        reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &reference;
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        auto info = vk_structure<VkRenderPassCreateInfo>(
            VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 1;
        info.pDependencies = &dependency;
        return vk_ok(vkCreateRenderPass(device, &info, nullptr, &render_pass),
                     "vkCreateRenderPass");
    }

    // Routes validation messages into this column's own log. Three wall
    // processes share one terminal, so stderr cannot say whose fault a
    // message reports - the per-process file can.
    static VKAPI_ATTR VkBool32 VKAPI_CALL validation_to_wall_log(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT &&
            data && data->pMessage)
            whitty_wall_log::note("VALIDATION: %.900s", data->pMessage);
        return VK_FALSE;
    }

    bool initialize_vulkan() {
        Uint32 extension_count = 0;
        const char* const* sdl_extensions =
            SDL_Vulkan_GetInstanceExtensions(&extension_count);
        if (!sdl_extensions)
            return false;
        std::vector<const char*> extensions(
            sdl_extensions, sdl_extensions + extension_count);
        // WHITTY_VK_VALIDATION=1: run with the validation layer and write its
        // findings to the wall column log. This is how a device loss gets a
        // name: the message naming the invalid work arrives before the
        // device dies.
        const char* want_validation = std::getenv("WHITTY_VK_VALIDATION");
        const bool validation = want_validation && *want_validation == '1';
        static const char* const validation_layer =
            "VK_LAYER_KHRONOS_validation";
        if (validation)
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        auto app = vk_structure<VkApplicationInfo>(
            VK_STRUCTURE_TYPE_APPLICATION_INFO);
        app.pApplicationName = "WhittyArcade";
        app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app.pEngineName = "WhittyArcade Output";
        app.apiVersion = VK_API_VERSION_1_0;
        auto create = vk_structure<VkInstanceCreateInfo>(
            VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        create.pApplicationInfo = &app;
        create.enabledExtensionCount =
            static_cast<uint32_t>(extensions.size());
        create.ppEnabledExtensionNames = extensions.data();
        if (validation) {
            create.enabledLayerCount = 1;
            create.ppEnabledLayerNames = &validation_layer;
        }
        if (!vk_ok(vkCreateInstance(&create, nullptr, &instance),
                   "vkCreateInstance") && validation) {
            // The layer may not be installed; run without it rather than
            // refusing to start.
            whitty_wall_log::note("validation layer unavailable");
            create.enabledLayerCount = 0;
            create.enabledExtensionCount = extension_count;
            if (!vk_ok(vkCreateInstance(&create, nullptr, &instance),
                       "vkCreateInstance"))
                return false;
        } else if (!instance) {
            return false;
        }
        if (validation && instance) {
            auto messenger_info =
                vk_structure<VkDebugUtilsMessengerCreateInfoEXT>(
                    VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
            messenger_info.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messenger_info.messageType =
                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
            messenger_info.pfnUserCallback = validation_to_wall_log;
            auto create_messenger =
                reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(
                        instance, "vkCreateDebugUtilsMessengerEXT"));
            if (create_messenger)
                create_messenger(instance, &messenger_info, nullptr,
                                 &debug_messenger);
            whitty_wall_log::note("validation layer active");
        }
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
            return false;

        uint32_t device_count = 0;
        vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
        for (VkPhysicalDevice candidate : devices) {
            uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count,
                                                     nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count,
                                                     families.data());
            for (uint32_t family = 0; family < family_count; ++family) {
                VkBool32 presents = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface,
                                                     &presents);
                if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presents) {
                    physical_device = candidate;
                    queue_family = family;
                    break;
                }
            }
            if (physical_device) break;
        }
        if (!physical_device) {
            std::fprintf(stderr, "No Vulkan presentation device found\n");
            return false;
        }
        const float priority = 1.0f;
        auto queue_info = vk_structure<VkDeviceQueueCreateInfo>(
            VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queue_info.queueFamilyIndex = queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        const char* device_extensions[]{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        // The System 22 polygon shader writes a second colour output whose
        // alpha carries per-pixel opacity, independent of the framebuffer
        // alpha that holds text priority. That needs dualSrcBlend, which is
        // optional, so record whether the device has it: the polygon pipeline
        // cannot be built without it and must decline to OpenGL instead.
        VkPhysicalDeviceFeatures available{};
        vkGetPhysicalDeviceFeatures(physical_device, &available);
        dual_source_blend = available.dualSrcBlend == VK_TRUE;
        VkPhysicalDeviceFeatures enabled{};
        enabled.dualSrcBlend = available.dualSrcBlend;
        auto device_info = vk_structure<VkDeviceCreateInfo>(
            VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = 1;
        device_info.ppEnabledExtensionNames = device_extensions;
        device_info.pEnabledFeatures = &enabled;
        if (!vk_ok(vkCreateDevice(physical_device, &device_info, nullptr, &device),
                   "vkCreateDevice"))
            return false;
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        auto pool = vk_structure<VkCommandPoolCreateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        pool.queueFamilyIndex = queue_family;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (!vk_ok(vkCreateCommandPool(device, &pool, nullptr, &command_pool),
                   "vkCreateCommandPool"))
            return false;
        auto command = vk_structure<VkCommandBufferAllocateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        command.commandPool = command_pool;
        command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command.commandBufferCount = 1;
        if (!vk_ok(vkAllocateCommandBuffers(device, &command, &command_buffer),
                   "vkAllocateCommandBuffers"))
            return false;
        auto semaphore = vk_structure<VkSemaphoreCreateInfo>(
            VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        auto fence = vk_structure<VkFenceCreateInfo>(
            VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        return vk_ok(vkCreateSemaphore(device, &semaphore, nullptr,
                                       &image_available), "vkCreateSemaphore") &&
               vk_ok(vkCreateFence(device, &fence, nullptr, &frame_fence),
                     "vkCreateFence") && create_swapchain();
    }

    void shutdown_vulkan() {
        if (device) vkDeviceWaitIdle(device);
        if (device) destroy_native_path();
        destroy_staging();
        if (frame_fence) vkDestroyFence(device, frame_fence, nullptr);
        destroy_render_semaphores();
        if (image_available) vkDestroySemaphore(device, image_available, nullptr);
        // Nothing may still be reading the staging buffer when the pool
        // that owns its command buffer goes.
        await_upload();
        if (upload_fence) vkDestroyFence(device, upload_fence, nullptr);
        upload_fence = VK_NULL_HANDLE;
        if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (debug_messenger && instance) {
            auto destroy_messenger =
                reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(
                        instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy_messenger)
                destroy_messenger(instance, debug_messenger, nullptr);
        }
        debug_messenger = VK_NULL_HANDLE;
        if (instance) vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
        surface = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
    }

    void record_image_upload(VkCommandBuffer commands, device_image& target,
                             VkDeviceSize buffer_offset) {
        record_image_upload_from(commands, target, upload_buffer,
                                 buffer_offset);
    }

    void record_image_upload_from(VkCommandBuffer commands,
                                  device_image& target, VkBuffer source,
                                  VkDeviceSize buffer_offset) {
        auto barrier = vk_structure<VkImageMemoryBarrier>(
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = target.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount =
            static_cast<uint32_t>(target.layers);
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.bufferOffset = buffer_offset;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount =
            static_cast<uint32_t>(target.layers);
        copy.imageExtent = {static_cast<uint32_t>(target.width),
                            static_cast<uint32_t>(target.height), 1};
        vkCmdCopyBufferToImage(commands, source, target.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
    }

    // Waits for a one-shot upload, and only for that upload.
    //
    // These used to end in vkQueueWaitIdle, which drains the entire queue -
    // the vsync-blocked presentation included - rather than the copy just
    // submitted. A board republishes its texture sheets whenever its attract
    // mode changes scene, so that turned every scene change into a stall of
    // up to a whole frame, and three cabinets sharing one GPU stalled each
    // other. Waiting on this submission's own fence costs the transfer and
    // nothing else.
    bool await_upload() {
        if (!upload_in_flight) return true;
        upload_in_flight = false;
        const VkResult waited = vkWaitForFences(device, 1, &upload_fence,
                                                VK_TRUE, frame_wait_ns);
        if (waited == VK_TIMEOUT)
            whitty_wall_log::note("upload fence timed out");
        const bool signalled = waited == VK_SUCCESS ||
                               vk_ok(waited, "vkWaitForFences");
        vkResetFences(device, 1, &upload_fence);
        if (upload_commands)
            vkFreeCommandBuffers(device, command_pool, 1, &upload_commands);
        upload_commands = VK_NULL_HANDLE;
        return signalled;
    }

    bool ensure_upload_fence() {
        if (upload_fence) return true;
        auto info = vk_structure<VkFenceCreateInfo>(
            VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        return vk_ok(vkCreateFence(device, &info, nullptr, &upload_fence),
                     "vkCreateFence");
    }

    // One-shot upload used while building the fixed images (gamma ramp, blank
    // overlay). Per-frame uploads ride the presentation command buffer.
    // The whole image, in bytes, for the format it was created with.
    static VkDeviceSize image_full_bytes(const device_image& target) {
        VkDeviceSize texel = 1;
        switch (target.format) {
        case VK_FORMAT_R16_UINT: texel = 2; break;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM: texel = 4; break;
        default: break;
        }
        return static_cast<VkDeviceSize>(target.width) * target.height *
               std::max(target.layers, 1) * texel;
    }

    bool upload_image(device_image& target, const uint8_t* data,
                      std::size_t bytes) {
        // The staging buffer is about to be rewritten, so any upload still
        // reading it has to finish first.
        if (!await_upload() || !ensure_upload_fence()) return false;
        // The copy below always covers the image's full extent, so the
        // staging buffer must hold the full image even when the caller
        // supplies less - a board whose ROM only fills half its sheet
        // (Ridge Racer's sprites, against Rave Racer's full set) otherwise
        // records a copy that reads past the end of the buffer. That
        // out-of-bounds read is undefined: it survived every single-cabinet
        // run and killed the device on the arcade wall, which is why the
        // wall looked broken while the same game alone looked fine. The
        // unfilled remainder is zeroed - pen 0, the same empty result the
        // cleared sheet started with.
        const VkDeviceSize full = image_full_bytes(target);
        if (bytes > full) bytes = static_cast<std::size_t>(full);
        if (!create_upload_buffer(full)) return false;
        std::memcpy(upload_mapped, data, bytes);
        if (bytes < full)
            std::memset(static_cast<uint8_t*>(upload_mapped) + bytes, 0,
                        static_cast<std::size_t>(full - bytes));
        auto allocate = vk_structure<VkCommandBufferAllocateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        allocate.commandPool = command_pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer commands{};
        if (!vk_ok(vkAllocateCommandBuffers(device, &allocate, &commands),
                   "vkAllocateCommandBuffers"))
            return false;
        auto begin = vk_structure<VkCommandBufferBeginInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commands, &begin);
        record_image_upload(commands, target, 0);
        vkEndCommandBuffer(commands);
        auto submit = vk_structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        const bool submitted =
            vk_ok(vkQueueSubmit(queue, 1, &submit, upload_fence),
                  "vkQueueSubmit");
        if (!submitted) {
            vkFreeCommandBuffers(device, command_pool, 1, &commands);
            return false;
        }
        // Held rather than freed: the GPU is still reading this buffer, and
        // await_upload releases it once the fence says otherwise.
        upload_commands = commands;
        upload_in_flight = true;
        return await_upload();
    }

    void refresh_descriptors() {
        if (descriptors_valid) return;
        constexpr std::size_t post_bindings = 2;
        constexpr std::size_t total = post_bindings + overlay_count;
        std::array<VkDescriptorImageInfo, total> images{};
        images[0].sampler =
            scene_uses_linear ? linear_sampler : nearest_sampler;
        images[0].imageView = scene_image.view;
        images[1].sampler = nearest_sampler;
        images[1].imageView = gamma_image.view;
        for (std::size_t index = 0; index < overlay_count; ++index) {
            // Panels are authored at their final pixel size, so filtering them
            // linearly keeps text from crawling when a pane is scaled.
            images[index + post_bindings].sampler = linear_sampler;
            images[index + post_bindings].imageView =
                overlays[index].image.view;
        }
        std::array<VkWriteDescriptorSet, total> writes{};
        for (std::size_t index = 0; index < total; ++index) {
            images[index].imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[index] = vk_structure<VkWriteDescriptorSet>(
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            writes[index].dstSet =
                index < post_bindings ? post_set :
                overlays[index - post_bindings].set;
            writes[index].dstBinding =
                index < post_bindings ? static_cast<uint32_t>(index) : 0u;
            writes[index].descriptorCount = 1;
            writes[index].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[index].pImageInfo = &images[index];
        }
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
        descriptors_valid = true;
    }

    void draw_quad(VkCommandBuffer commands, const fitted_viewport& rect) {
        VkViewport viewport{};
        viewport.x = static_cast<float>(rect.x);
        viewport.y = static_cast<float>(rect.y);
        viewport.width = static_cast<float>(std::max(rect.w, 1));
        viewport.height = static_cast<float>(std::max(rect.h, 1));
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.offset = {std::max(rect.x, 0), std::max(rect.y, 0)};
        scissor.extent = {
            static_cast<uint32_t>(std::clamp(
                rect.w, 0, std::max(static_cast<int>(swapchain_extent.width) -
                                        std::max(rect.x, 0), 0))),
            static_cast<uint32_t>(std::clamp(
                rect.h, 0, std::max(static_cast<int>(swapchain_extent.height) -
                                        std::max(rect.y, 0), 0)))};
        if (!scissor.extent.width || !scissor.extent.height) return;
        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);
        vkCmdDraw(commands, 4, 1, 0, 0);
    }

    // Variant of create_pipeline for the scene render pass: vertex input,
    // depth state and the System 22 dual-source blend all differ from the
    // presentation quads above.
    bool create_scene_pipeline(VkPipelineLayout layout, VkShaderModule vertex,
                               VkShaderModule fragment,
                               const VkPipelineVertexInputStateCreateInfo&
                                   vertex_input,
                               bool depth_test, bool dual_source_blend,
                               VkPipeline& pipeline,
                               VkCompareOp depth_compare =
                                   VK_COMPARE_OP_LESS_OR_EQUAL,
                               bool alpha_blend = false,
                               VkPrimitiveTopology topology =
                                   VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0] = vk_structure<VkPipelineShaderStageCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1] = vk_structure<VkPipelineShaderStageCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";
        auto assembly = vk_structure<VkPipelineInputAssemblyStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
        // Explicit rather than inferred: this used to follow the depth
        // flag, and removing System 22's depth test silently turned its
        // triangle list into a strip - every consecutive vertex triplet
        // became a spurious triangle across the whole scene.
        assembly.topology = topology;
        auto viewport_state = vk_structure<VkPipelineViewportStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;
        auto raster = vk_structure<VkPipelineRasterizationStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.0f;
        auto multisample = vk_structure<VkPipelineMultisampleStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        auto depth = vk_structure<VkPipelineDepthStencilStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
        depth.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
        depth.depthWriteEnable = depth_test ? VK_TRUE : VK_FALSE;
        depth.depthCompareOp = depth_compare;
        depth.maxDepthBounds = 1.0f;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.blendEnable =
            (dual_source_blend || alpha_blend) ? VK_TRUE : VK_FALSE;
        // System 22's fragment shader carries per-pixel opacity in a second
        // colour output, kept separate from the framebuffer alpha that holds
        // text priority. Model 2's overlay blends on plain source alpha.
        attachment.srcColorBlendFactor = dual_source_blend ?
            VK_BLEND_FACTOR_SRC1_ALPHA : VK_BLEND_FACTOR_SRC_ALPHA;
        attachment.dstColorBlendFactor = dual_source_blend ?
            VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA :
            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        attachment.colorWriteMask =
            VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        auto blend_state = vk_structure<VkPipelineColorBlendStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
        blend_state.attachmentCount = 1;
        blend_state.pAttachments = &attachment;
        const std::array<VkDynamicState, 2> dynamic_states{
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        auto dynamic = vk_structure<VkPipelineDynamicStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        dynamic.dynamicStateCount =
            static_cast<uint32_t>(dynamic_states.size());
        dynamic.pDynamicStates = dynamic_states.data();
        auto info = vk_structure<VkGraphicsPipelineCreateInfo>(
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport_state;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend_state;
        info.pDynamicState = &dynamic;
        info.layout = layout;
        info.renderPass = scene_render_pass;
        info.subpass = 0;
        return vk_ok(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                               &info, nullptr, &pipeline),
                     "vkCreateGraphicsPipelines");
    }

    // ---- Offscreen scene target -----------------------------------------
    //
    // Polygon boards rasterise into an image at the board's own resolution,
    // which the post pass then samples exactly as it samples an uploaded RGBA
    // frame. One scene image serves both: the RGBA path copies into it, the
    // polygon path draws into it, and everything downstream is identical.

    VkFormat pick_depth_format() const {
        for (VkFormat candidate : {VK_FORMAT_D32_SFLOAT,
                                   VK_FORMAT_D24_UNORM_S8_UINT,
                                   VK_FORMAT_D16_UNORM}) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical_device, candidate,
                                                &properties);
            if (properties.optimalTilingFeatures &
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                return candidate;
        }
        return VK_FORMAT_UNDEFINED;
    }

    bool create_scene_render_pass() {
        if (scene_render_pass) return true;
        scene_depth_format = pick_depth_format();
        if (scene_depth_format == VK_FORMAT_UNDEFINED) return false;
        std::array<VkAttachmentDescription, 2> attachments{};
        attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Ends ready for the post pass to sample without a further barrier.
        attachments[0].finalLayout =
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        attachments[1].format = scene_depth_format;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color_reference{};
        color_reference.attachment = 0;
        color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference depth_reference{};
        depth_reference.attachment = 1;
        depth_reference.layout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_reference;
        subpass.pDepthStencilAttachment = &depth_reference;
        // The post pass reads this image in the same command buffer, so make
        // the write visible to the fragment stage that follows.
        VkSubpassDependency dependency{};
        dependency.srcSubpass = 0;
        dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
        dependency.srcStageMask =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        auto info = vk_structure<VkRenderPassCreateInfo>(
            VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
        info.attachmentCount = static_cast<uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 1;
        info.pDependencies = &dependency;
        return vk_ok(vkCreateRenderPass(device, &info, nullptr,
                                        &scene_render_pass),
                     "vkCreateRenderPass");
    }

    // Geometry of each System 22 source sheet, matching the OpenGL textures
    // it replaces. R16_UINT for the tile map because its entries are 16-bit.
    // Everything per-sheet lives here. Keeping a property in a separate
    // index-keyed function is what previously let the texel size drift out of
    // step with the format, undersizing three staging buffers by 4x.
    struct sheet_shape {
        int width;
        int height;
        int layers;
        VkFormat format;
        int bytes_per_texel;
        // Rewritten by the board as it runs, so it cannot use the one-shot
        // upload: a queue wait every frame would serialise CPU against GPU.
        // These stage instead and are copied inside the frame's own buffer.
        bool streams;
    };
    static sheet_shape shape_of(int index) {
        switch (index) {
        case sheet_texture_tiles: return {2048, 1024, 8, VK_FORMAT_R8_UINT, 1, false};
        case sheet_texture_map:   return {256, 4096, 1, VK_FORMAT_R16_UINT, 2, false};
        case sheet_texture_attr:  return {1024, 512, 1, VK_FORMAT_R8_UINT, 1, false};
        case sheet_palette:       return {256, 128, 3, VK_FORMAT_R8_UINT, 1, true};
        case sheet_character_data: return {512, 256, 1, VK_FORMAT_R8_UINT, 1, true};
        case sheet_text_map:      return {64, 64, 1, VK_FORMAT_R16_UINT, 2, true};
        // Model 2. The colour table is GL_RGB8 on the OpenGL side; three
        // byte formats are not sampleable in Vulkan optimal tiling on this
        // hardware, so it is widened to RGBA when uploaded.
        case sheet_model2_base:
        case sheet_model2_foreground:
            return {496, 384, 1, VK_FORMAT_R8G8B8A8_UNORM, 4, true};
        case sheet_model2_sheets: return {1024, 1024, 2, VK_FORMAT_R8_UINT, 1, false};
        case sheet_model2_luma:   return {256, 128, 1, VK_FORMAT_R8_UINT, 1, true};
        case sheet_model2_color:  return {1024, 64, 1, VK_FORMAT_R8G8B8A8_UNORM, 4, true};
        default:                  return {1024, 1024, 16, VK_FORMAT_R8_UINT, 1, false};
        }
    }

    // Clears a sheet and leaves it in the layout its descriptor advertises.
    //
    // Not every board fills every sheet - Ridge Racer has no sprite region at
    // all, so sprite_tiles is never uploaded - but the descriptor set still
    // binds it and the shader may still sample it. An image left in
    // VK_IMAGE_LAYOUT_UNDEFINED while a descriptor claims it is readable is
    // undefined behaviour, and in practice costs the device.
    bool initialise_sheet(device_image& sheet) {
        if (!await_upload() || !ensure_upload_fence()) {
            static int noted = 0;
            if (whitty_wall_log::first(noted))
                whitty_wall_log::note("sheet init: upload fence unavailable");
            return false;
        }
        auto allocate = vk_structure<VkCommandBufferAllocateInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        allocate.commandPool = command_pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer commands{};
        if (!vk_ok(vkAllocateCommandBuffers(device, &allocate, &commands),
                   "vkAllocateCommandBuffers"))
            return false;
        auto begin = vk_structure<VkCommandBufferBeginInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commands, &begin);
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = static_cast<uint32_t>(sheet.layers);
        auto barrier = vk_structure<VkImageMemoryBarrier>(
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = sheet.image;
        barrier.subresourceRange = range;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        const VkClearColorValue zero{};
        vkCmdClearColorImage(commands, sheet.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                             &range);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        vkEndCommandBuffer(commands);
        auto submit = vk_structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        const bool submitted =
            vk_ok(vkQueueSubmit(queue, 1, &submit, upload_fence),
                  "vkQueueSubmit");
        if (!submitted) {
            vkFreeCommandBuffers(device, command_pool, 1, &commands);
            return false;
        }
        upload_commands = commands;
        upload_in_flight = true;
        return await_upload();
    }

    bool ensure_scene_sheets() {
        for (int index = 0; index < scene_sheet_count; ++index) {
            device_image& sheet = scene_sheets[static_cast<std::size_t>(index)];
            if (sheet.image) continue;
            const sheet_shape shape = shape_of(index);
            if (!create_image(sheet, shape.width, shape.height, shape.layers,
                              shape.format) ||
                !initialise_sheet(sheet))
                return false;
            // A new image means a new view, so every set that samples these
            // sheets has to be written again - both boards' sets, not just
            // System 22's. Clearing one and not the other is how a board
            // ends up sampling a view that no longer exists.
            scene_descriptors_valid = false;
            model2_descriptors_valid = false;
        }
        return true;
    }

    bool create_host_buffer(VkBuffer& buffer, VkDeviceMemory& memory,
                            void*& mapped, VkDeviceSize bytes,
                            VkBufferUsageFlags usage) {
        auto info = vk_structure<VkBufferCreateInfo>(
            VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        info.size = bytes;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!vk_ok(vkCreateBuffer(device, &info, nullptr, &buffer),
                   "vkCreateBuffer"))
            return false;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        const uint32_t type = memory_type(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == std::numeric_limits<uint32_t>::max()) return false;
        auto allocation = vk_structure<VkMemoryAllocateInfo>(
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        return vk_ok(vkAllocateMemory(device, &allocation, nullptr, &memory),
                     "vkAllocateMemory") &&
               vk_ok(vkBindBufferMemory(device, buffer, memory, 0),
                     "vkBindBufferMemory") &&
               vk_ok(vkMapMemory(device, memory, 0, requirements.size, 0,
                                 &mapped), "vkMapMemory");
    }

    void destroy_host_buffer(VkBuffer& buffer, VkDeviceMemory& memory,
                             void*& mapped) {
        if (memory && mapped) vkUnmapMemory(device, memory);
        mapped = nullptr;
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    }

    // Grows the vertex buffer to hold a frame's worth of polygons. Kept
    // host-visible: the display list is rebuilt from scratch every frame, so
    // a staged copy would only add a transfer for data used once.
    bool ensure_vertex_capacity(VkDeviceSize bytes) {
        if (vertex_capacity >= bytes) return true;
        vkDeviceWaitIdle(device);
        destroy_host_buffer(vertex_buffer, vertex_memory, vertex_mapped);
        vertex_capacity = 0;
        const VkDeviceSize rounded = std::max<VkDeviceSize>(bytes * 2, 1 << 20);
        if (!create_host_buffer(vertex_buffer, vertex_memory, vertex_mapped,
                                rounded, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
            return false;
        vertex_capacity = rounded;
        return true;
    }

    bool create_scene_pipelines() {
        if (scene_pipelines_ready) return true;
        if (!dual_source_blend || !create_scene_render_pass() ||
            !ensure_scene_sheets()) {
            static int noted = 0;
            if (whitty_wall_log::first(noted))
                whitty_wall_log::note(
                    "scene pipelines: dual_src=%d render_pass=%d (sheets "
                    "otherwise)",
                    dual_source_blend ? 1 : 0, scene_render_pass ? 1 : 0);
            return false;
        }
        if (!create_host_buffer(polygon_uniform_buffer, polygon_uniform_memory,
                                polygon_uniform_mapped,
                                sizeof(system22_uniform_block),
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
            !create_host_buffer(text_uniform_buffer, text_uniform_memory,
                                text_uniform_mapped,
                                sizeof(system22_text_uniform_block),
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) {
            static int noted = 0;
            if (whitty_wall_log::first(noted))
                whitty_wall_log::note("scene pipelines: uniform buffers "
                                      "failed");
            return false;
        }

        // Binding 0 is the uniform block; the rest are the sheets, in the
        // order the shaders declare them.
        const auto make_set_layout = [&](uint32_t sampler_count,
                                         VkDescriptorSetLayout& layout) {
            std::vector<VkDescriptorSetLayoutBinding> bindings(
                sampler_count + 1);
            for (uint32_t index = 0; index < bindings.size(); ++index) {
                bindings[index].binding = index;
                bindings[index].descriptorType = index == 0 ?
                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER :
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bindings[index].descriptorCount = 1;
                bindings[index].stageFlags = index == 0 ?
                    (VK_SHADER_STAGE_VERTEX_BIT |
                     VK_SHADER_STAGE_FRAGMENT_BIT) :
                    VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            auto info = vk_structure<VkDescriptorSetLayoutCreateInfo>(
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
            info.bindingCount = static_cast<uint32_t>(bindings.size());
            info.pBindings = bindings.data();
            return vk_ok(vkCreateDescriptorSetLayout(device, &info, nullptr,
                                                     &layout),
                         "vkCreateDescriptorSetLayout");
        };
        if (!make_set_layout(5, polygon_set_layout) ||
            !make_set_layout(3, text_set_layout))
            return false;

        auto layout_info = vk_structure<VkPipelineLayoutCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &polygon_set_layout;
        if (!vk_ok(vkCreatePipelineLayout(device, &layout_info, nullptr,
                                          &polygon_pipeline_layout),
                   "vkCreatePipelineLayout"))
            return false;
        layout_info.pSetLayouts = &text_set_layout;
        if (!vk_ok(vkCreatePipelineLayout(device, &layout_info, nullptr,
                                          &text_pipeline_layout),
                   "vkCreatePipelineLayout"))
            return false;

        std::array<VkDescriptorPoolSize, 2> sizes{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = 2;
        sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = 8;
        auto pool_info = vk_structure<VkDescriptorPoolCreateInfo>(
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        pool_info.maxSets = 2;
        pool_info.poolSizeCount = static_cast<uint32_t>(sizes.size());
        pool_info.pPoolSizes = sizes.data();
        if (!vk_ok(vkCreateDescriptorPool(device, &pool_info, nullptr,
                                          &scene_descriptor_pool),
                   "vkCreateDescriptorPool"))
            return false;
        const std::array<VkDescriptorSetLayout, 2> layouts{polygon_set_layout,
                                                           text_set_layout};
        std::array<VkDescriptorSet, 2> sets{};
        auto allocate = vk_structure<VkDescriptorSetAllocateInfo>(
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        allocate.descriptorPool = scene_descriptor_pool;
        allocate.descriptorSetCount = static_cast<uint32_t>(layouts.size());
        allocate.pSetLayouts = layouts.data();
        if (!vk_ok(vkAllocateDescriptorSets(device, &allocate, sets.data()),
                   "vkAllocateDescriptorSets"))
            return false;
        polygon_set = sets[0];
        text_set = sets[1];

        // draw_vertex in arcade_renderer.cpp is 25 consecutive floats laid out
        // in exactly this order, so the renderer's array uploads unmodified.
        namespace layout = system22_vertex_layout;
        static const std::array<VkVertexInputAttributeDescription, 14>
            attributes{{
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, layout::position},
                {1, 0, VK_FORMAT_R32_SFLOAT, layout::u_over_z},
                {2, 0, VK_FORMAT_R32_SFLOAT, layout::v_over_z},
                {3, 0, VK_FORMAT_R32_SFLOAT, layout::brightness_over_z},
                {4, 0, VK_FORMAT_R32_SFLOAT, layout::one_over_z},
                {5, 0, VK_FORMAT_R32G32_SFLOAT, layout::palette},
                {6, 0, VK_FORMAT_R32_SFLOAT, layout::color_mode},
                {7, 0, VK_FORMAT_R32_SFLOAT, layout::texture_bank},
                {8, 0, VK_FORMAT_R32_SFLOAT, layout::direct},
                {9, 0, VK_FORMAT_R32_SFLOAT, layout::fog},
                {10, 0, VK_FORMAT_R32G32B32_SFLOAT, layout::fog_color},
                {11, 0, VK_FORMAT_R32G32_SFLOAT, layout::viewport},
                {12, 0, VK_FORMAT_R32G32B32A32_SFLOAT, layout::clip_rect},
                {13, 0, VK_FORMAT_R32G32B32_SFLOAT, layout::render_flags},
            }};
        static VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = layout::stride;
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        auto polygon_input = vk_structure<VkPipelineVertexInputStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        polygon_input.vertexBindingDescriptionCount = 1;
        polygon_input.pVertexBindingDescriptions = &binding;
        polygon_input.vertexAttributeDescriptionCount =
            static_cast<uint32_t>(attributes.size());
        polygon_input.pVertexAttributeDescriptions = attributes.data();
        // The text layer is a screen-filling quad generated from the vertex
        // index, so it declares no vertex input at all.
        auto text_input = vk_structure<VkPipelineVertexInputStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);

        const VkShaderModule polygon_vertex = create_shader(
            system22_polygon_vert_spirv,
            sizeof(system22_polygon_vert_spirv));
        const VkShaderModule polygon_fragment = create_shader(
            system22_polygon_frag_spirv,
            sizeof(system22_polygon_frag_spirv));
        const VkShaderModule text_vertex = create_shader(
            fullscreen_quad_vert_spirv, sizeof(fullscreen_quad_vert_spirv));
        const VkShaderModule text_fragment = create_shader(
            system22_text_frag_spirv, sizeof(system22_text_frag_spirv));
        const bool modules_ready = polygon_vertex && polygon_fragment &&
            text_vertex && text_fragment;
        const bool built = modules_ready &&
            // No depth test: System 22 polygons arrive in the DSP's
            // priority order and are composited painter-style - the OpenGL
            // path draws them with GL_DEPTH_TEST disabled for exactly this
            // reason. A depth test re-orders by raw Z wherever the DSP's
            // priority disagrees with it, which garbles any scene that leans
            // on priority - Rave Racer's dense city constantly, Ridge
            // Racer's open track rarely enough to look correct.
            create_scene_pipeline(polygon_pipeline_layout, polygon_vertex,
                                  polygon_fragment, polygon_input, false,
                                  true, polygon_pipeline,
                                  VK_COMPARE_OP_LESS_OR_EQUAL, false,
                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) &&
            create_scene_pipeline(text_pipeline_layout, text_vertex,
                                  text_fragment, text_input, false, false,
                                  text_pipeline,
                                  VK_COMPARE_OP_LESS_OR_EQUAL, false,
                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
        for (VkShaderModule module : {polygon_vertex, polygon_fragment,
                                      text_vertex, text_fragment})
            if (module) vkDestroyShaderModule(device, module, nullptr);
        if (!built) return false;
        scene_pipelines_ready = true;
        return true;
    }

    bool create_model2_pipelines() {
        if (model2_pipelines_ready) return true;
        if (!create_scene_render_pass() || !ensure_scene_sheets()) return false;

        const auto make_layout = [&](uint32_t samplers,
                                     VkDescriptorSetLayout& layout) {
            std::vector<VkDescriptorSetLayoutBinding> bindings(samplers);
            for (uint32_t index = 0; index < samplers; ++index) {
                bindings[index].binding = index;
                bindings[index].descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bindings[index].descriptorCount = 1;
                bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            auto info = vk_structure<VkDescriptorSetLayoutCreateInfo>(
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
            info.bindingCount = samplers;
            info.pBindings = bindings.data();
            return vk_ok(vkCreateDescriptorSetLayout(device, &info, nullptr,
                                                     &layout),
                         "vkCreateDescriptorSetLayout");
        };
        if (!make_layout(3, model2_set_layout) ||
            !make_layout(1, model2_layer_set_layout))
            return false;

        // crtc_offset is two floats, comfortably inside the push-constant
        // space Vulkan guarantees, so Model 2 needs no uniform buffer.
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.size = sizeof(float) * 2;
        auto layout_info = vk_structure<VkPipelineLayoutCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &model2_set_layout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &range;
        if (!vk_ok(vkCreatePipelineLayout(device, &layout_info, nullptr,
                                          &model2_pipeline_layout),
                   "vkCreatePipelineLayout"))
            return false;
        layout_info.pSetLayouts = &model2_layer_set_layout;
        layout_info.pushConstantRangeCount = 0;
        layout_info.pPushConstantRanges = nullptr;
        if (!vk_ok(vkCreatePipelineLayout(device, &layout_info, nullptr,
                                          &model2_layer_pipeline_layout),
                   "vkCreatePipelineLayout"))
            return false;

        std::array<VkDescriptorPoolSize, 1> sizes{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[0].descriptorCount = 5;
        auto pool_info = vk_structure<VkDescriptorPoolCreateInfo>(
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        pool_info.maxSets = 3;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = sizes.data();
        if (!vk_ok(vkCreateDescriptorPool(device, &pool_info, nullptr,
                                          &model2_descriptor_pool),
                   "vkCreateDescriptorPool"))
            return false;
        const std::array<VkDescriptorSetLayout, 3> layouts{
            model2_set_layout, model2_layer_set_layout,
            model2_layer_set_layout};
        std::array<VkDescriptorSet, 3> sets{};
        auto allocate = vk_structure<VkDescriptorSetAllocateInfo>(
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        allocate.descriptorPool = model2_descriptor_pool;
        allocate.descriptorSetCount = static_cast<uint32_t>(layouts.size());
        allocate.pSetLayouts = layouts.data();
        if (!vk_ok(vkAllocateDescriptorSets(device, &allocate, sets.data()),
                   "vkAllocateDescriptorSets"))
            return false;
        model2_set = sets[0];
        model2_base_set = sets[1];
        model2_foreground_set = sets[2];

        namespace layout = model2_vertex_layout;
        static const std::array<VkVertexInputAttributeDescription, 12>
            attributes{{
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, layout::position},
                {1, 0, VK_FORMAT_R32G32_SFLOAT, layout::uv},
                {2, 0, VK_FORMAT_R32G32_SFLOAT, layout::center},
                {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, layout::clip},
                {4, 0, VK_FORMAT_R32_SFLOAT, layout::header0},
                {5, 0, VK_FORMAT_R32_SFLOAT, layout::header1},
                {6, 0, VK_FORMAT_R32_SFLOAT, layout::header2},
                {7, 0, VK_FORMAT_R32_SFLOAT, layout::color_base},
                {8, 0, VK_FORMAT_R32_SFLOAT, layout::luma},
                {9, 0, VK_FORMAT_R32_SFLOAT, layout::renderer},
                {10, 0, VK_FORMAT_R32_SFLOAT, layout::texture_lod},
                {11, 0, VK_FORMAT_R32_SFLOAT, layout::draw_priority},
            }};
        static VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = layout::stride;
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        auto polygon_input = vk_structure<VkPipelineVertexInputStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        polygon_input.vertexBindingDescriptionCount = 1;
        polygon_input.pVertexBindingDescriptions = &binding;
        polygon_input.vertexAttributeDescriptionCount =
            static_cast<uint32_t>(attributes.size());
        polygon_input.pVertexAttributeDescriptions = attributes.data();
        auto layer_input = vk_structure<VkPipelineVertexInputStateCreateInfo>(
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);

        const VkShaderModule polygon_vertex = create_shader(
            model2_polygon_vert_spirv, sizeof(model2_polygon_vert_spirv));
        const VkShaderModule polygon_fragment = create_shader(
            model2_polygon_frag_spirv, sizeof(model2_polygon_frag_spirv));
        const VkShaderModule layer_vertex = create_shader(
            fullscreen_quad_vert_spirv, sizeof(fullscreen_quad_vert_spirv));
        const VkShaderModule layer_fragment = create_shader(
            model2_overlay_frag_spirv, sizeof(model2_overlay_frag_spirv));
        const bool modules_ready = polygon_vertex && polygon_fragment &&
            layer_vertex && layer_fragment;
        // Depth is written per fragment from the polygon's draw priority and
        // compared with LESS, which is how Model 2's one-bit fill buffer is
        // emulated: the first polygon to claim a pixel keeps it.
        const bool built = modules_ready &&
            create_scene_pipeline(model2_pipeline_layout, polygon_vertex,
                                  polygon_fragment, polygon_input, true, false,
                                  model2_pipeline, VK_COMPARE_OP_LESS) &&
            create_scene_pipeline(model2_layer_pipeline_layout, layer_vertex,
                                  layer_fragment, layer_input, false, false,
                                  model2_base_pipeline,
                                  VK_COMPARE_OP_LESS_OR_EQUAL, false,
                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP) &&
            create_scene_pipeline(model2_layer_pipeline_layout, layer_vertex,
                                  layer_fragment, layer_input, false, false,
                                  model2_foreground_pipeline,
                                  VK_COMPARE_OP_LESS_OR_EQUAL, true,
                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
        for (VkShaderModule module : {polygon_vertex, polygon_fragment,
                                      layer_vertex, layer_fragment})
            if (module) vkDestroyShaderModule(device, module, nullptr);
        if (!built) return false;
        model2_pipelines_ready = true;
        return true;
    }

    void refresh_model2_descriptors() {
        if (model2_descriptors_valid || !model2_set) return;
        const std::array<int, 5> bound{
            sheet_model2_sheets, sheet_model2_luma, sheet_model2_color,
            sheet_model2_base, sheet_model2_foreground};
        std::array<VkDescriptorImageInfo, 5> images{};
        std::array<VkWriteDescriptorSet, 5> writes{};
        for (std::size_t index = 0; index < bound.size(); ++index) {
            images[index].sampler = nearest_sampler;
            images[index].imageView =
                scene_sheets[static_cast<std::size_t>(bound[index])].view;
            images[index].imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[index] = vk_structure<VkWriteDescriptorSet>(
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            writes[index].dstSet = index < 3 ? model2_set :
                (index == 3 ? model2_base_set : model2_foreground_set);
            writes[index].dstBinding =
                index < 3 ? static_cast<uint32_t>(index) : 0u;
            writes[index].descriptorCount = 1;
            writes[index].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[index].pImageInfo = &images[index];
        }
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
        model2_descriptors_valid = true;
    }

    // Base 2D layer, then polygons, then the alpha-blended foreground layer -
    // the order the OpenGL path draws them in.
    void record_model2_scene_pass(VkCommandBuffer commands) {
        VkClearValue clears[2]{};
        clears[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        clears[1].depthStencil = {1.0f, 0};
        auto pass = vk_structure<VkRenderPassBeginInfo>(
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        pass.renderPass = scene_render_pass;
        pass.framebuffer = scene_framebuffer;
        pass.renderArea.extent = {
            static_cast<uint32_t>(scene_image.width),
            static_cast<uint32_t>(scene_image.height)};
        pass.clearValueCount = 2;
        pass.pClearValues = clears;
        vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport{};
        viewport.width = static_cast<float>(scene_image.width);
        viewport.height = static_cast<float>(scene_image.height);
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.extent = pass.renderArea.extent;
        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          model2_base_pipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                model2_layer_pipeline_layout, 0, 1,
                                &model2_base_set, 0, nullptr);
        vkCmdDraw(commands, 4, 1, 0, 0);

        if (model2_vertex_count > 0) {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              model2_pipeline);
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    model2_pipeline_layout, 0, 1, &model2_set,
                                    0, nullptr);
            vkCmdPushConstants(commands, model2_pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(model2_crtc_offset), model2_crtc_offset);
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(commands, 0, 1, &vertex_buffer, &offset);
            vkCmdDraw(commands, static_cast<uint32_t>(model2_vertex_count), 1,
                      0, 0);
        }

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          model2_foreground_pipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                model2_layer_pipeline_layout, 0, 1,
                                &model2_foreground_set, 0, nullptr);
        vkCmdDraw(commands, 4, 1, 0, 0);
        vkCmdEndRenderPass(commands);
    }

    void refresh_scene_descriptors() {
        if (scene_descriptors_valid || !polygon_set) return;
        VkDescriptorBufferInfo polygon_buffer{};
        polygon_buffer.buffer = polygon_uniform_buffer;
        polygon_buffer.range = sizeof(system22_uniform_block);
        VkDescriptorBufferInfo text_buffer{};
        text_buffer.buffer = text_uniform_buffer;
        text_buffer.range = sizeof(system22_text_uniform_block);
        // The text layer samples character data, the tile map and the shared
        // palette; the polygon layer samples all five sheets.
        const std::array<int, 5> polygon_sheets{
            sheet_texture_tiles, sheet_texture_map, sheet_texture_attr,
            sheet_palette, sheet_sprite_tiles};
        // system22_text.frag declares character_data and text_map as plain
        // usampler2D. The polygon sheets are layered arrays, so binding those
        // here would be an image-view-type mismatch.
        const std::array<int, 3> text_sheets{
            sheet_character_data, sheet_text_map, sheet_palette};
        std::vector<VkDescriptorImageInfo> images;
        images.reserve(polygon_sheets.size() + text_sheets.size());
        std::vector<VkWriteDescriptorSet> writes;
        const auto add_samplers = [&](VkDescriptorSet set, const int* sheets,
                                      std::size_t count) {
            for (std::size_t index = 0; index < count; ++index) {
                VkDescriptorImageInfo info{};
                info.sampler = nearest_sampler;
                info.imageView =
                    scene_sheets[static_cast<std::size_t>(sheets[index])].view;
                info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                images.push_back(info);
                auto write = vk_structure<VkWriteDescriptorSet>(
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
                write.dstSet = set;
                write.dstBinding = static_cast<uint32_t>(index) + 1;
                write.descriptorCount = 1;
                write.descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes.push_back(write);
            }
        };
        auto polygon_write = vk_structure<VkWriteDescriptorSet>(
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        polygon_write.dstSet = polygon_set;
        polygon_write.descriptorCount = 1;
        polygon_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        polygon_write.pBufferInfo = &polygon_buffer;
        writes.push_back(polygon_write);
        auto text_write = polygon_write;
        text_write.dstSet = text_set;
        text_write.pBufferInfo = &text_buffer;
        writes.push_back(text_write);
        add_samplers(polygon_set, polygon_sheets.data(),
                     polygon_sheets.size());
        add_samplers(text_set, text_sheets.data(), text_sheets.size());
        std::size_t image_index = 0;
        for (VkWriteDescriptorSet& write : writes)
            if (write.descriptorType ==
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                write.pImageInfo = &images[image_index++];
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
        scene_descriptors_valid = true;
    }

    void destroy_scene_resources() {
        for (device_image& sheet : scene_sheets) destroy_image(sheet);
        for (sheet_stream& stream : sheet_streaming) {
            destroy_host_buffer(stream.buffer, stream.memory, stream.mapped);
            stream.pending = false;
        }
        destroy_host_buffer(vertex_buffer, vertex_memory, vertex_mapped);
        vertex_capacity = 0;
        destroy_host_buffer(polygon_uniform_buffer, polygon_uniform_memory,
                            polygon_uniform_mapped);
        destroy_host_buffer(text_uniform_buffer, text_uniform_memory,
                            text_uniform_mapped);
        if (scene_descriptor_pool)
            vkDestroyDescriptorPool(device, scene_descriptor_pool, nullptr);
        scene_descriptor_pool = VK_NULL_HANDLE;
        polygon_set = text_set = VK_NULL_HANDLE;
        for (VkPipeline* pipeline : {&polygon_pipeline, &text_pipeline}) {
            if (*pipeline) vkDestroyPipeline(device, *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
        }
        for (VkPipelineLayout* layout :
             {&polygon_pipeline_layout, &text_pipeline_layout}) {
            if (*layout) vkDestroyPipelineLayout(device, *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
        for (VkDescriptorSetLayout* layout :
             {&polygon_set_layout, &text_set_layout}) {
            if (*layout)
                vkDestroyDescriptorSetLayout(device, *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
        for (VkPipeline* pipeline : {&model2_pipeline, &model2_base_pipeline,
                                     &model2_foreground_pipeline}) {
            if (*pipeline) vkDestroyPipeline(device, *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
        }
        for (VkPipelineLayout* layout : {&model2_pipeline_layout,
                                         &model2_layer_pipeline_layout}) {
            if (*layout) vkDestroyPipelineLayout(device, *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
        for (VkDescriptorSetLayout* layout : {&model2_set_layout,
                                              &model2_layer_set_layout}) {
            if (*layout)
                vkDestroyDescriptorSetLayout(device, *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
        if (model2_descriptor_pool)
            vkDestroyDescriptorPool(device, model2_descriptor_pool, nullptr);
        model2_descriptor_pool = VK_NULL_HANDLE;
        model2_set = model2_base_set = model2_foreground_set = VK_NULL_HANDLE;
        model2_pipelines_ready = false;
        model2_descriptors_valid = false;
        scene_pipelines_ready = false;
        scene_descriptors_valid = false;
    }

    // Sizes the scene colour and depth images to the board raster and builds
    // the framebuffer the polygon pipelines draw into.
    bool ensure_scene_target(int width, int height) {
        if (!create_scene_render_pass()) {
            static int noted = 0;
            if (whitty_wall_log::first(noted))
                whitty_wall_log::note("scene target: render pass failed");
            return false;
        }
        if (scene_image.width == width && scene_image.height == height &&
            scene_framebuffer)
            return true;
        vkDeviceWaitIdle(device);
        if (scene_framebuffer)
            vkDestroyFramebuffer(device, scene_framebuffer, nullptr);
        scene_framebuffer = VK_NULL_HANDLE;
        if (!create_image(scene_image, width, height, 1,
                          VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
            !create_image(scene_depth, width, height, 1, scene_depth_format,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT)) {
            static int noted = 0;
            if (whitty_wall_log::first(noted))
                whitty_wall_log::note("scene target: image creation failed");
            return false;
        }
        descriptors_valid = false;
        // No blank pre-fill: the scene image is only sampled after the scene
        // render pass has drawn it in the same frame (the rasterise call that
        // sizes this target also queues the draw), so the render pass's own
        // clear defines the image. The pre-fill this replaced was a one-shot
        // submit that could fail and take the whole native path down with it.
        const std::array<VkImageView, 2> views{scene_image.view,
                                               scene_depth.view};
        auto info = vk_structure<VkFramebufferCreateInfo>(
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
        info.renderPass = scene_render_pass;
        info.attachmentCount = static_cast<uint32_t>(views.size());
        info.pAttachments = views.data();
        info.width = static_cast<uint32_t>(width);
        info.height = static_cast<uint32_t>(height);
        info.layers = 1;
        return vk_ok(vkCreateFramebuffer(device, &info, nullptr,
                                         &scene_framebuffer),
                     "vkCreateFramebuffer");
    }

    // Records the offscreen System 22 pass. Called at the head of the
    // presentation command buffer so the scene and the frame that shows it
    // are one submission.
    void record_scene_pass(VkCommandBuffer commands) {
        VkClearValue clears[2]{};
        clears[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
        clears[1].depthStencil = {1.0f, 0};
        auto pass = vk_structure<VkRenderPassBeginInfo>(
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        pass.renderPass = scene_render_pass;
        pass.framebuffer = scene_framebuffer;
        pass.renderArea.extent = {
            static_cast<uint32_t>(scene_image.width),
            static_cast<uint32_t>(scene_image.height)};
        pass.clearValueCount = 2;
        pass.pClearValues = clears;
        vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.width = static_cast<float>(scene_image.width);
        viewport.height = static_cast<float>(scene_image.height);
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.extent = pass.renderArea.extent;
        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);

        if (pending_vertex_count > 0) {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              polygon_pipeline);
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    polygon_pipeline_layout, 0, 1,
                                    &polygon_set, 0, nullptr);
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(commands, 0, 1, &vertex_buffer, &offset);
            vkCmdDraw(commands,
                      static_cast<uint32_t>(pending_vertex_count), 1, 0, 0);
        }
        // The text layer covers the screen and decides per pixel whether it
        // has anything to draw, so it needs no geometry of its own.
        if (pending_draw_text) {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              text_pipeline);
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    text_pipeline_layout, 0, 1, &text_set, 0,
                                    nullptr);
            vkCmdDraw(commands, 4, 1, 0, 0);
        }
        vkCmdEndRenderPass(commands);
    }


    // Where the bezel artwork sits inside a pane, and where the game goes
    // inside that. Returns false when there is no bezel, leaving the caller's
    // ordinary letterbox fit untouched.
    bool bezel_layout(const present_pane& pane, int surface_h,
                      fitted_viewport& artwork, fitted_viewport& game) const {
        const int bezel_width = bezel.width;
        const int bezel_height = bezel.height;
        const fitted_viewport& bezel_cutout = bezel.cutout;
        if (bezel_width <= 0 || bezel_height <= 0 ||
            bezel_cutout.w <= 0 || bezel_cutout.h <= 0)
            return false;
        // Largest aspect-correct fit of the artwork in the pane.
        int width = pane.w;
        int height = width * bezel_height / std::max(bezel_width, 1);
        if (height > surface_h) {
            height = surface_h;
            width = height * bezel_width / std::max(bezel_height, 1);
        }
        artwork = {pane.x + (pane.w - width) / 2, (surface_h - height) / 2,
                   std::max(width, 1), std::max(height, 1)};
        // The opening scales with it.
        const auto scale_x = [&](int value) {
            return value * artwork.w / std::max(bezel_width, 1);
        };
        const auto scale_y = [&](int value) {
            return value * artwork.h / std::max(bezel_height, 1);
        };
        game = {artwork.x + scale_x(bezel_cutout.x),
                artwork.y + scale_y(bezel_cutout.y),
                std::max(scale_x(bezel_cutout.w), 1),
                std::max(scale_y(bezel_cutout.h), 1)};
        return true;
    }

    void draw_overlay_slot(VkCommandBuffer commands, std::size_t index,
                           const fitted_viewport& rect) {
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          overlay_pipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                overlay_pipeline_layout, 0, 1,
                                &overlays[index].set, 0, nullptr);
        draw_quad(commands, rect);
    }

    // The native path: board pixels straight into a sampled image, then drawn
    // by the post-process pipeline. Returns false when the path is not
    // available, which tells the caller to use the OpenGL route instead.

    // A null pixels pointer repeats the frame already resident in the scene
    // image, which is how a paused board redraws without the emulator
    // producing anything new.
    bool render_board_frame(const uint8_t* pixels, int width, int height,
                            bool flip_y, const board_overlays& panels,
                            bool menu_visible, int display_width,
                            int display_height) {
        if (native_failed) {
            whitty_wall_log::note("native path already gave up");
            return false;
        }
        whitty_wall_log::begin(settings.wall_slot, settings.wall_count);
        count_present_attempt();
        const bool upload_scene = pixels != nullptr;
        if (upload_scene) {
            if (width <= 0 || height <= 0) return false;
            board_flip_y = flip_y;
            board_display_width = display_width;
            board_display_height = display_height;
            board_menu_visible = menu_visible;
        } else {
            if (!have_board_frame || !scene_image.image)
                {
                whitty_wall_log::note("declined: no retained scene");
                return decline("no retained scene to present");
            }
            width = scene_image.width;
            height = scene_image.height;
            flip_y = board_flip_y;
            display_width = board_display_width;
            display_height = board_display_height;
            menu_visible = board_menu_visible;
        }
        int drawable_width = 0;
        int drawable_height = 0;
        SDL_GetWindowSizeInPixels(window, &drawable_width, &drawable_height);
        if (swapchain && drawable_width > 0 && drawable_height > 0 &&
            (swapchain_extent.width !=
                 static_cast<uint32_t>(drawable_width) ||
             swapchain_extent.height !=
                 static_cast<uint32_t>(drawable_height)))
            swapchain_dirty = true;
        if (swapchain_dirty && !create_swapchain()) {
            whitty_wall_log::note("declined: swapchain not created");
            return decline("swapchain could not be created");
        }
        // A wall column that renders into the wrong part of its window, or
        // never appears at all, is indistinguishable from the outside. Report
        // what this column actually got, once and again whenever it changes,
        // so a bad wall says which column and how it differs.
        if (settings.wall_count > 1 &&
            (reported_extent.width != swapchain_extent.width ||
             reported_extent.height != swapchain_extent.height ||
             reported_drawable[0] != drawable_width ||
             reported_drawable[1] != drawable_height)) {
            reported_extent = swapchain_extent;
            reported_drawable[0] = drawable_width;
            reported_drawable[1] = drawable_height;
            whitty_wall_log::note("window %dx%d, swapchain %ux%u",
                                  drawable_width, drawable_height,
                                  swapchain_extent.width,
                                  swapchain_extent.height);
        }
        if (!create_native_path()) {
            whitty_wall_log::note("declined: native path unavailable");
            return decline("native path unavailable");
        }

        // Shared with the polygon path, so it is always built as a render
        // target even when this frame only copies into it.
        if (upload_scene && !ensure_scene_target(width, height))
            return native_give_up();
        if (scene_uses_linear != settings.linear_filtering) {
            scene_uses_linear = settings.linear_filtering;
            descriptors_valid = false;
        }
        // Gather the panels this frame supplies. A slot the caller leaves null
        // keeps whatever it last held, which is what lets a repeat redraw the
        // same furniture without the renderer rebuilding any of it.
        struct pending_overlay {
            const uint8_t* pixels{};
            int width{};
            int height{};
            VkDeviceSize offset{};
        };
        const std::array<pending_overlay, overlay_count> supplied{{
            {panels.status, panels.status_width, panels.status_height, 0},
            {panels.menu, panels.menu_width, panels.menu_height, 0},
            {panels.player_label[0], panels.player_label_width[0],
             panels.player_label_height[0], 0},
            {panels.player_label[1], panels.player_label_width[1],
             panels.player_label_height[1], 0},
        }};
        // Only an uploaded frame needs room reserved ahead of the panels.
        const VkDeviceSize scene_bytes = upload_scene ?
            static_cast<VkDeviceSize>(width) * height * 4 : 0;
        std::array<pending_overlay, overlay_count> uploads{};
        VkDeviceSize upload_bytes = scene_bytes;
        for (std::size_t index = 0; index < overlay_count; ++index) {
            const pending_overlay& panel = supplied[index];
            if (!panel.pixels || panel.width <= 0 || panel.height <= 0)
                continue;
            overlay_slot& slot = overlays[index];
            if (panel.width == 1 && panel.height == 1) {
                // The caller blanks a panel by handing over a single
                // transparent texel. Drop the slot instead of uploading and
                // blending it for every frame from here on.
                slot.resident = false;
                continue;
            }
            if (slot.image.width != panel.width ||
                slot.image.height != panel.height) {
                vkDeviceWaitIdle(device);
                if (!create_image(slot.image, panel.width, panel.height, 1,
                                  VK_FORMAT_R8G8B8A8_UNORM))
                    return native_give_up();
                descriptors_valid = false;
            }
            uploads[index] = {panel.pixels, panel.width, panel.height,
                              upload_bytes};
            upload_bytes += static_cast<VkDeviceSize>(panel.width) *
                            panel.height * 4;
            slot.resident = true;
        }
        if (!create_upload_buffer(std::max<VkDeviceSize>(upload_bytes, 4)))
            return native_give_up();

        if (!wait_frame_fence("native present")) return true;
        uint32_t image_index = 0;
        const VkResult acquired = vkAcquireNextImageKHR(
            device, swapchain, frame_wait_ns, image_available, VK_NULL_HANDLE,
            &image_index);
        if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY) {
            // The compositor has stopped serving this surface - occluded, or
            // mid-shuffle while its neighbours are re-placed. Drop the frame
            // and rebuild; blocking forever here is how a wall column froze
            // while its game played on.
            whitty_wall_log::note("native present: acquire starved - "
                                  "rebuilding swapchain");
            swapchain_dirty = true;
            return true;
        }
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            static int noted = 0;
            if (whitty_wall_log::first(noted, 6))
                whitty_wall_log::note("native present: acquire OUT_OF_DATE");
            swapchain_dirty = true;
            return true;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            whitty_wall_log::note("native present: acquire failed (%d) - "
                                  "rebuilding swapchain",
                                  static_cast<int>(acquired));
            // A failed acquire can leave the signal semaphore pending - the
            // driver then refuses every later acquire with VK_NOT_READY, so
            // without a rebuild this failure repeats forever.
            swapchain_dirty = true;
            return vk_ok(acquired, "vkAcquireNextImageKHR") ? true : false;
        }
        if (image_index >= framebuffers.size()) return native_give_up();
        vkResetFences(device, 1, &frame_fence);

        // Safe to touch host-visible memory and descriptors now that the
        // previous frame has retired.
        auto* upload = static_cast<uint8_t*>(upload_mapped);
        if (upload_scene) std::memcpy(upload, pixels, scene_bytes);
        for (const pending_overlay& panel : uploads) {
            if (!panel.pixels) continue;
            std::memcpy(upload + panel.offset, panel.pixels,
                        static_cast<std::size_t>(panel.width) * panel.height *
                            4);
        }
        refresh_descriptors();

        vkResetCommandBuffer(command_buffer, 0);
        auto begin = vk_structure<VkCommandBufferBeginInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(command_buffer, &begin);
        // A polygon board rasterises its scene here; an RGBA board copies one
        // in. Either way the post pass below sees the same image.
        for (int index = 0; index < scene_sheet_count; ++index) {
            sheet_stream& stream =
                sheet_streaming[static_cast<std::size_t>(index)];
            if (!stream.pending) continue;
            record_image_upload_from(
                command_buffer, scene_sheets[static_cast<std::size_t>(index)],
                stream.buffer, 0);
            stream.pending = false;
        }
        if (pending_scene) {
            record_scene_pass(command_buffer);
            pending_scene = false;
        } else if (model2_pending) {
            record_model2_scene_pass(command_buffer);
            model2_pending = false;
        }
        if (upload_scene) record_image_upload(command_buffer, scene_image, 0);
        for (std::size_t index = 0; index < overlay_count; ++index) {
            if (!uploads[index].pixels) continue;
            record_image_upload(command_buffer, overlays[index].image,
                                uploads[index].offset);
        }

        VkClearValue clear{};
        clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        auto pass = vk_structure<VkRenderPassBeginInfo>(
            VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        pass.renderPass = render_pass;
        pass.framebuffer = framebuffers[image_index];
        pass.renderArea.extent = swapchain_extent;
        pass.clearValueCount = 1;
        pass.pClearValues = &clear;
        vkCmdBeginRenderPass(command_buffer, &pass,
                             VK_SUBPASS_CONTENTS_INLINE);

        // The window exists by now, so a wall column that could not be placed
        // at start-up gets another go. Spaced out because each attempt spawns
        // a compositor command.
        if (settings.wall_count > 1) {
            if (--wall_place_countdown <= 0) {
                wall_place_countdown = 30;
                // Re-placed when the cabinet being played changes, because
                // every column's position depends on which one is the big
                // one - not only the column that gained or lost focus.
                const int focused = whitty_window::read_wall_focus();
                // Checked every tick, not once: a sibling column opening
                // re-tiles the ones already up, so "placed" is a state to be
                // maintained rather than a step to be completed.
                bool drifted = false;
                if (wall_placed) {
                    int got_x = 0, got_y = 0, got_w = 0, got_h = 0;
                    if (whitty_window::compositor_window_geometry(got_x, got_y,
                                                                  got_w, got_h))
                        drifted = got_w != placed_width ||
                                  got_h != placed_height ||
                                  got_x != placed_x;
                }
                if (!wall_placed || drifted || focused != wall_focus_applied) {
                    wall_focus_applied = focused;
                    const bool took = whitty_window::place_wall_slot(
                        window, settings.wall_slot, settings.wall_count);
                    if (took != wall_placed || !took)
                        whitty_wall_log::note("placement %s (playing %d)",
                                              took ? "took" : "not yet",
                                              focused);
                    wall_placed = took;
                    if (took)
                        whitty_window::compositor_window_geometry(
                            placed_x, placed_y, placed_width, placed_height);
                }
            }
        }

        // Linked cabinets sharing one screen hold their grid cells the same
        // way a wall column holds its slot: re-checked every 30 presents,
        // because the compositor does not know a window at start-up and a
        // sibling cabinet opening re-tiles the ones already up.
        if (settings.twin_one_screen) {
            if (--cabinet_place_countdown <= 0) {
                cabinet_place_countdown = 30;
                if (!cabinet_placed) {
                    static const int node = [] {
                        const char* text =
                            std::getenv("WHITTY_CABINET_NODE");
                        const int value = text ? std::atoi(text) : 1;
                        return value >= 1 ? value : 1;
                    }();
                    static const int count = [] {
                        const char* text =
                            std::getenv("WHITTY_CABINET_COUNT");
                        const int value = text ? std::atoi(text) : 2;
                        return value >= 2 ? value : 2;
                    }();
                    const bool took = whitty_window::place_cabinet_cell(
                        window, node, count);
                    if (took != cabinet_placed || !took)
                        whitty_wall_log::note("cabinet cell %d/%d %s",
                                              node, count,
                                              took ? "took" : "not yet");
                    cabinet_placed = took;
                }
            }
        }

        const int surface_w = static_cast<int>(swapchain_extent.width);
        const int surface_h = static_cast<int>(swapchain_extent.height);
        const int aspect_width = display_width > 0 ? display_width : width;
        const int aspect_height = display_height > 0 ? display_height : height;
        // An arcade wall is one fullscreen surface divided into a column per
        // game - the same mechanism as twin display, just wider.
        std::array<present_pane, max_panes> panes{};
        const int pane_count = layout_wall_panes(
            surface_w,
            wanted_wall_panes(settings, max_panes, menu_visible),
            panes.data(), max_panes);
        const bool framed =
            settings.output == output_mode::dual && !menu_visible;
        for (int pane = 0; pane < pane_count; ++pane) {
            // A cabinet bezel replaces the plain letterbox: the game fills the
            // artwork's opening instead of the pane. Without one this is the
            // ordinary fit, unchanged.
            fitted_viewport bezel_art{};
            fitted_viewport bezel_game{};
            const bool bezelled =
                bezel_layout(panes[pane], surface_h, bezel_art, bezel_game);
            const fitted_viewport fit = bezelled ? bezel_game :
                fit_arcade_viewport(
                    panes[pane].x, panes[pane].w, surface_h, aspect_width,
                    aspect_height, width, height, settings.integer_scaling,
                    settings.fullscreen, framed);
            if (framed && !bezelled) {
                vkCmdBindPipeline(command_buffer,
                                  VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  solid_pipeline);
                const std::array<float, 4> surround{
                    13.0f / 255.0f, 29.0f / 255.0f, 39.0f / 255.0f, 1.0f};
                vkCmdPushConstants(command_buffer, solid_pipeline_layout,
                                   VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(surround), surround.data());
                draw_quad(command_buffer, expanded(fit, 10));
                const std::array<float, 4> accent{
                    56.0f / 255.0f, 198.0f / 255.0f, 1.0f, 1.0f};
                vkCmdPushConstants(command_buffer, solid_pipeline_layout,
                                   VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(accent), accent.data());
                draw_quad(command_buffer, expanded(fit, 3));
            }
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              post_pipeline);
            vkCmdBindDescriptorSets(command_buffer,
                                    VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    post_pipeline_layout, 0, 1,
                                    &post_set,
                                    0, nullptr);
            post_push_constants constants{};
            constants.flip_y = flip_y ? 1u : 0u;
            constants.linear_filtering = settings.linear_filtering ? 1u : 0u;
            constants.apply_board_color = board_apply_color ? 1u : 0u;
            constants.super_system22 = board_super_system22 ? 1u : 0u;
            constants.screen_fade[0] = board_screen_fade[0];
            constants.screen_fade[1] = board_screen_fade[1];
            constants.screen_fade[2] = board_screen_fade[2];
            constants.pane_width = static_cast<float>(std::max(fit.w, 1));
            constants.pane_height = static_cast<float>(std::max(fit.h, 1));
            vkCmdPushConstants(command_buffer, post_pipeline_layout,
                               VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(constants), &constants);
            draw_quad(command_buffer, fit);
            // The artwork goes over the game: its opening is transparent, so
            // the picture shows through and the surround covers the rest.
            if (bezelled && overlays[overlay_bezel].resident)
                draw_overlay_slot(command_buffer, overlay_bezel, bezel_art);
            // The cabinet label belongs to its pane, sitting inside the
            // bottom-left corner exactly as the OpenGL path places it. There
            // are only two labels - Player 1 and Player 2 - so this is a twin
            // cabinet's furniture, not a wall's: past pane 1 the index would
            // run into the bezel slots.
            if (pane_count == 2) {
                const std::size_t label = overlay_label_one +
                    static_cast<std::size_t>(pane);
                const overlay_slot& label_slot = overlays[label];
                if (label_slot.resident) {
                    constexpr int inset = 16;
                    draw_overlay_slot(
                        command_buffer, label,
                        {panes[pane].x + inset,
                         surface_h - inset - label_slot.image.height,
                         label_slot.image.width, label_slot.image.height});
                }
            }
        }

        // Frame rate and cabinet status, inset from the top-left corner.
        if (overlays[overlay_status].resident) {
            constexpr int margin = 12;
            const device_image& panel = overlays[overlay_status].image;
            draw_overlay_slot(command_buffer, overlay_status,
                              {margin, margin, panel.width, panel.height});
        }
        // Settings and ROM menus are host UI: centred, pinned to the top edge
        // and scaled down only far enough to fit a small window.
        if (overlays[overlay_menu].resident) {
            const device_image& panel = overlays[overlay_menu].image;
            const float scale = std::min(
                1.0f,
                std::min(static_cast<float>(surface_w) /
                             static_cast<float>(std::max(panel.width, 1)),
                         static_cast<float>(surface_h) /
                             static_cast<float>(std::max(panel.height, 1))));
            const int menu_w =
                std::max(1, static_cast<int>(panel.width * scale));
            const int menu_h =
                std::max(1, static_cast<int>(panel.height * scale));
            draw_overlay_slot(command_buffer, overlay_menu,
                              {(surface_w - menu_w) / 2, 0, menu_w, menu_h});
        }
        vkCmdEndRenderPass(command_buffer);
        if (!vk_ok(vkEndCommandBuffer(command_buffer), "vkEndCommandBuffer"))
            return native_give_up();

        const VkPipelineStageFlags wait_stage =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        auto submit = vk_structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &image_available;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &render_finished[image_index];
        if (!vk_ok(vkQueueSubmit(queue, 1, &submit, frame_fence),
                   "vkQueueSubmit"))
            return native_give_up();
        auto present = vk_structure<VkPresentInfoKHR>(
            VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_finished[image_index];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &image_index;
        const VkResult result = vkQueuePresentKHR(queue, &present);
        if (title_capture_countdown > 0 && !title_capture_name.empty() &&
            --title_capture_countdown == 0) {
            // The scene image is this frame's finished game picture,
            // before bezels and overlays; one blocking readback at a
            // single moment of the session is invisible.
            title_capture_due = true;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR ||
            result == VK_SUBOPTIMAL_KHR) {
            swapchain_dirty = true;
            return true;
        }
        if (vk_ok(result, "vkQueuePresentKHR")) {
            have_board_frame = true;
            if (first_present) {
                whitty_wall_log::note("first present succeeded - showing");
                SDL_ShowWindow(window);
                first_present = false;
            }
        } else {
            whitty_wall_log::note("vkQueuePresentKHR failed (%d)",
                                  static_cast<int>(result));
        }
        return true;
    }

    bool present_vulkan(const uint8_t* pixels, int width, int height,
                        const uint8_t* overlay, int overlay_width,
                        int overlay_height, bool menu_visible,
                        int display_width, int display_height) {
        // SDL's Wayland fullscreen transition does not necessarily make the
        // first acquire return VK_ERROR_OUT_OF_DATE_KHR. The compositor may
        // instead scale the original window-sized swapchain, which stretches
        // X and Y independently on an ultrawide display. Compare against the
        // live drawable every frame and rebuild before presenting.
        static int bridge_note = 0;
        if (whitty_wall_log::first(bridge_note))
            whitty_wall_log::note("presenting via transfer bridge %dx%d",
                                  width, height);
        count_present_attempt();
        int drawable_width = 0;
        int drawable_height = 0;
        SDL_GetWindowSizeInPixels(window, &drawable_width, &drawable_height);
        if (swapchain && drawable_width > 0 && drawable_height > 0 &&
            (swapchain_extent.width !=
                 static_cast<uint32_t>(drawable_width) ||
             swapchain_extent.height !=
                 static_cast<uint32_t>(drawable_height)))
            swapchain_dirty = true;
        if (swapchain_dirty && !create_swapchain()) {
            static int fail_note = 0;
            if (whitty_wall_log::first(fail_note))
                whitty_wall_log::note("bridge: swapchain not created");
            return true;
        }
        if (!wait_frame_fence("bridge")) return true;
        uint32_t image_index = 0;
        VkResult acquired = vkAcquireNextImageKHR(
            device, swapchain, frame_wait_ns, image_available, VK_NULL_HANDLE,
            &image_index);
        if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY) {
            whitty_wall_log::note("bridge: acquire starved - rebuilding "
                                  "swapchain");
            swapchain_dirty = true;
            return true;
        }
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            static int noted = 0;
            if (whitty_wall_log::first(noted, 6))
                whitty_wall_log::note("bridge: acquire OUT_OF_DATE");
            swapchain_dirty = true;
            return true;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            static int noted = 0;
            if (whitty_wall_log::first(noted, 6))
                whitty_wall_log::note("bridge: acquire failed (%d) - "
                                      "rebuilding swapchain",
                                      static_cast<int>(acquired));
            // Same reasoning as the native path: a pending acquire semaphore
            // only clears with the swapchain rebuild that recreates it.
            swapchain_dirty = true;
            return vk_ok(acquired, "vkAcquireNextImageKHR");
        }
        vkResetFences(device, 1, &frame_fence);

        const std::size_t destination_pitch =
            static_cast<std::size_t>(swapchain_extent.width) * 4;
        if (!staging_mapped) {
            // Bailing after a successful acquire leaves its semaphore
            // pending; only the rebuild that recreates the semaphores can
            // recover, so ask for one rather than poisoning every later
            // acquire.
            swapchain_dirty = true;
            return false;
        }
        auto* destination = static_cast<uint8_t*>(staging_mapped);
        std::memset(destination, 0,
                    destination_pitch * swapchain_extent.height);
        const bool bgra = swapchain_format == VK_FORMAT_B8G8R8A8_UNORM ||
                          swapchain_format == VK_FORMAT_B8G8R8A8_SRGB;
        const int surface_w = static_cast<int>(swapchain_extent.width);
        const int surface_h = static_cast<int>(swapchain_extent.height);
        const int aspect_width =
            display_width > 0 ? display_width : width;
        const int aspect_height =
            display_height > 0 ? display_height : height;
        // One pane, or two side-by-side panes for fullscreen dual output; each
        // letterboxes the arcade frame within its own half of the swapchain.
        // An arcade wall is one fullscreen surface divided into a column per
        // game - the same mechanism as twin display, just wider.
        std::array<present_pane, max_panes> panes{};
        const int pane_count = layout_wall_panes(
            surface_w,
            wanted_wall_panes(settings, max_panes, menu_visible),
            panes.data(), max_panes);
        for (int pane = 0; pane < pane_count; ++pane) {
            const bool framed = settings.output == output_mode::dual &&
                !menu_visible;
            const fitted_viewport fit = fit_arcade_viewport(
                panes[pane].x, panes[pane].w, surface_h,
                aspect_width, aspect_height, width, height,
                settings.integer_scaling, settings.fullscreen, framed);
            if (framed) {
                fill_host_rect(destination, surface_w, surface_h,
                               destination_pitch, expanded(fit, 10), bgra,
                               13, 29, 39);
                fill_host_rect(destination, surface_w, surface_h,
                               destination_pitch, expanded(fit, 3), bgra,
                               56, 198, 255);
            }
            // The nearest-neighbour source column for a given output column
            // depends only on the pane geometry, so hoist the divide out of
            // the inner loop and move whole pixels as 32-bit words. Measured
            // on the 496x384 Model 2 raster this is ~5x the throughput of a
            // per-pixel divide with byte stores, which is the difference
            // between fitting in a 60 Hz frame at 1440p and not.
            column_offsets.resize(static_cast<std::size_t>(fit.w));
            for (int x = 0; x < fit.w; ++x)
                column_offsets[static_cast<std::size_t>(x)] =
                    x * width / std::max(fit.w, 1);
            for (int y = 0; y < fit.h; ++y) {
                const int source_y = height - 1 -
                    (y * height / std::max(fit.h, 1));
                const auto* source = reinterpret_cast<const uint32_t*>(
                    pixels + (static_cast<std::size_t>(source_y) * width) * 4);
                auto* row = reinterpret_cast<uint32_t*>(
                    destination + (static_cast<std::size_t>(y + fit.y) *
                                   swapchain_extent.width + fit.x) * 4);
                if (bgra) {
                    for (int x = 0; x < fit.w; ++x) {
                        // RGBA -> BGRA: swap the red and blue lanes and keep
                        // green and alpha where they are.
                        const uint32_t rgba =
                            source[column_offsets[static_cast<std::size_t>(x)]];
                        row[x] = (rgba & 0xff00ff00u) |
                                 ((rgba & 0x00ff0000u) >> 16) |
                                 ((rgba & 0x000000ffu) << 16);
                    }
                } else {
                    for (int x = 0; x < fit.w; ++x)
                        row[x] =
                            source[column_offsets[static_cast<std::size_t>(x)]];
                }
            }
        }
        composite_status_overlay(
            destination, static_cast<int>(swapchain_extent.width),
            static_cast<int>(swapchain_extent.height), destination_pitch,
            bgra, overlay, overlay_width, overlay_height);

        vkResetCommandBuffer(command_buffer, 0);
        auto begin = vk_structure<VkCommandBufferBeginInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        vkBeginCommandBuffer(command_buffer, &begin);
        auto to_transfer = vk_structure<VkImageMemoryBarrier>(
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        to_transfer.srcAccessMask = 0;
        to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.image = swapchain_images[image_index];
        to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_transfer.subresourceRange.levelCount = 1;
        to_transfer.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &to_transfer);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {swapchain_extent.width, swapchain_extent.height, 1};
        vkCmdCopyBufferToImage(command_buffer, staging_buffer,
                               swapchain_images[image_index],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        VkImageMemoryBarrier to_present = to_transfer;
        to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_present.dstAccessMask = 0;
        to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &to_present);
        vkEndCommandBuffer(command_buffer);
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        auto submit = vk_structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &image_available;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &render_finished[image_index];
        if (!vk_ok(vkQueueSubmit(queue, 1, &submit, frame_fence),
                   "vkQueueSubmit"))
            return false;
        auto present = vk_structure<VkPresentInfoKHR>(
            VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_finished[image_index];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &image_index;
        const VkResult result = vkQueuePresentKHR(queue, &present);
        if (title_capture_countdown > 0 && !title_capture_name.empty() &&
            --title_capture_countdown == 0) {
            // The scene image is this frame's finished game picture,
            // before bezels and overlays; one blocking readback at a
            // single moment of the session is invisible.
            title_capture_due = true;
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            swapchain_dirty = true;
            return true;
        }
        const bool presented = vk_ok(result, "vkQueuePresentKHR");
        if (!presented)
            whitty_wall_log::note("bridge: vkQueuePresentKHR failed (%d)",
                                  static_cast<int>(result));
        if (presented && first_present) {
            whitty_wall_log::note("first present succeeded via bridge - "
                                  "showing");
            SDL_ShowWindow(window);
            first_present = false;
        }
        return presented;
    }
};

alternate_presenter::alternate_presenter() = default;
alternate_presenter::~alternate_presenter() { shutdown(); }

bool alternate_presenter::initialize(renderer_backend backend, int width,
                                     int height,
                                     const emulator_settings& settings) {
    shutdown();
    if (backend == renderer_backend::opengl) return false;
    m_impl = std::make_unique<implementation>();
    m_impl->backend = backend;
    m_impl->settings = settings;
    // X11 compositors only see a fullscreen request once the window is
    // mapped.  Setting SDL_WINDOW_FULLSCREEN_DESKTOP on a hidden window and
    // showing it after the first present leaves SDL believing it is
    // fullscreen while the compositor tiles it as an ordinary window.  That
    // made the live drawable jump between arbitrary tile sizes.  Create an
    // initially-fullscreen output in the mapped state so SDL can publish the
    // WM state as part of the initial map.
    const SDL_WindowFlags visibility_flags =
        settings.fullscreen && settings.display_index < 0 ?
            SDL_WINDOW_FULLSCREEN : SDL_WINDOW_HIDDEN;
    const SDL_WindowFlags flags = visibility_flags | SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_HIGH_PIXEL_DENSITY |
        (backend == renderer_backend::vulkan ? SDL_WINDOW_VULKAN : 0);
    m_impl->window = SDL_CreateWindow(
        backend == renderer_backend::vulkan ? "WhittyArcade - Vulkan" :
                                              "WhittyArcade - Software",
        width, height, flags);
    if (!m_impl->window) {
        std::fprintf(stderr, "Output window creation failed: %s\n", SDL_GetError());
        shutdown();
        return false;
    }
    m_impl->first_present = !settings.fullscreen;
    if (backend == renderer_backend::vulkan) {
        if (!m_impl->initialize_vulkan()) {
            shutdown();
            return false;
        }
    } else {
        m_impl->software_renderer = SDL_CreateRenderer(
            m_impl->window, "software");
        if (!m_impl->software_renderer) {
            std::fprintf(stderr, "Software output creation failed: %s\n",
                         SDL_GetError());
            shutdown();
            return false;
        }
    }
    apply_settings(settings);
    std::printf("%s presentation initialized\n", renderer_backend_name(backend));
    return true;
}

void alternate_presenter::shutdown() {
    if (!m_impl) return;
    if (m_impl->backend == renderer_backend::vulkan)
        m_impl->shutdown_vulkan();
    m_impl->destroy_mirror();
    if (m_impl->software_overlay_texture)
        SDL_DestroyTexture(m_impl->software_overlay_texture);
    if (m_impl->software_texture)
        SDL_DestroyTexture(m_impl->software_texture);
    if (m_impl->software_renderer)
        SDL_DestroyRenderer(m_impl->software_renderer);
    if (m_impl->window) SDL_DestroyWindow(m_impl->window);
    m_impl.reset();
}

void alternate_presenter::apply_settings(const emulator_settings& settings) {
    if (!m_impl || !m_impl->window) return;
    const bool vsync_changed = m_impl->settings.vsync != settings.vsync;
    const bool fullscreen_changed =
        m_impl->settings.fullscreen != settings.fullscreen;
    const bool monitor_mode_changed =
        m_impl->settings.twin_separate_monitors !=
        settings.twin_separate_monitors;
    m_impl->settings = settings;
    if (settings.output != output_mode::dual || settings.fullscreen ||
        monitor_mode_changed)
        m_impl->destroy_mirror();
    if (settings.fullscreen) {
        SDL_SetWindowMinimumSize(m_impl->window, 1, 1);
        SDL_SetWindowMaximumSize(m_impl->window, 16384, 16384);
        place_window_on_display(m_impl->window, settings.display_index, false);
        SDL_ShowWindow(m_impl->window);
        if (!SDL_SetWindowFullscreen(m_impl->window, true))
            std::fprintf(stderr, "Fullscreen request failed: %s\n",
                         SDL_GetError());
        m_impl->first_present = false;
    } else if (settings.wall_count > 1) {
        // Arcade wall: one column of several, each a different game.
        SDL_SetWindowFullscreen(m_impl->window, false);
        SDL_ShowWindow(m_impl->window);
        m_impl->first_present = false;
        m_impl->wall_placed = whitty_window::place_wall_slot(
            m_impl->window, settings.wall_slot, settings.wall_count);
        m_impl->wall_place_countdown = 0;
    } else if (settings.twin_one_screen) {
        // Linked cabinets sharing one screen: each owns its exact cell of
        // a borderless grid - halves for a pair, quarters and beyond for
        // a ring - so together they cover the display like one wall.
        SDL_SetWindowFullscreen(m_impl->window, false);
        SDL_ShowWindow(m_impl->window);
        m_impl->first_present = false;
        const char* node_text = std::getenv("WHITTY_CABINET_NODE");
        const char* count_text = std::getenv("WHITTY_CABINET_COUNT");
        const int node = node_text ? std::atoi(node_text) : 0;
        const int count = count_text ? std::atoi(count_text) : 2;
        m_impl->cabinet_placed = whitty_window::place_cabinet_cell(
            m_impl->window, node >= 1 ? node : 1, count >= 2 ? count : 2);
        // The per-present loop keeps trying: at this point the compositor
        // usually does not know the window yet.
        m_impl->cabinet_place_countdown = 0;
    } else if (settings.output == output_mode::dual &&
               settings.twin_separate_monitors) {
        // A cabinet per display: each one fullscreen on its own screen. Two
        // players never share a desktop with windows around them.
        SDL_SetWindowMinimumSize(m_impl->window, 1, 1);
        SDL_SetWindowMaximumSize(m_impl->window, 16384, 16384);
        place_window_on_display(m_impl->window, 0, false);
        SDL_ShowWindow(m_impl->window);
        m_impl->first_present = false;
        if (!SDL_SetWindowFullscreen(m_impl->window, true))
            std::fprintf(stderr, "Fullscreen request failed: %s\n",
                         SDL_GetError());
    } else {
        SDL_SetWindowFullscreen(m_impl->window, false);
        set_fixed_window_size(m_impl->window, settings.window_width);
        bool arranged_pair = false;
        if (settings.output == output_mode::dual &&
            !settings.twin_separate_monitors && m_impl->mirror_window) {
            whitty_window::arrange_twin_windows(
                m_impl->window, m_impl->mirror_window,
                settings.window_width);
            arranged_pair = true;
        }
        const int target_display =
            settings.output == output_mode::dual &&
                    settings.twin_separate_monitors
                ? 0
                : settings.display_index;
        if (!arranged_pair &&
            !place_window_on_display(m_impl->window, target_display, true))
            SDL_SetWindowPosition(m_impl->window, SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED);
    }
    if ((vsync_changed || fullscreen_changed) &&
        m_impl->backend == renderer_backend::vulkan)
        m_impl->swapchain_dirty = true;
}

void alternate_presenter::drawable_size(int& width, int& height) const {
    width = height = 1;
    if (!m_impl || !m_impl->window) return;
    if (m_impl->backend == renderer_backend::vulkan) {
        SDL_GetWindowSizeInPixels(m_impl->window, &width, &height);
    } else if (m_impl->software_renderer) {
        SDL_GetCurrentRenderOutputSize(m_impl->software_renderer, &width, &height);
    }
    width = std::max(width, 1);
    height = std::max(height, 1);
}

void* alternate_presenter::window() const {
    return m_impl ? m_impl->window : nullptr;
}

renderer_backend alternate_presenter::backend() const {
    return m_impl ? m_impl->backend : renderer_backend::opengl;
}

bool alternate_presenter::render_board_frame(const uint8_t* pixels, int width,
                                             int height, bool flip_y,
                                             const board_overlays& overlays,
                                             bool menu_visible,
                                             int display_width,
                                             int display_height) {
    if (!m_impl || m_impl->backend != renderer_backend::vulkan) return false;
    // A null pixels pointer is not an error: it means the scene image already
    // holds the picture, which is how a rasterised polygon frame and a paused
    // redraw both reach the screen. Only a partially described upload is
    // rejected here; the implementation decides whether a retained frame
    // exists to present.
    if (pixels && (width <= 0 || height <= 0)) return false;
    // The windowed twin mirror runs off the board pixels, so it only has
    // something to show when this call is carrying them.
    if (pixels)
        m_impl->present_mirror(pixels, width, height, display_width,
                               display_height, menu_visible, flip_y);
    const bool presented =
        m_impl->render_board_frame(pixels, width, height, flip_y, overlays,
                                   menu_visible, display_width,
                                   display_height);
    service_title_capture();
    return presented;
}

bool alternate_presenter::upload_scene_sheet(scene_sheet sheet,
                                             const void* pixels,
                                             std::size_t bytes) {
    if (!m_impl || m_impl->backend != renderer_backend::vulkan) return false;
    if (!pixels || bytes == 0) return false;
    if (!m_impl->create_native_path() || !m_impl->create_scene_pipelines())
        return false;
    auto& target =
        m_impl->scene_sheets[static_cast<std::size_t>(sheet)];
    const int index = static_cast<int>(sheet);
    const auto shape =
        alternate_presenter::implementation::shape_of(index);
    const std::size_t capacity_bytes =
        static_cast<std::size_t>(shape.width) * shape.height * shape.layers *
        shape.bytes_per_texel;
    if (shape.streams) {
        // Rewritten as the board runs: stage it and let the next frame's
        // command buffer carry the copy, rather than stalling the queue.
        if (bytes < capacity_bytes) {
            // Say so: this refusal ran silently while Model 2's 2D layers
            // were handed over in pixels rather than bytes, and the only
            // symptom was every 2D screen on the board coming up black.
            static int refused = 0;
            if (whitty_wall_log::first(refused))
                whitty_wall_log::note(
                    "sheet %d upload refused: %zu bytes, image needs %zu",
                    index, bytes, capacity_bytes);
            return false;
        }
        auto& stream =
            m_impl->sheet_streaming[static_cast<std::size_t>(index)];
        if (!stream.buffer &&
            !m_impl->create_host_buffer(stream.buffer, stream.memory,
                                        stream.mapped, capacity_bytes,
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
            return false;
        std::memcpy(stream.mapped, pixels, capacity_bytes);
        stream.pending = true;
        return true;
    }
    // A short upload is legitimate - a board may publish only the banks it
    // has - so clamp rather than reject, leaving the remainder as it was.
    return m_impl->upload_image(target,
                                static_cast<const uint8_t*>(pixels),
                                std::min(bytes, capacity_bytes));
}

bool alternate_presenter::upload_gamma_ramp(const void* pixels,
                                            std::size_t bytes) {
    if (!m_impl || m_impl->backend != renderer_backend::vulkan) return false;
    if (!pixels || bytes < 256 * 3) return false;
    if (!m_impl->create_native_path()) return false;
    const auto* ramp = static_cast<const uint8_t*>(pixels);
    constexpr std::size_t ramp_bytes = 256 * 3;
    if (m_impl->gamma_resident.size() == ramp_bytes &&
        std::equal(ramp, ramp + ramp_bytes, m_impl->gamma_resident.begin()))
        return true;
    if (!m_impl->upload_image(m_impl->gamma_image, ramp, ramp_bytes))
        return false;
    m_impl->gamma_resident.assign(ramp, ramp + ramp_bytes);
    return true;
}

bool alternate_presenter::render_system22_scene(
        const system22_scene& scene) {
    static int backend_note = 0, extent_note = 0, target_note = 0;
    if (!m_impl || m_impl->backend != renderer_backend::vulkan) {
        if (whitty_wall_log::first(backend_note))
            whitty_wall_log::note("scene declined: presenter backend=%d",
                                  m_impl ? static_cast<int>(m_impl->backend)
                                         : -1);
        return false;
    }
    if (scene.width <= 0 || scene.height <= 0) {
        if (whitty_wall_log::first(extent_note))
            whitty_wall_log::note("scene declined: extent %dx%d", scene.width,
                                  scene.height);
        return false;
    }
    if (!m_impl->create_native_path()) return false;
    if (!m_impl->ensure_scene_target(scene.width, scene.height) ||
        !m_impl->create_scene_pipelines()) {
        if (whitty_wall_log::first(target_note))
            whitty_wall_log::note("scene declined: target or pipelines");
        return false;
    }
    // Splits "the board produced no polygons" from "the polygons were drawn
    // invisibly" without another instrumentation round: a scene with zero
    // vertices is an emulation question, not a renderer one.
    static int content_note = 0;
    if (whitty_wall_log::first(content_note, 5))
        whitty_wall_log::note("scene: %zu vertices, text=%d",
                              scene.vertex_count, scene.draw_text ? 1 : 0);

    // The previous frame may still be reading the host-visible vertex and
    // uniform buffers, so retire it before overwriting them.
    if (!m_impl->wait_frame_fence("system22 scene")) return false;

    const std::size_t vertex_bytes =
        scene.vertex_count * system22_vertex_layout::stride;
    if (vertex_bytes > 0) {
        if (!scene.vertices ||
            !m_impl->ensure_vertex_capacity(vertex_bytes))
            return false;
        std::memcpy(m_impl->vertex_mapped, scene.vertices, vertex_bytes);
    }
    std::memcpy(m_impl->polygon_uniform_mapped, &scene.uniforms,
                sizeof(scene.uniforms));
    std::memcpy(m_impl->text_uniform_mapped, &scene.text_uniforms,
                sizeof(scene.text_uniforms));
    m_impl->refresh_scene_descriptors();

    m_impl->pending_vertex_count = scene.vertex_count;
    m_impl->pending_draw_text = scene.draw_text;
    m_impl->pending_scene = true;
    // The scene image now carries the picture, top row first, which is what
    // the post shader's flip_y describes.
    m_impl->board_flip_y = true;
    m_impl->board_apply_color = scene.apply_board_color;
    m_impl->board_super_system22 = scene.super_system22;
    for (int channel = 0; channel < 3; ++channel)
        m_impl->board_screen_fade[channel] = scene.screen_fade[channel];
    m_impl->board_display_width = scene.width;
    m_impl->board_display_height = scene.height;
    m_impl->have_board_frame = true;
    return true;
}

bool alternate_presenter::upload_model2_color_table(const void* rgb,
                                                    std::size_t bytes) {
    if (!m_impl || m_impl->backend != renderer_backend::vulkan) return false;
    if (!rgb) return false;
    constexpr std::size_t texels = std::size_t{1024} * 64;
    if (bytes < texels * 3) return false;
    const auto* source = static_cast<const uint8_t*>(rgb);
    std::vector<uint8_t> widened(texels * 4);
    for (std::size_t index = 0; index < texels; ++index) {
        widened[index * 4 + 0] = source[index * 3 + 0];
        widened[index * 4 + 1] = source[index * 3 + 1];
        widened[index * 4 + 2] = source[index * 3 + 2];
        widened[index * 4 + 3] = 255;
    }
    return upload_scene_sheet(scene_sheet::model2_color, widened.data(),
                              widened.size());
}

bool alternate_presenter::render_model2_scene(const model2_scene& scene) {
    static int declined_note = 0;
    if (!m_impl || m_impl->backend != renderer_backend::vulkan ||
        scene.width <= 0 || scene.height <= 0 ||
        !m_impl->create_native_path() ||
        !m_impl->ensure_scene_target(scene.width, scene.height) ||
        !m_impl->create_model2_pipelines()) {
        if (whitty_wall_log::first(declined_note))
            whitty_wall_log::note(
                "model2 scene declined: impl=%d vulkan=%d size=%dx%d "
                "native=%d target=%d pipelines=%d",
                m_impl ? 1 : 0,
                m_impl && m_impl->backend == renderer_backend::vulkan,
                scene.width, scene.height,
                m_impl && m_impl->create_native_path(),
                m_impl && m_impl->ensure_scene_target(scene.width,
                                                      scene.height),
                m_impl && m_impl->create_model2_pipelines());
        return false;
    }

    // Retire the previous frame before overwriting host-visible memory it
    // may still be reading.
    if (!m_impl->wait_frame_fence("model2 scene")) return false;

    const std::size_t vertex_bytes =
        scene.vertex_count * model2_vertex_layout::stride;
    if (vertex_bytes > 0) {
        if (!scene.vertices || !m_impl->ensure_vertex_capacity(vertex_bytes))
            return false;
        std::memcpy(m_impl->vertex_mapped, scene.vertices, vertex_bytes);
    }
    // How much geometry the board actually handed over. "Rasterised" only
    // says the pass ran; a pass with nothing in it succeeds and produces a
    // black screen, which is indistinguishable from the outside.
    static int geometry_note = 0;
    static std::size_t frames_seen = 0;
    if (whitty_wall_log::first(geometry_note, 3) || ++frames_seen % 600 == 0)
        whitty_wall_log::note("model2 geometry: %zu vertices (%zu triangles)",
                              scene.vertex_count, scene.vertex_count / 3);
    m_impl->refresh_model2_descriptors();
    m_impl->model2_vertex_count = scene.vertex_count;
    m_impl->model2_crtc_offset[0] = scene.crtc_offset[0];
    m_impl->model2_crtc_offset[1] = scene.crtc_offset[1];
    m_impl->model2_pending = true;
    m_impl->board_flip_y = true;
    m_impl->board_apply_color = false;
    m_impl->board_display_width = scene.width;
    m_impl->board_display_height = scene.height;
    m_impl->have_board_frame = true;
    return true;
}

bool alternate_presenter::set_bezel(const bezel_frame& bezel) {
    if (!m_impl || m_impl->backend != renderer_backend::vulkan) return false;
    auto& geometry = m_impl->bezel;
    if (!bezel.pixels || bezel.width <= 0 || bezel.height <= 0 ||
        bezel.cutout_width <= 0 || bezel.cutout_height <= 0) {
        // Clearing is legitimate - a board with no published bezel - and puts
        // the ordinary letterbox back.
        geometry = implementation::bezel_geometry{};
        m_impl->overlays[implementation::overlay_bezel].resident = false;
        return true;
    }
    if (!m_impl->create_native_path()) return false;
    auto& slot = m_impl->overlays[implementation::overlay_bezel];
    if (slot.image.width != bezel.width || slot.image.height != bezel.height) {
        vkDeviceWaitIdle(m_impl->device);
        if (!m_impl->create_image(slot.image, bezel.width, bezel.height, 1,
                                  VK_FORMAT_R8G8B8A8_UNORM))
            return false;
        m_impl->descriptors_valid = false;
    }
    if (!m_impl->upload_image(
            slot.image, bezel.pixels,
            static_cast<std::size_t>(bezel.width) * bezel.height * 4))
        return false;
    slot.resident = true;
    geometry.width = bezel.width;
    geometry.height = bezel.height;
    geometry.cutout = {bezel.cutout_x, bezel.cutout_y, bezel.cutout_width,
                       bezel.cutout_height};
    return true;
}

void alternate_presenter::arm_title_capture(std::string short_name,
                                            int frames) {
    if (!m_impl || short_name.empty() || frames <= 0) return;
    m_impl->title_capture_name = std::move(short_name);
    m_impl->title_capture_countdown = frames;
    m_impl->title_capture_due = false;
    whitty_wall_log::note("title capture armed for %s",
                          m_impl->title_capture_name.c_str());
}

void alternate_presenter::service_title_capture() {
    if (!m_impl || !m_impl->title_capture_due) return;
    m_impl->title_capture_due = false;
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    if (read_scene_image(rgba, width, height) &&
        title_capture_write(m_impl->title_capture_name, rgba.data(), width,
                            height, /*top_down=*/true))
        whitty_wall_log::note("title captured for %s",
                              m_impl->title_capture_name.c_str());
    m_impl->title_capture_name.clear();
    m_impl->title_capture_countdown = -1;
}

bool alternate_presenter::read_scene_image(std::vector<uint8_t>& rgba,
                                           int& width, int& height) {
    if (!m_impl || m_impl->backend != renderer_backend::vulkan) return false;
    auto& impl = *m_impl;
    if (!impl.scene_image.image || impl.scene_image.width <= 0) return false;
    width = impl.scene_image.width;
    height = impl.scene_image.height;
    const VkDeviceSize bytes =
        static_cast<VkDeviceSize>(width) * height * 4;
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void* mapped{};
    if (!impl.create_host_buffer(buffer, memory, mapped, bytes,
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;
    auto allocate = vk_structure<VkCommandBufferAllocateInfo>(
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    allocate.commandPool = impl.command_pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer commands{};
    bool copied = false;
    if (vk_ok(vkAllocateCommandBuffers(impl.device, &allocate, &commands),
              "vkAllocateCommandBuffers")) {
        auto begin = vk_structure<VkCommandBufferBeginInfo>(
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commands, &begin);
        auto barrier = vk_structure<VkImageMemoryBarrier>(
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.image = impl.scene_image.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {static_cast<uint32_t>(width),
                            static_cast<uint32_t>(height), 1};
        vkCmdCopyImageToBuffer(commands, impl.scene_image.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer,
                               1, &copy);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        vkEndCommandBuffer(commands);
        auto submit =
            vk_structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        if (vk_ok(vkQueueSubmit(impl.queue, 1, &submit, VK_NULL_HANDLE),
                  "vkQueueSubmit")) {
            vkQueueWaitIdle(impl.queue);
            rgba.assign(static_cast<const uint8_t*>(mapped),
                        static_cast<const uint8_t*>(mapped) + bytes);
            copied = true;
        }
        vkFreeCommandBuffers(impl.device, impl.command_pool, 1, &commands);
    }
    impl.destroy_host_buffer(buffer, memory, mapped);
    return copied;
}

bool alternate_presenter::device_lost() const {
    return g_device_lost.load();
}

bool alternate_presenter::repeat_board_frame() {
    if (!m_impl || m_impl->backend != renderer_backend::vulkan) return false;
    const board_overlays none{};
    const bool presented =
        m_impl->render_board_frame(nullptr, 0, 0, false, none, false, 0, 0);
    service_title_capture();
    return presented;
}

bool alternate_presenter::present_rgba_bottom_up(const uint8_t* pixels,
                                                  int width, int height,
                                                  const uint8_t* overlay_pixels,
                                                  int overlay_width,
                                                  int overlay_height,
                                                  bool menu_visible,
                                                  int display_width,
                                                  int display_height) {
    if (!m_impl || !pixels || width <= 0 || height <= 0) return false;
    m_impl->present_mirror(pixels, width, height, display_width,
                           display_height, menu_visible);
    if (m_impl->backend == renderer_backend::vulkan) {
        const bool presented =
            m_impl->present_vulkan(pixels, width, height, overlay_pixels,
                                   overlay_width, overlay_height,
                                   menu_visible, display_width,
                                   display_height);
        service_title_capture();
        return presented;
    }
    if (!m_impl->software_texture || width != m_impl->texture_width ||
        height != m_impl->texture_height) {
        if (m_impl->software_texture)
            SDL_DestroyTexture(m_impl->software_texture);
        m_impl->software_texture = SDL_CreateTexture(
            m_impl->software_renderer, SDL_PIXELFORMAT_RGBA32,
            SDL_TEXTUREACCESS_STREAMING, width, height);
        m_impl->texture_width = width;
        m_impl->texture_height = height;
    }
    if (!m_impl->software_texture) return false;
    m_impl->flipped_pixels.resize(static_cast<std::size_t>(width) * height * 4);
    const std::size_t pitch = static_cast<std::size_t>(width) * 4;
    for (int y = 0; y < height; ++y)
        std::memcpy(m_impl->flipped_pixels.data() + y * pitch,
                    pixels + (height - 1 - y) * pitch, pitch);
    SDL_UpdateTexture(m_impl->software_texture, nullptr,
                      m_impl->flipped_pixels.data(), static_cast<int>(pitch));
    SDL_SetRenderDrawColor(m_impl->software_renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_impl->software_renderer);
    int output_width = 1;
    int output_height = 1;
    drawable_size(output_width, output_height);
    const int aspect_width =
        display_width > 0 ? display_width : width;
    const int aspect_height =
        display_height > 0 ? display_height : height;
    // One pane, or two side-by-side panes for fullscreen dual output. Each pane
    // keeps the arcade framebuffer's aspect and letterboxes within its half
    // instead of stretching to the (possibly ultrawide) drawable.
    struct present_pane { int x; int w; };
    std::array<present_pane, 2> panes{{{0, output_width}, {0, 0}}};
    int pane_count = 1;
    if (m_impl->settings.output == output_mode::dual &&
        m_impl->settings.fullscreen && !menu_visible) {
        const int gap = std::max(output_width / 40, 24);
        const int left_w = std::clamp((output_width - gap) / 2, 1,
                                      output_width - 1);
        panes[0] = {0, left_w};
        panes[1] = {left_w + gap, output_width - left_w - gap};
        pane_count = 2;
    }
    for (int pane = 0; pane < pane_count; ++pane) {
        const bool framed = m_impl->settings.output == output_mode::dual &&
            !menu_visible;
        const fitted_viewport fit = fit_arcade_viewport(
            panes[pane].x, panes[pane].w, output_height,
            aspect_width, aspect_height, width, height,
            m_impl->settings.integer_scaling,
            m_impl->settings.fullscreen, framed);
        if (framed) {
            SDL_SetRenderDrawColor(m_impl->software_renderer, 13, 29, 39, 255);
            SDL_FRect outer{static_cast<float>(fit.x - 10),
                            static_cast<float>(fit.y - 10),
                            static_cast<float>(fit.w + 20),
                            static_cast<float>(fit.h + 20)};
            SDL_RenderFillRect(m_impl->software_renderer, &outer);
            SDL_SetRenderDrawColor(m_impl->software_renderer,
                                   56, 198, 255, 255);
            SDL_FRect accent{static_cast<float>(fit.x - 3),
                             static_cast<float>(fit.y - 3),
                             static_cast<float>(fit.w + 6),
                             static_cast<float>(fit.h + 6)};
            SDL_RenderFillRect(m_impl->software_renderer, &accent);
        }
        const SDL_FRect destination{
            static_cast<float>(fit.x), static_cast<float>(fit.y),
            static_cast<float>(fit.w), static_cast<float>(fit.h)};
        SDL_RenderTexture(m_impl->software_renderer, m_impl->software_texture,
                          nullptr, &destination);
    }
    if (overlay_pixels && overlay_width > 0 && overlay_height > 0) {
        if (!m_impl->software_overlay_texture ||
            overlay_width != m_impl->overlay_width ||
            overlay_height != m_impl->overlay_height) {
            if (m_impl->software_overlay_texture)
                SDL_DestroyTexture(m_impl->software_overlay_texture);
            m_impl->software_overlay_texture = SDL_CreateTexture(
                m_impl->software_renderer, SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_STREAMING, overlay_width, overlay_height);
            m_impl->overlay_width = overlay_width;
            m_impl->overlay_height = overlay_height;
            if (m_impl->software_overlay_texture)
                SDL_SetTextureBlendMode(m_impl->software_overlay_texture,
                                        SDL_BLENDMODE_BLEND);
        }
        if (m_impl->software_overlay_texture) {
            SDL_UpdateTexture(m_impl->software_overlay_texture, nullptr,
                              overlay_pixels, overlay_width * 4);
            const SDL_FRect overlay_destination{
                12.0f, 12.0f, static_cast<float>(overlay_width),
                static_cast<float>(overlay_height)};
            SDL_RenderTexture(m_impl->software_renderer,
                              m_impl->software_overlay_texture, nullptr,
                              &overlay_destination);
        }
    }
    SDL_RenderPresent(m_impl->software_renderer);
    if (m_impl->first_present) {
        SDL_ShowWindow(m_impl->window);
        m_impl->first_present = false;
    }
    return true;
}
