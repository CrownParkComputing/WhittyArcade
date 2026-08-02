// arcade_renderer.cpp - shared renderer and output UI implementation
#include "arcade_renderer.h"
#include "arcade_catalog.h"
#include "namco/system22/system22_config.h"
#include "arcade_presenter.h"
#include "arcade_sdl_guard.h"
#include "network_video_link.h"
#include "title_capture.h"
#include "sega/model2/model2_draw_list.h"
#include "platform_paths.h"
#include "bezel_library.h"
#include "system22_vulkan_uniforms.h"
#include "twin_window_layout.h"
#include "wall_log.h"

#include <GL/glew.h>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <string>

namespace {
std::array<int, 2> bounded_window_size(SDL_Window* window,
                                       int requested_width) {
    SDL_Rect usable{0, 0, 1920, 1080};
    SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) :
                                     SDL_GetPrimaryDisplay();
    if (!display || !SDL_GetDisplayUsableBounds(display, &usable)) {
        display = SDL_GetPrimaryDisplay();
        if (display) SDL_GetDisplayBounds(display, &usable);
    }
    int border_top = 0, border_left = 0, border_bottom = 0, border_right = 0;
    if (window)
        SDL_GetWindowBordersSize(window, &border_top, &border_left,
                                 &border_bottom, &border_right);
    int maximum_width = std::min(
        usable.w - border_left - border_right,
        (usable.h - border_top - border_bottom) * 4 / 3);
    // Linked cabinets that share one desktop are constrained to half the
    // usable width so the two side-by-side windows never overlap.
    const char* node_text = std::getenv("WHITTY_CABINET_NODE");
    const bool cabinet_node = node_text && *node_text &&
        (node_text[0] == '1' || node_text[0] == '2');
    if (cabinet_node)
        maximum_width = std::min(maximum_width, usable.w / 2);
    maximum_width = std::max(320, maximum_width);
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

// Place a linked cabinet on its half of the only physical display.
// Used when the host machine has one monitor and is running two separate
// MANX processes for cabinet 1 / cabinet 2 — each process needs
// to position its own window without help from the other. Cabinet 1 →
// left half, cabinet 2 → right half, so the two side-by-side windows fit
// a single ultrawide without overlap.
// Whether System 22 rasterises through Vulkan. On by default; set
// WHITTY_VK_SYSTEM22=0 to fall back to the OpenGL route.
//
// This gates the sheet uploads as well as the draw, because an unused path
// must cost nothing at all - not even a texture upload. Building the scene
// sheets is tens of megabytes of image creation and several queue waits, and
// once cost the window entirely when only the draw was gated.
bool native_path_enabled(const char* variable, bool default_on) {
    const char* value = std::getenv(variable);
    if (!value) return default_on;
    return *value != '0';
}

bool native_system22_enabled() {
    static const bool enabled =
        native_path_enabled("WHITTY_VK_SYSTEM22", true);
    return enabled;
}

// Model 2 through Vulkan. On by default; set WHITTY_VK_MODEL2=0 to fall back
// to the OpenGL route. Gates the uploads as well as the draw, for the reason
// the System 22 gate does.
bool native_model2_enabled() {
    static const bool enabled =
        native_path_enabled("WHITTY_VK_MODEL2", true);
    return enabled;
}

bool place_window_on_cabinet_half(SDL_Window* window, int cabinet_node) {
    const char* count_text = std::getenv("WHITTY_CABINET_COUNT");
    const int count = count_text ? std::atoi(count_text) : 2;
    return whitty_window::place_cabinet_cell(window, cabinet_node,
                                             count >= 2 ? count : 2);
}

// Which half of the shared desktop this process owns. The launcher exports the
// node before the window exists, so both the OpenGL and alternate-presenter
// paths can read it without threading it through their own settings.
int cabinet_node_from_environment() {
    const char* node_text = std::getenv("WHITTY_CABINET_NODE");
    const int node = node_text ? std::atoi(node_text) : 0;
    return node >= 1 && node <= 8 ? node : 1;
}

constexpr float normalized_byte(uint8_t value) {
    return static_cast<float>(value) / 255.0f;
}

struct draw_vertex {
    float x, y, z;
    float u_over_z;
    float v_over_z;
    float brightness_over_z;
    float one_over_z;
    float palette_base;
    float alpha_palette;
    float color_mode;
    float texture_bank;
    float direct;
    float fog;
    float fog_color_r;
    float fog_color_g;
    float fog_color_b;
    float viewport_x;
    float viewport_y;
    float clip_left;
    float clip_right;
    float clip_top;
    float clip_bottom;
    float sprite;
    float sprite_alpha;
    float object_flags;
};

// The Vulkan polygon pipeline declares its vertex attributes from these same
// offsets, so this array uploads to it unmodified. Reordering the struct
// without updating system22_vertex_layout breaks the build here rather than
// feeding the shader plausible-looking nonsense.
static_assert(sizeof(draw_vertex) == system22_vertex_layout::stride);
static_assert(offsetof(draw_vertex, x) == system22_vertex_layout::position);
static_assert(offsetof(draw_vertex, u_over_z) ==
              system22_vertex_layout::u_over_z);
static_assert(offsetof(draw_vertex, v_over_z) ==
              system22_vertex_layout::v_over_z);
static_assert(offsetof(draw_vertex, brightness_over_z) ==
              system22_vertex_layout::brightness_over_z);
static_assert(offsetof(draw_vertex, one_over_z) ==
              system22_vertex_layout::one_over_z);
static_assert(offsetof(draw_vertex, palette_base) ==
              system22_vertex_layout::palette);
static_assert(offsetof(draw_vertex, color_mode) ==
              system22_vertex_layout::color_mode);
static_assert(offsetof(draw_vertex, texture_bank) ==
              system22_vertex_layout::texture_bank);
static_assert(offsetof(draw_vertex, direct) ==
              system22_vertex_layout::direct);
static_assert(offsetof(draw_vertex, fog) == system22_vertex_layout::fog);
static_assert(offsetof(draw_vertex, fog_color_r) ==
              system22_vertex_layout::fog_color);
static_assert(offsetof(draw_vertex, viewport_x) ==
              system22_vertex_layout::viewport);
static_assert(offsetof(draw_vertex, clip_left) ==
              system22_vertex_layout::clip_rect);
static_assert(offsetof(draw_vertex, sprite) ==
              system22_vertex_layout::render_flags);

// model2_draw_vertex and the draw-list construction live in
// sega/model2/model2_draw_list.h, shared with the Vulkan presenter harness so
// its replay exercises the exact records the game submits.

constexpr const char* model2_vertex_shader_source = R"GLSL(
#version 430 core
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec2 in_center;
layout(location = 3) in vec4 in_clip;
layout(location = 4) in float in_header0;
layout(location = 5) in float in_header1;
layout(location = 6) in float in_header2;
layout(location = 7) in float in_color_base;
layout(location = 8) in float in_luma;
layout(location = 9) in float in_renderer;
layout(location = 10) in float in_texture_lod;
layout(location = 11) in float in_draw_priority;

uniform vec2 crtc_offset;
out vec2 texture_uv;
flat out vec4 clip_rect;
flat out uint header0;
flat out uint header1;
flat out uint header2;
flat out uint color_base;
flat out uint polygon_luma;
flat out uint renderer;
flat out int texture_lod;
flat out float object_priority;

void main() {
    float z = in_position.z;
    float center_x = crtc_offset.x + in_center.x;
    float center_y = 384.0 - in_center.y + crtc_offset.y;
    float clip_x = ((2.0 * center_x / 496.0) - 1.0) * z +
                   (2.0 * in_position.x / 496.0);
    float clip_y = (1.0 - (2.0 * center_y / 384.0)) * z +
                   (2.0 * in_position.y / 384.0);
    gl_Position = vec4(clip_x, clip_y, 0.0, z);
    texture_uv = in_uv;
    clip_rect = in_clip;
    header0 = uint(in_header0);
    header1 = uint(in_header1);
    header2 = uint(in_header2);
    color_base = uint(in_color_base);
    polygon_luma = uint(in_luma);
    renderer = uint(in_renderer);
    texture_lod = int(in_texture_lod);
    object_priority = in_draw_priority;
}
)GLSL";

constexpr const char* model2_fragment_shader_source = R"GLSL(
#version 430 core
in vec2 texture_uv;
flat in vec4 clip_rect;
flat in uint header0;
flat in uint header1;
flat in uint header2;
flat in uint color_base;
flat in uint polygon_luma;
flat in uint renderer;
flat in int texture_lod;
flat in float object_priority;

uniform usampler2DArray texture_sheets;
uniform usampler2D luma_table;
uniform sampler2D color_table;
out vec4 output_color;

uint sheet_byte(uint layer, uint address) {
    return texelFetch(texture_sheets,
        ivec3(int(address & 1023u), int(address >> 10u), int(layer)), 0).r;
}

uint model2_texel_at(int base_x, int base_y, int x, int y,
                     int level, bool microtexture) {
    int sheet_x = base_x + x;
    int sheet_y = base_y + y;
    if (sheet_x >= 1024) {
        sheet_x -= 1024;
        sheet_y ^= 1024;
    }
    uint packed_index = uint((sheet_y / 2) * 512 + sheet_x / 2);
    uint address = (packed_index >> 1u) * 4u +
                   ((packed_index & 1u) * 2u);
    uint base_layer = (header2 & 0x1000u) != 0u ? 1u : 0u;
    uint layer = microtexture ? (base_layer ^ 1u) :
                                (base_layer ^ uint(level & 1));
    uint value = sheet_byte(layer, address) |
                 (sheet_byte(layer, address + 1u) << 8u);
    if ((y & 1) == 0) value >>= 8u;
    if ((x & 1) == 0) value >>= 4u;
    return value & 15u;
}

uint model2_lerp(uint first, uint second, uint amount) {
    return (first + (((second - first) * amount) >> 8u)) & 0x00ff00ffu;
}

uint model2_filtered_texel(int level, int u, int v, bool microtexture) {
    int width;
    int height;
    int base_x;
    int base_y;
    if (microtexture) {
        width = 128;
        height = 128;
        base_x = int((header2 >> 13u) & 1u) * 128;
        base_y = int((header2 >> 14u) & 3u) * 128;
        int coordinate_shift = 1 << int((header0 >> 10u) & 3u);
        u = int(uint(u) << coordinate_shift);
        v = int(uint(v) << coordinate_shift);
        level = 0;
    } else {
        width = (32 << int(header0 & 7u)) >> level;
        height = (32 << int((header0 >> 3u) & 7u)) >> level;
        base_x = ((32 * int(header2 & 0x3fu) - 2048) >> level) & 2047;
        base_y = ((32 * int((header2 >> 6u) & 0x1fu) - 1024) >> level) & 1023;
        u >>= level;
        v >>= level;
    }

    bool mirror_x = (header0 & 0x100u) != 0u;
    bool mirror_y = (header0 & 0x200u) != 0u;
    bool wrap_x = (header0 & 0x040u) != 0u && !mirror_x;
    bool wrap_y = (header0 & 0x080u) != 0u && !mirror_y;
    if (mirror_x && (u & (width << 8)) != 0) u = ~u;
    if (mirror_y && (v & (height << 8)) != 0) v = ~v;

    u -= 0x80;
    v -= 0x80;
    uint u_fraction = uint(u) & 0xffu;
    uint v_fraction = uint(v) & 0xffu;
    int u0 = (u >> 8) & (width - 1);
    int u1 = (u0 + 1) & (width - 1);
    int v0 = (v >> 8) & (height - 1);
    int v1 = (v0 + 1) & (height - 1);
    if (!wrap_x && u1 == 0) {
        if (u_fraction >= 0x80u) {
            u0 = u1;
            ++u1;
            u_fraction = 0u;
        } else {
            u1 = u0;
            --u0;
            u_fraction = 0x100u;
        }
    }
    if (!wrap_y && v1 == 0) {
        if (v_fraction >= 0x80u) {
            v0 = 0;
            ++v1;
            v_fraction = 0u;
        } else {
            v1 = v0;
            --v0;
            v_fraction = 0x100u;
        }
    }

    uint tex00 = model2_texel_at(base_x, base_y, u0, v0,
                                 level, microtexture) << 4u;
    uint tex01 = model2_texel_at(base_x, base_y, u1, v0,
                                 level, microtexture) << 4u;
    uint tex10 = model2_texel_at(base_x, base_y, u0, v1,
                                 level, microtexture) << 4u;
    uint tex11 = model2_texel_at(base_x, base_y, u1, v1,
                                 level, microtexture) << 4u;
    if (renderer == 3u) {
        if (tex00 != 0xf0u) tex00 |= 0x00800000u;
        if (tex01 != 0xf0u) tex01 |= 0x00800000u;
        if (tex10 != 0xf0u) tex10 |= 0x00800000u;
        if (tex11 != 0xf0u) tex11 |= 0x00800000u;
        if (tex00 == 0x000000f0u) tex00 = tex01 & 0xffu;
        if (tex01 == 0x000000f0u) tex01 = tex00 & 0xffu;
        if (tex10 == 0x000000f0u) tex10 = tex11 & 0xffu;
        if (tex11 == 0x000000f0u) tex11 = tex10 & 0xffu;
    }
    uint top = model2_lerp(tex00, tex01, u_fraction);
    uint bottom = model2_lerp(tex10, tex11, u_fraction);
    if (renderer == 3u) {
        if (top == 0x000000f0u) top = bottom & 0xffu;
        if (bottom == 0x000000f0u) bottom = top & 0xffu;
    }
    return model2_lerp(top, bottom, v_fraction);
}

void main() {
    int screen_x = int(gl_FragCoord.x);
    int screen_y = 383 - int(gl_FragCoord.y);
    if (screen_x < int(clip_rect.x) || screen_x > int(clip_rect.y) ||
        screen_y < int(clip_rect.z) || screen_y > int(clip_rect.w))
        discard;
    if ((header0 & 0x8000u) != 0u &&
        ((screen_x ^ screen_y) & 1) == 0)
        discard;

    uint luma = min(polygon_luma >> 2u, 63u);
    if ((renderer & 2u) != 0u) {
        int width = 32 << int(header0 & 7u);
        int height = 32 << int((header0 >> 3u) & 7u);
        int maximum_level = findMSB(min(width, height)) - 1;
        float camera_z = 1.0 / max(gl_FragCoord.w, 1.0e-30);
        int mml = -texture_lod + int(round(log2(camera_z) * 256.0));
        int level = clamp(mml >> 7, 0, maximum_level);
        int u = int(texture_uv.x * 256.0);
        int v = int(texture_uv.y * 256.0);
        uint sample_value = model2_filtered_texel(level, u, v, false);
        if (mml > 0 && level < maximum_level) {
            uint next_value = model2_filtered_texel(level + 1, u, v, false);
            sample_value = model2_lerp(
                sample_value, next_value, uint(mml & 127) << 1u);
        } else if ((header0 & 0x1000u) != 0u && mml < 0) {
            uint micro_value = model2_filtered_texel(0, u, v, true);
            int minimum_lod = int((header0 >> 10u) & 3u);
            uint amount = uint(min((-mml) >> minimum_lod, 127));
            sample_value = model2_lerp(sample_value, micro_value, amount);
        }
        if (renderer == 3u && sample_value < 0x00400000u) discard;
        uint texel_luma = (sample_value & 0xffu) >> 1u;
        uint luma_address = ((header1 & 255u) << 7u) + texel_luma;
        uint texture_luma = texelFetch(luma_table,
            ivec2(int(luma_address & 255u), int(luma_address >> 8u)), 0).r;
        luma = min((texture_luma * polygon_luma) >> 8u, 63u);
    }
    vec3 rgb = texelFetch(color_table,
        ivec2(int(color_base & 1023u), int(luma)), 0).rgb;
    // Model 2 has no per-pixel camera-Z test.  Its geometrizer places each
    // polygon in a 16-bit Z-sort bucket, then the rasterizer draws the
    // buckets front-to-back into a one-bit fill buffer.  Once a pixel is
    // filled, a later polygon cannot replace it.  Using interpolated camera
    // Z here lets large road polygons overwrite cars whose object-level
    // Z-sort deliberately places them in front, making the cars vanish as
    // the camera moves.  polygon_order already matches the hardware bucket
    // and linked-list order, so use that order as the depth value directly.
    gl_FragDepth = object_priority;
    output_color = vec4(rgb, 1.0);
}
)GLSL";

constexpr const char* model2_overlay_vertex_shader_source = R"GLSL(
#version 430 core
void main() {
    const vec2 positions[4] = vec2[4](
        vec2(-1.0, -1.0), vec2(1.0, -1.0),
        vec2(-1.0,  1.0), vec2(1.0,  1.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)GLSL";

constexpr const char* model2_overlay_fragment_shader_source = R"GLSL(
#version 430 core
uniform sampler2D layer_texture;
out vec4 output_color;
void main() {
    ivec2 coordinate = ivec2(int(gl_FragCoord.x),
                             383 - int(gl_FragCoord.y));
    output_color = texelFetch(layer_texture, coordinate, 0);
}
)GLSL";

constexpr const char* vertex_shader_source = R"GLSL(
#version 430 core
layout(location = 0) in vec3 in_position;
layout(location = 1) in float in_u_over_z;
layout(location = 2) in float in_v_over_z;
layout(location = 3) in float in_brightness_over_z;
layout(location = 4) in float in_one_over_z;
layout(location = 5) in vec2 in_palette;
layout(location = 6) in float in_color_mode;
layout(location = 7) in float in_texture_bank;
layout(location = 8) in float in_direct;
layout(location = 9) in float in_fog;
layout(location = 10) in vec3 in_fog_color;
layout(location = 11) in vec2 in_viewport;
layout(location = 12) in vec4 in_clip_rect;
layout(location = 13) in vec3 in_render_flags;

uniform mat4 view_matrix;
noperspective out float u_over_z;
noperspective out float v_over_z;
noperspective out float brightness_over_z;
noperspective out float one_over_z;
noperspective out float fog_factor;
flat out uint palette_base;
flat out uint alpha_palette;
flat out uint color_mode;
flat out uint texture_bank;
flat out vec3 polygon_fog_color;
flat out vec4 polygon_clip_rect;
flat out uint sprite_mode;
flat out uint sprite_alpha;
flat out uint object_flags;

void main() {
    vec4 position = view_matrix * vec4(in_position, 1.0);
    if (in_render_flags.x > 0.5) {
        // Sprite coordinates are absolute native-screen pixels with a
        // top-left origin, unlike direct polygon coordinates which are
        // centred around the System 22 viewport.
        gl_Position = vec4(position.x / 320.0 - 1.0,
                           1.0 - position.y / 240.0, 0.0, 1.0);
    } else if (in_direct > 0.5) {
        gl_Position = vec4(position.x / 320.0, position.y / 240.0,
                           clamp(1.0 / max(position.z, 1.0), 0.0, 1.0),
                           1.0);
    } else {
        // Preserve camera-space Z as homogeneous W. OpenGL then performs the
        // same perspective divide while clipping edges that cross the near
        // plane, avoiding the huge coordinates produced by a manual divide.
        float z = position.z;
        gl_Position = vec4((position.x + in_viewport.x * z) / 320.0,
                           (position.y - in_viewport.y * z) / 240.0,
                           z - 0.00002, z);
    }
    u_over_z = in_u_over_z;
    v_over_z = in_v_over_z;
    brightness_over_z = in_brightness_over_z;
    one_over_z = in_one_over_z;
    fog_factor = in_fog;
    palette_base = uint(in_palette.x);
    alpha_palette = uint(in_palette.y);
    color_mode = uint(in_color_mode);
    texture_bank = uint(in_texture_bank);
    polygon_fog_color = in_fog_color;
    polygon_clip_rect = in_clip_rect;
    sprite_mode = uint(in_render_flags.x);
    sprite_alpha = uint(in_render_flags.y);
    object_flags = uint(in_render_flags.z);
}
)GLSL";

constexpr const char* fragment_shader_source = R"GLSL(
#version 430 core
noperspective in float u_over_z;
noperspective in float v_over_z;
noperspective in float brightness_over_z;
noperspective in float one_over_z;
noperspective in float fog_factor;
flat in uint palette_base;
flat in uint alpha_palette;
flat in uint color_mode;
flat in uint texture_bank;
flat in vec3 polygon_fog_color;
flat in vec4 polygon_clip_rect;
flat in uint sprite_mode;
flat in uint sprite_alpha;
flat in uint object_flags;

uniform usampler2DArray texture_tiles;
uniform usampler2D texture_map;
uniform usampler2D texture_attr;
uniform usampler2DArray palette_data;
uniform usampler2DArray sprite_tiles;
uniform uint texture_address_base;
uniform uint texture_tile_high_bit_from_attr;
uniform bool super_system22;
uniform uvec3 super_fade_color;
uniform uint super_fade_factor;
uniform uint super_mixer_flags;
uniform uvec3 super_poly_fade_color;
uniform bool super_poly_fade_enabled;
uniform uint super_poly_alpha_color;
uniform uint super_poly_alpha_pen;
uniform uint super_poly_alpha_factor;
layout(location = 0, index = 0) out vec4 output_color;
layout(location = 0, index = 1) out vec4 output_blend;

vec3 apply_super_fade(vec3 rgb, uint target_flag) {
    if (!super_system22 || super_fade_factor == 0u ||
        (super_mixer_flags & target_flag) == 0u)
        return rgb;
    return mix(rgb, vec3(super_fade_color) / 255.0,
               float(super_fade_factor) / 255.0);
}

float source_opacity(uint pen) {
    if (!super_system22) return 1.0;
    uint alpha = sprite_mode != 0u ? 255u - sprite_alpha :
                                        255u - super_poly_alpha_factor;
    if (alpha == 255u ||
        (alpha_palette == super_poly_alpha_color &&
         pen != super_poly_alpha_pen))
        return 1.0;
    // MAME and the board mix two eight-bit channels with a denominator of
    // 256. Opaque pixels bypass this path, so 255/256 remains intentional.
    return float(alpha) / 256.0;
}

void write_scene_color(vec3 rgb, uint pen, float over_text) {
    // Dual-source blending keeps the source opacity independent from the
    // framebuffer alpha component, which stores System 22 text priority.
    output_color = vec4(rgb, over_text);
    output_blend = vec4(0.0, 0.0, 0.0, source_opacity(pen));
}

void main() {
    int screen_x = int(gl_FragCoord.x);
    int screen_y = 479 - int(gl_FragCoord.y);
    if (screen_x < int(polygon_clip_rect.x) ||
        screen_x > int(polygon_clip_rect.y) ||
        screen_y < int(polygon_clip_rect.z) ||
        screen_y > int(polygon_clip_rect.w))
        discard;

    float q = max(one_over_z, 0.0000001);
    if (sprite_mode != 0u) {
        uint ix = uint(clamp(int(u_over_z / q), 0, 31));
        uint iy = uint(clamp(int(v_over_z / q), 0, 31));
        uint address = texture_bank * 1024u + iy * 32u + ix;
        uint layer = address >> 20u;
        uint layer_offset = address & 0xfffffu;
        uint pen = texelFetch(sprite_tiles,
            ivec3(int(layer_offset & 0x3ffu),
                  int(layer_offset >> 10u), int(layer)), 0).r;
        if (pen == 0xffu) discard;

        uint palette_index = palette_base + pen;
        ivec2 palette_coord = ivec2(int(palette_index & 0xffu),
                                    int((palette_index >> 8u) & 0x7fu));
        vec3 rgb = vec3(
            texelFetch(palette_data, ivec3(palette_coord, 0), 0).r,
            texelFetch(palette_data, ivec3(palette_coord, 1), 0).r,
            texelFetch(palette_data, ivec3(palette_coord, 2), 0).r) / 255.0;
        rgb = mix(rgb, polygon_fog_color, clamp(fog_factor, 0.0, 1.0));
        rgb = apply_super_fade(rgb, 0x02u);
        write_scene_color(rgb, pen,
                          (color_mode & 1u) != 0u ? 1.0 : 0.0);
        return;
    }
    uint tx = uint(int(u_over_z / q)) & 0xfffu;
    uint ty = (uint(int(v_over_z / q)) & 0xfffu) | (texture_bank << 12u);
    // System 22 tilemap addressing: the 12-bit Y coordinate occupies the
    // upper 12 bits after its tile-row conversion, not bits 8..19.
    uint tile_offset = ((ty << 4u) & 0xfff00u) | (tx >> 4u);

    uint tile = texelFetch(texture_map,
        ivec2(int(tile_offset & 0xffu), int((tile_offset >> 8u) & 0xfffu)), 0).r;
    uint packed_index = tile_offset >> 1u;
    uint packed_attr = texelFetch(texture_attr,
        ivec2(int(packed_index & 0x3ffu), int(packed_index >> 10u)), 0).r;
    uint attr = ((tile_offset & 1u) == 0u) ? (packed_attr >> 4u) :
                                                 (packed_attr & 0x0fu);
    if (texture_tile_high_bit_from_attr != 0u && (attr & 1u) == 0u)
        tile = (tile & 0x3fffu) | 0x8000u;

    uint ix = tx & 0x0fu;
    uint iy = ty & 0x0fu;
    if ((attr & 4u) != 0u) ix = 15u - ix;
    if ((attr & 2u) != 0u) iy = 15u - iy;
    if ((attr & 8u) != 0u) {
        uint swap_value = ix;
        ix = iy;
        iy = swap_value;
    }

    uint pen = 0u;
    if (object_flags == 0u) {
        uint tile_address = (tile << 8u) | (iy << 4u) | ix;
        if (tile_address >= texture_address_base) {
            uint relative = tile_address - texture_address_base;
            uint layer = relative >> 21u;
            uint layer_offset = relative & 0x1fffffu;
            pen = texelFetch(texture_tiles,
                ivec3(int(layer_offset & 0x7ffu),
                      int(layer_offset >> 11u), int(layer)), 0).r;
        }
    }

    uint effective_color_mode = object_flags != 0u ? 0u : color_mode;
    uint pen_base = 0u;
    uint pen_mask = 0xffu;
    uint pen_shift = 0u;
    if ((effective_color_mode & 4u) != 0u) {
        pen_base = 0xecu + ((effective_color_mode & 8u) << 1u);
        pen_mask = 3u;
        pen_shift = 2u * ((~effective_color_mode) & 3u);
    } else if ((effective_color_mode & 2u) != 0u) {
        pen_base = 0xe0u + ((effective_color_mode & 8u) << 1u);
        pen_mask = 15u;
        pen_shift = 4u * ((~effective_color_mode) & 1u);
    }

    uint palette_index = palette_base + pen_base +
                         ((pen >> pen_shift) & pen_mask);
    ivec2 palette_coord = ivec2(int(palette_index & 0xffu),
                                int((palette_index >> 8u) & 0x7fu));
    vec3 rgb = vec3(
        texelFetch(palette_data, ivec3(palette_coord, 0), 0).r,
        texelFetch(palette_data, ivec3(palette_coord, 1), 0).r,
        texelFetch(palette_data, ivec3(palette_coord, 2), 0).r) / 255.0;
    float shade = max(brightness_over_z / q, 0.0) / 64.0;
    bool shade_enabled = (object_flags & 6u) == 0u;
    // Super System 22 shades before fog; the original board shades after it.
    // The shade is 8.6 fixed point, with 0x40 representing unity.
    if (super_system22 && shade_enabled)
        rgb = clamp(rgb * shade, 0.0, 1.0);
    rgb = mix(rgb, polygon_fog_color, clamp(fog_factor, 0.0, 1.0));
    if (!super_system22 && shade_enabled)
        rgb = clamp(rgb * shade, 0.0, 1.0);
    if (super_system22 && super_poly_fade_enabled)
        rgb = clamp(rgb * (vec3(super_poly_fade_color) / 256.0),
                    0.0, 1.0);
    rgb = apply_super_fade(rgb, 0x01u);
    // Non-Super System 22 keeps a one-bit per-pixel polygon-over-text flag.
    // Store the flag in the otherwise unused framebuffer alpha component;
    // the following text pass uses destination-alpha blending to honour it.
    float over_text = ((color_mode & 7u) == 1u) ? 1.0 : 0.0;
    write_scene_color(rgb, pen, over_text);
}
)GLSL";

constexpr const char* text_vertex_shader_source = R"GLSL(
#version 430 core

void main() {
    const vec2 positions[4] = vec2[4](
        vec2(-1.0, -1.0), vec2(1.0, -1.0),
        vec2(-1.0,  1.0), vec2(1.0,  1.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
}
)GLSL";

constexpr const char* text_fragment_shader_source = R"GLSL(
#version 430 core
uniform usampler2D character_data;
uniform usampler2D text_map;
uniform usampler2DArray palette_data;
uniform ivec2 text_scroll;
uniform int text_palette_base;
uniform bool super_system22;
uniform uvec3 super_fade_color;
uniform uint super_fade_factor;
uniform uint super_mixer_flags;
out vec4 output_color;

void main() {
    int screen_x = int(gl_FragCoord.x);
    int screen_y = 479 - int(gl_FragCoord.y);
    int source_x = (screen_x + text_scroll.x) & 0x3ff;
    int source_y = (screen_y + text_scroll.y) & 0x3ff;
    uint entry = texelFetch(text_map,
        ivec2((source_x >> 4) & 0x3f, (source_y >> 4) & 0x3f), 0).r;

    uint pixel_x = uint(source_x & 0x0f);
    uint pixel_y = uint(source_y & 0x0f);
    if ((entry & 0x0400u) != 0u) pixel_x = 15u - pixel_x;
    if ((entry & 0x0800u) != 0u) pixel_y = 15u - pixel_y;
    uint character = entry & 0x03ffu;
    uint byte_address = character * 128u + pixel_y * 8u + (pixel_x >> 1u);
    uint packed_byte = texelFetch(character_data,
        ivec2(int(byte_address & 0x1ffu), int(byte_address >> 9u)), 0).r;
    // Character RAM is addressed big-endian by the 68020.  MAME's
    // WORD2_XOR_BE layout therefore decodes the high nibble first.
    uint pen = ((pixel_x & 1u) == 0u) ? (packed_byte >> 4u) :
                                                  (packed_byte & 0x0fu);
    if (pen == 0x0fu) discard;

    uint palette_index = uint(text_palette_base) + ((entry >> 12u) * 16u) + pen;
    ivec2 palette_coord = ivec2(int(palette_index & 0xffu),
                                int((palette_index >> 8u) & 0x7fu));
    vec3 rgb = vec3(
        texelFetch(palette_data, ivec3(palette_coord, 0), 0).r,
        texelFetch(palette_data, ivec3(palette_coord, 1), 0).r,
        texelFetch(palette_data, ivec3(palette_coord, 2), 0).r) / 255.0;
    if (super_system22 && super_fade_factor != 0u &&
        (super_mixer_flags & 0x02u) != 0u)
        rgb = mix(rgb, vec3(super_fade_color) / 255.0,
                  float(super_fade_factor) / 255.0);
    output_color = vec4(rgb, 1.0);
}
)GLSL";

constexpr const char* post_vertex_shader_source = text_vertex_shader_source;

constexpr const char* post_fragment_shader_source = R"GLSL(
#version 430 core
uniform sampler2D scene_color;
uniform usampler2DArray gamma_data;
uniform uvec3 screen_fade;
uniform bool super_system22;
uniform bool apply_board_color;
uniform bool flip_y;
uniform bool linear_filtering;
uniform ivec2 output_size;
uniform ivec2 output_origin;
out vec4 output_color;

uint apply_fade(uint component, uint factor) {
    // MAME's non-Super System 22 mixer seeds a zero component before a
    // greater-than-unity fade so black can brighten, then performs 8.8 fixed
    // point multiplication and clamps to one byte.
    if (factor > 0x100u && component == 0u) component = 1u;
    return min((component * factor) >> 8u, 255u);
}

// Sharp bilinear, matching shaders/arcade_post.frag. Plain bilinear blends
// across a whole source texel, which is what makes an arcade raster look soft
// once it is scaled by a non-integer factor. This keeps each texel's interior
// flat - as nearest does - and confines the blend to a single output pixel at
// the texel boundary, so the picture stays crisp without the uneven pixel
// widths nearest produces.
vec2 sharp_texel_coordinate(vec2 uv, vec2 source_size) {
    vec2 scale = max(vec2(output_size) / source_size, vec2(1.0));
    vec2 texel = uv * source_size;
    vec2 base = floor(texel);
    vec2 offset = texel - base;
    vec2 flat_range = vec2(0.5) - vec2(0.5) / scale;
    vec2 centre_distance = offset - vec2(0.5);
    vec2 ramp =
        (centre_distance - clamp(centre_distance, -flat_range, flat_range)) *
            scale + vec2(0.5);
    return (base + ramp) / source_size;
}

void main() {
    ivec2 source_size = textureSize(scene_color, 0);
    vec2 output_coordinate = gl_FragCoord.xy - vec2(output_origin);
    vec3 sampled;
    if (linear_filtering) {
        // Sample at destination pixel centres.  The previous integer-only
        // texelFetch path made non-integer PS2 scaling repeat rows unevenly,
        // visibly squeezing fine HUD text even when LINEAR was selected.
        vec2 uv = output_coordinate / vec2(output_size);
        if (flip_y) uv.y = 1.0 - uv.y;
        sampled = texture(scene_color,
                          sharp_texel_coordinate(uv, vec2(source_size))).rgb;
    } else {
        ivec2 coordinate = ivec2(output_coordinate * vec2(source_size) /
                                 vec2(output_size));
        coordinate = clamp(coordinate, ivec2(0), source_size - ivec2(1));
        if (flip_y) coordinate.y = source_size.y - 1 - coordinate.y;
        sampled = texelFetch(scene_color, coordinate, 0).rgb;
    }
    uvec3 pixel = uvec3(round(clamp(
        sampled, 0.0, 1.0) * 255.0));
    if (!apply_board_color) {
        output_color = vec4(vec3(pixel) / 255.0, 1.0);
        return;
    }

    if (!super_system22) {
        pixel = uvec3(apply_fade(pixel.r, screen_fade.r),
                      apply_fade(pixel.g, screen_fade.g),
                      apply_fade(pixel.b, screen_fade.b));
    }

    uvec3 corrected = uvec3(
        texelFetch(gamma_data, ivec3(int(pixel.r), 0, 0), 0).r,
        texelFetch(gamma_data, ivec3(int(pixel.g), 0, 1), 0).r,
        texelFetch(gamma_data, ivec3(int(pixel.b), 0, 2), 0).r);
    output_color = vec4(vec3(corrected) / 255.0, 1.0);
}
)GLSL";

constexpr const char* settings_vertex_shader_source = R"GLSL(
#version 430 core
out vec2 texture_coordinate;
void main() {
    const vec2 positions[4] = vec2[4](
        vec2(-1.0, -1.0), vec2(1.0, -1.0),
        vec2(-1.0,  1.0), vec2(1.0,  1.0));
    const vec2 coordinates[4] = vec2[4](
        vec2(0.0, 1.0), vec2(1.0, 1.0),
        vec2(0.0, 0.0), vec2(1.0, 0.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
    texture_coordinate = coordinates[gl_VertexID];
}
)GLSL";

constexpr const char* settings_fragment_shader_source = R"GLSL(
#version 430 core
in vec2 texture_coordinate;
uniform sampler2D settings_texture;
out vec4 output_color;
void main() {
    output_color = texture(settings_texture, texture_coordinate);
}
)GLSL";

constexpr int settings_width = 560;
constexpr int settings_height = 510;
constexpr int settings_row_top = 58;
constexpr int settings_row_height = 29;
constexpr int settings_row_count = 15;
constexpr int pause_menu_row_count = 6;
constexpr int menu_axis_threshold = 16000;

uint32_t compile_shader(GLenum type, const char* source) {
    const uint32_t shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success == GL_TRUE) return shader;

    char log[2048]{};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    std::fprintf(stderr, "OpenGL shader compilation failed: %s\n", log);
    glDeleteShader(shader);
    return 0;
}

struct clipped_poly_vertex {
    float x;
    float y;
    float z;
    float u;
    float v;
    float bri;
};

void append_quad(std::vector<draw_vertex>& vertices, const polygon_object& polygon) {
    std::array<clipped_poly_vertex, 6> clipped{};
    int clipped_count = 0;

    if (polygon.direct) {
        clipped_count = 4;
        for (int index = 0; index < 4; ++index) {
            const poly_vertex& source = polygon.vertices[index];
            clipped[index] = {source.x, source.y, source.z,
                              static_cast<float>(source.u),
                              static_cast<float>(source.v),
                              static_cast<float>(source.bri)};
        }
    } else {
        // Match MAME's zclip_if_less(): clip the complete quad in camera
        // space before triangulation.  Splitting first loses a foreground
        // half of long road quads as they cross the camera plane.
        constexpr float near_z = 0.00001f;
        auto source_vertex = [&](int index) {
            const poly_vertex& source = polygon.vertices[index];
            return clipped_poly_vertex{
                source.x, source.y, source.z,
                static_cast<float>(source.u),
                static_cast<float>(source.v),
                static_cast<float>(source.bri)};
        };
        clipped_poly_vertex previous = source_vertex(3);
        bool previous_clipped = previous.z < near_z;
        for (int index = 0; index < 4; ++index) {
            const clipped_poly_vertex current = source_vertex(index);
            const bool current_clipped = current.z < near_z;
            if (current_clipped != previous_clipped) {
                const float fraction =
                    (near_z - previous.z) / (current.z - previous.z);
                auto interpolate = [&](float first, float second) {
                    return first + fraction * (second - first);
                };
                clipped[clipped_count++] = {
                    interpolate(previous.x, current.x),
                    interpolate(previous.y, current.y), near_z,
                    interpolate(previous.u, current.u),
                    interpolate(previous.v, current.v),
                    interpolate(previous.bri, current.bri),
                };
            }
            if (!current_clipped) clipped[clipped_count++] = current;
            previous = current;
            previous_clipped = current_clipped;
        }
    }

    if (clipped_count < 3) return;

    const uint32_t color_extension = (polygon.cz_adjust >> 16) & 0x7fu;
    const uint32_t alpha_palette = polygon.color & 0x7fu;
    uint32_t palette_base = alpha_palette << 8;
    if (polygon.objectflags != 0) {
        if ((polygon.objectflags & 6u) != 0)
            palette_base = polygon.cz_adjust & 0x7fffu;
        else
            palette_base += color_extension & (polygon.color | 0x1fu);
    }

    auto make_vertex = [&](const clipped_poly_vertex& source) {
        const float q = polygon.direct ? source.z :
            1.0f / source.z;
        draw_vertex result{};
        result.x = source.x;
        result.y = source.y;
        result.z = source.z;
        result.u_over_z = (source.u + 0.5f) * q;
        result.v_over_z = (source.v + 0.5f) * q;
        result.brightness_over_z = (source.bri + 0.5f) * q;
        result.one_over_z = q;
        result.palette_base = static_cast<float>(palette_base);
        result.alpha_palette = static_cast<float>(alpha_palette);
        result.color_mode = static_cast<float>(polygon.cmode & 0x0f);
        result.texture_bank = static_cast<float>(
            polygon.sprite ? polygon.sprite_tile :
                             (polygon.texturebank & 0x0f));
        result.direct = polygon.direct ? 1.0f : 0.0f;
        result.fog = normalized_byte(polygon.fog_factor);
        result.fog_color_r = normalized_byte(polygon.fog_r);
        result.fog_color_g = normalized_byte(polygon.fog_g);
        result.fog_color_b = normalized_byte(polygon.fog_b);
        result.viewport_x = polygon.viewport_x;
        result.viewport_y = polygon.viewport_y;
        result.clip_left = polygon.clip_left;
        result.clip_right = polygon.clip_right;
        result.clip_top = polygon.clip_top;
        result.clip_bottom = polygon.clip_bottom;
        result.sprite = polygon.sprite ? 1.0f : 0.0f;
        result.sprite_alpha = static_cast<float>(polygon.sprite_alpha);
        result.object_flags = static_cast<float>(polygon.objectflags);
        return result;
    };

    for (int index = 1; index + 1 < clipped_count; ++index) {
        vertices.push_back(make_vertex(clipped[0]));
        vertices.push_back(make_vertex(clipped[index]));
        vertices.push_back(make_vertex(clipped[index + 1]));
    }
}
} // namespace

polygon_renderer_gpu::polygon_renderer_gpu() = default;

polygon_renderer_gpu::~polygon_renderer_gpu() {
    shutdown();
}

namespace {
// Reported a handful of times, not every frame: enough to see whether a path
// is reached at all without filling the file.
bool log_first(int& counter, int limit = 3) { return ++counter <= limit; }
} // namespace

bool polygon_renderer_gpu::initialize(const emulator_settings& settings) {
    if (m_initialized) return true;

    // Give Wayland compositors and X11 window managers a stable identity.
    // Hyprland can then float only this emulator window; tiled clients are
    // deliberately not allowed to honor SDL_SetWindowSize requests.
    SDL_SetHint("SDL_APP_ID", "WhittyArcade");
    SDL_SetHint("SDL_VIDEO_WAYLAND_WMCLASS", "WhittyArcade");
    SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "WhittyArcade");

    if (!whitty_platform::video_available()) {
        printf("No display available, initializing headless mode\n");
        printf("GPU renderer initialized (headless)\n");
        m_headless = true;
        m_initialized = true;
        return true;
    }

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        whitty_wall_log::note("SDL video init FAILED: %s", SDL_GetError());
        std::fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return false;
    }
    std::printf("SDL video driver: %s\n",
                SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() :
                                              "unknown");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    const auto initial_size = bounded_window_size(nullptr,
                                                   settings.window_width);
    const int initial_width = initial_size[0];
    const int initial_height = initial_size[1];
    const bool alternate_output = settings.renderer != renderer_backend::opengl;
    const SDL_WindowFlags output_visibility = alternate_output ?
        SDL_WINDOW_HIDDEN :
        (settings.fullscreen && settings.display_index < 0 ?
             SDL_WINDOW_FULLSCREEN : 0);
    SDL_Window* window = SDL_CreateWindow(
        alternate_output ? "WhittyArcade Render Worker" : "WhittyArcade",
        initial_width, initial_height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
            output_visibility);

    if (!window) {
        std::fprintf(stderr, "Window creation failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        return false;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        std::fprintf(stderr, "OpenGL context creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
        return false;
    }

    m_ctx = std::make_unique<opengl_context>(
        opengl_context{window, nullptr, gl_ctx, initial_width, initial_height});
    SDL_GetWindowSizeInPixels(window, &m_ctx->width, &m_ctx->height);

    if (!m_ctx->make_current()) {
        std::fprintf(stderr, "Could not activate OpenGL context: %s\n",
                     SDL_GetError());
        shutdown();
        return false;
    }
    glewExperimental = GL_TRUE;
    const GLenum glew_result = glewInit();
    // On native Wayland GLEW can finish loading the core OpenGL entry points
    // and then report that no GLX display exists. GLX is not used by SDL's
    // Wayland context, so accept that specific result after checking the
    // required OpenGL version below.
    if (glew_result != GLEW_OK &&
        glew_result != GLEW_ERROR_NO_GLX_DISPLAY) {
        std::fprintf(stderr, "GLEW initialization failed (%u): %s\n",
                     static_cast<unsigned>(glew_result),
                     reinterpret_cast<const char*>(
                         glewGetErrorString(glew_result)));
        shutdown();
        return false;
    }
    if (!GLEW_VERSION_4_3) {
        std::fprintf(stderr,
                     "OpenGL 4.3 is unavailable after GLEW initialization\n");
        shutdown();
        return false;
    }
    // glewInit can leave GL_INVALID_ENUM set when probing a core context.
    while (glGetError() != GL_NO_ERROR) {}

    if (!configure_output_backend(settings.renderer, settings)) {
        std::fprintf(stderr, "Requested %s output could not be initialized\n",
                     renderer_backend_name(settings.renderer));
        shutdown();
        return false;
    }
    m_ctx->make_current();

    printf("OpenGL version: %s\n", glGetString(GL_VERSION));

    // System 22 uses four banks while Rave Racer populates all eight. Integer
    // textures preserve the tile pen, tile index, transform attribute and
    // planar palette bytes exactly for the fragment-shader indirection.
    glGenTextures(1, &m_texture_buffer);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_texture_buffer);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8UI,
        SYSTEM22_TEXTURE_BANK_WIDTH, SYSTEM22_TEXTURE_BANK_HEIGHT, 8,
        0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Super System 22 stores raw 32x32x8bpp sprite tiles. Sixteen 1 MiB
    // layers retain the ROM byte addressing directly in the sprite shader.
    glGenTextures(1, &m_sprite_texture);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_sprite_texture);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8UI, 1024, 1024, 16,
                 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &m_character_texture);
    glBindTexture(GL_TEXTURE_2D, m_character_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, 512, 256, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &m_textmap_texture);
    glBindTexture(GL_TEXTURE_2D, m_textmap_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, 64, 64, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_SHORT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &m_gamma_texture);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_gamma_texture);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8UI, 256, 1, 3, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // Super System 22 drives its display DAC directly and has no Ridge Racer
    // gamma PROM set. Seed an identity table so those games render normally;
    // base System 22 replaces it with the three validated PROMs at load time.
    std::array<uint8_t, 256 * 3> identity_gamma{};
    for (std::size_t component = 0; component < 3; ++component)
        for (std::size_t value = 0; value < 256; ++value)
            identity_gamma[component * 256 + value] =
                static_cast<uint8_t>(value);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 256, 1, 3,
                    GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                    identity_gamma.data());

    // Render the scene and the one-bit over-character priority into a known
    // RGBA8 target. A final fullscreen pass applies the board's fade and
    // three gamma PROMs before presenting it to the window framebuffer.
    glGenTextures(1, &m_output_texture);
    glBindTexture(GL_TEXTURE_2D, m_output_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, SYSTEM22_SCREEN_WIDTH,
                 SYSTEM22_SCREEN_HEIGHT, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &m_scene_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_scene_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_output_texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "OpenGL scene framebuffer is incomplete\n");
        shutdown();
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_ctx->width, m_ctx->height);

    glGenTextures(1, &m_model2_scene_texture);
    glBindTexture(GL_TEXTURE_2D, m_model2_scene_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, model2_gpu_frame::width,
                 model2_gpu_frame::height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &m_model2_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_model2_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_model2_scene_texture, 0);
    glGenRenderbuffers(1, &m_model2_depth_buffer);
    glBindRenderbuffer(GL_RENDERBUFFER, m_model2_depth_buffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
                          model2_gpu_frame::width, model2_gpu_frame::height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, m_model2_depth_buffer);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "OpenGL Model 2 framebuffer is incomplete\n");
        shutdown();
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    const auto create_model2_rgba_texture = [](uint32_t& texture) {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, model2_gpu_frame::width,
                     model2_gpu_frame::height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    create_model2_rgba_texture(m_model2_base_texture);
    create_model2_rgba_texture(m_model2_foreground_texture);

    glGenTextures(1, &m_model2_sheet_texture);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_model2_sheet_texture);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8UI, 1024, 1024, 2, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &m_model2_luma_texture);
    glBindTexture(GL_TEXTURE_2D, m_model2_luma_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, 256, 128, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &m_model2_color_texture);
    glBindTexture(GL_TEXTURE_2D, m_model2_color_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 1024, 64, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &m_tilemap_texture);
    glBindTexture(GL_TEXTURE_2D, m_tilemap_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, 256, 4096, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_SHORT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &m_tileattr_texture);
    glBindTexture(GL_TEXTURE_2D, m_tileattr_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, 1024, 512, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &m_palette_texture);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_palette_texture);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8UI, 256, 128, 3, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // Create uniform buffer for camera data
    glGenBuffers(1, &m_camera_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, m_camera_ubo);
    glBufferData(GL_UNIFORM_BUFFER, 20 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    if (!create_graphics_pipeline() || !create_model2_pipeline() ||
        !create_text_pipeline() ||
        !create_post_pipeline() || !create_settings_overlay()) {
        whitty_wall_log::note("renderer pipeline setup FAILED");
        shutdown();
        return false;
    }
    // System 22's renderer consumes the DSP's 24-bit priority order; it does
    // not resolve polygons with a conventional host Z buffer.
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_network_video = std::make_unique<network_video_link>();
    apply_display_settings(settings);
    printf("GPU renderer initialized\n");
    whitty_wall_log::note("renderer up: wanted=%d actual=%d window=%d "
                          "headless=%d",
                          static_cast<int>(settings.renderer),
                          m_alternate_presenter
                              ? static_cast<int>(m_alternate_presenter->backend())
                              : -1,
                          m_ctx ? 1 : 0, m_headless ? 1 : 0);
    m_initialized = true;
    return true;
}

void polygon_renderer_gpu::shutdown() {
    // Stop the transport before deleting the GL texture it feeds.
    m_network_video.reset();
    if (!m_headless && m_ctx) {
        m_ctx->make_current();
        SDL_SetCursor(SDL_GetDefaultCursor());
        for (SDL_Cursor*& cursor : m_lightgun_cursors) {
            if (cursor) SDL_DestroyCursor(cursor);
            cursor = nullptr;
        }
        destroy_graphics_pipeline();
        destroy_model2_pipeline();
        destroy_text_pipeline();
        destroy_post_pipeline();
        destroy_settings_overlay();
        if (m_texture_buffer) glDeleteTextures(1, &m_texture_buffer);
        if (m_sprite_texture) glDeleteTextures(1, &m_sprite_texture);
        if (m_tilemap_texture) glDeleteTextures(1, &m_tilemap_texture);
        if (m_tileattr_texture) glDeleteTextures(1, &m_tileattr_texture);
        if (m_palette_texture) glDeleteTextures(1, &m_palette_texture);
        if (m_character_texture) glDeleteTextures(1, &m_character_texture);
        if (m_textmap_texture) glDeleteTextures(1, &m_textmap_texture);
        if (m_gamma_texture) glDeleteTextures(1, &m_gamma_texture);
        if (m_output_texture) glDeleteTextures(1, &m_output_texture);
        if (m_external_frame_texture)
            glDeleteTextures(1, &m_external_frame_texture);
        if (m_network_frame_texture)
            glDeleteTextures(1, &m_network_frame_texture);
        if (m_model2_scene_texture)
            glDeleteTextures(1, &m_model2_scene_texture);
        if (m_model2_base_texture)
            glDeleteTextures(1, &m_model2_base_texture);
        if (m_model2_foreground_texture)
            glDeleteTextures(1, &m_model2_foreground_texture);
        if (m_model2_sheet_texture)
            glDeleteTextures(1, &m_model2_sheet_texture);
        if (m_model2_luma_texture)
            glDeleteTextures(1, &m_model2_luma_texture);
        if (m_model2_color_texture)
            glDeleteTextures(1, &m_model2_color_texture);
        if (m_scene_framebuffer) glDeleteFramebuffers(1, &m_scene_framebuffer);
        if (m_model2_framebuffer)
            glDeleteFramebuffers(1, &m_model2_framebuffer);
        if (m_model2_depth_buffer)
            glDeleteRenderbuffers(1, &m_model2_depth_buffer);
        if (m_present_texture) glDeleteTextures(1, &m_present_texture);
        if (m_present_framebuffer)
            glDeleteFramebuffers(1, &m_present_framebuffer);
        if (m_index_buffer) glDeleteBuffers(1, &m_index_buffer);
        if (m_camera_ubo) glDeleteBuffers(1, &m_camera_ubo);
        if (m_alternate_presenter) {
            m_alternate_presenter->shutdown();
            m_alternate_presenter.reset();
        }
        m_ctx->destroy();
        m_ctx.reset();
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    }
    m_vertex_buffer = m_index_buffer = m_texture_buffer = 0;
    m_sprite_texture = 0;
    m_tilemap_texture = m_tileattr_texture = m_palette_texture = 0;
    m_character_texture = m_textmap_texture = 0;
    m_gamma_texture = m_output_texture = m_external_frame_texture = 0;
    m_network_frame_texture = 0;
    m_model2_scene_texture = m_model2_base_texture = 0;
    m_model2_foreground_texture = m_model2_sheet_texture = 0;
    m_model2_luma_texture = m_model2_color_texture = 0;
    m_scene_framebuffer = 0;
    m_model2_framebuffer = m_model2_depth_buffer = 0;
    m_present_texture = m_present_framebuffer = 0;
    m_present_width = m_present_height = 0;
    m_external_frame_width = m_external_frame_height = 0;
    m_network_frame_width = m_network_frame_height = 0;
    m_network_frame_display_width = m_network_frame_display_height = 0;
    m_network_frame_flip_y = m_network_frame_board_color = false;
    m_network_frame_valid = false;
    m_network_video_sequence = 0;
    m_network_capture_divider = 0;
    m_camera_ubo = 0;
    m_vertex_array = m_graphics_program = 0;
    m_model2_vertex_buffer = m_model2_vertex_array = m_model2_program = 0;
    m_model2_overlay_array = m_model2_overlay_program = 0;
    m_text_vertex_array = m_text_program = 0;
    m_post_vertex_array = m_post_program = 0;
    m_settings_vertex_array = m_settings_program = m_settings_texture = 0;
    m_fps_texture = 0;
    m_temporal_shadow_polygons.clear();
    m_temporal_shadow_age = 2;
    m_initialized = false;
}

void polygon_renderer_gpu::update_lightgun_cursor() {
    if (m_headless || !m_ctx) return;
    SDL_Cursor* desired =
        m_lightgun_cursor_enabled && !m_settings_visible ?
            m_lightgun_cursors[m_lightgun_cursor_player] :
            SDL_GetDefaultCursor();
    if (desired) SDL_SetCursor(desired);
    SDL_ShowCursor();
}

void polygon_renderer_gpu::set_lightgun_cursor(bool enabled, uint8_t player) {
    if (m_headless || !m_ctx) return;
    player = std::min<uint8_t>(player, 1);
    if (enabled && !m_lightgun_cursors[player]) {
        constexpr int size = 44;
        constexpr int centre = size / 2;
        SDL_Surface* surface = SDL_CreateSurface(
            size, size, SDL_PIXELFORMAT_RGBA32);
        if (surface) {
            const SDL_PixelFormatDetails* fmt =
                SDL_GetPixelFormatDetails(surface->format);
            const uint32_t transparent = SDL_MapRGBA(
                fmt, nullptr, 0, 0, 0, 0);
            const uint32_t outline = SDL_MapRGBA(
                fmt, nullptr, 0, 0, 0, 255);
            constexpr std::array<std::array<uint8_t, 3>, 2> player_colours{{
                {{32, 224, 255}},
                {{255, 48, 112}},
            }};
            const auto& colour = player_colours[player];
            const uint32_t sight = SDL_MapRGBA(
                fmt, nullptr, colour[0], colour[1], colour[2], 255);
            SDL_FillSurfaceRect(surface, nullptr, transparent);

            // WhittyArcade's light-gun sight is generated here rather than
            // borrowed from a game or another emulator. Dark edging keeps
            // both vivid player colours readable against bright muzzle
            // flashes and dark scenery.
            if (SDL_LockSurface(surface)) {
                auto* pixels = static_cast<uint32_t*>(surface->pixels);
                const int stride = surface->pitch /
                                   static_cast<int>(sizeof(uint32_t));
                for (int y = 0; y < size; ++y) {
                    for (int x = 0; x < size; ++x) {
                        const int dx = x - centre;
                        const int dy = y - centre;
                        const int radius_squared = dx * dx + dy * dy;
                        if (radius_squared >= 108 && radius_squared <= 240)
                            pixels[y * stride + x] = outline;
                        if (radius_squared >= 139 && radius_squared <= 210)
                            pixels[y * stride + x] = sight;
                    }
                }
                SDL_UnlockSurface(surface);
            }
            const std::array<SDL_Rect, 4> outline_segments{{
                {1, centre - 3, 8, 7}, {36, centre - 3, 7, 7},
                {centre - 3, 1, 7, 8}, {centre - 3, 36, 7, 7},
            }};
            const std::array<SDL_Rect, 4> sight_segments{{
                {2, centre - 1, 7, 3}, {36, centre - 1, 6, 3},
                {centre - 1, 2, 3, 7}, {centre - 1, 36, 3, 6},
            }};
            for (const SDL_Rect& rectangle : outline_segments)
                SDL_FillSurfaceRect(surface, &rectangle, outline);
            for (const SDL_Rect& rectangle : sight_segments)
                SDL_FillSurfaceRect(surface, &rectangle, sight);
            SDL_Rect centre_outline{centre - 2, centre - 2, 5, 5};
            SDL_Rect centre_point{centre - 1, centre - 1, 3, 3};
            SDL_FillSurfaceRect(surface, &centre_outline, outline);
            SDL_FillSurfaceRect(surface, &centre_point, sight);
            m_lightgun_cursors[player] = SDL_CreateColorCursor(
                surface, centre, centre);
            SDL_DestroySurface(surface);
        }
    }
    if (enabled) m_lightgun_cursor_player = player;
    m_lightgun_cursor_enabled =
        enabled && m_lightgun_cursors[m_lightgun_cursor_player];
    update_lightgun_cursor();
}

arcade_host_action polygon_renderer_gpu::process_events() {
    if (m_headless) return arcade_host_action::continue_running;
    std::lock_guard<std::mutex> sdl_lock(arcade_sdl_mutex());
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        const bool key_down = event.type == SDL_EVENT_KEY_DOWN;
        // A compositor asking a column to close looks, one event later,
        // exactly like the user quitting: SDL turns the last window closing
        // into SDL_EVENT_QUIT. Recording it separates the two.
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            whitty_wall_log::note("compositor asked window %u to close",
                                  static_cast<unsigned>(event.window.windowID));
        if (event.type == SDL_EVENT_WINDOW_DESTROYED)
            whitty_wall_log::note("window %u destroyed",
                                  static_cast<unsigned>(event.window.windowID));
        const arcade_host_action host_action = classify_arcade_host_event(
            event.type == SDL_EVENT_QUIT,
            key_down && event.key.key == SDLK_ESCAPE,
            key_down && event.key.repeat != 0);
        if (host_action != arcade_host_action::continue_running) {
            whitty_wall_log::note(
                "host stop requested: sdl event %u (quit=%d escape=%d)",
                static_cast<unsigned>(event.type),
                event.type == SDL_EVENT_QUIT ? 1 : 0,
                key_down && event.key.key == SDLK_ESCAPE ? 1 : 0);
            return host_action;
        }
        // On an arcade wall every column keeps running, but only the one you
        // are looking at should be heard. Following keyboard focus means the
        // flip needs no messaging between the processes: each mutes itself
        // when focus leaves it, and SDL delivers key events only to the
        // focused window.
        if (m_display_settings.wall_count > 1 &&
            (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
             event.type == SDL_EVENT_WINDOW_FOCUS_LOST)) {
            m_wall_column_active =
                event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
            m_wall_audio_changed = true;
            // Only the column that gained focus knows the wall has a new
            // big cabinet. Publishing it is what lets the other two shrink
            // themselves: each of them can see its own focus, but not whose
            // gain caused their loss.
            if (m_wall_column_active)
                whitty_window::publish_wall_focus(
                    m_display_settings.wall_slot);
        }
        if (!m_alternate_presenter &&
            (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
             event.type == SDL_EVENT_WINDOW_RESIZED) && m_ctx) {
            SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(m_ctx->window),
                                   &m_ctx->width, &m_ctx->height);
            m_ctx->width = std::max(m_ctx->width, 1);
            m_ctx->height = std::max(m_ctx->height, 1);
        }
        if (event.type == SDL_EVENT_KEY_DOWN) {
            const SDL_Keycode key = event.key.key;
            if (!event.key.repeat && key == SDLK_P) {
                if (m_paused) resume_game();
                else show_pause_menu();
                continue;
            }
            if (m_settings_visible) {
                if (key == SDLK_BACKSPACE) {
                    show_pause_menu();
                    continue;
                }
                if (m_pause_menu_visible) {
                    if (key == SDLK_UP)
                        select_pause_menu_row(-1);
                    else if (key == SDLK_DOWN)
                        select_pause_menu_row(1);
                    else if ((key == SDLK_RETURN || key == SDLK_SPACE) &&
                             activate_pause_menu())
                        return arcade_host_action::return_to_menu;
                    m_settings_texture_dirty = true;
                    continue;
                } else if (m_operator_menu_visible) {
                    if (key == SDLK_UP)
                        select_operator_row(-1);
                    else if (key == SDLK_DOWN)
                        select_operator_row(1);
                    else if (key == SDLK_LEFT)
                        adjust_operator_setting(-1, false);
                    else if (key == SDLK_RIGHT)
                        adjust_operator_setting(1, false);
                    else if (key == SDLK_RETURN || key == SDLK_SPACE)
                        adjust_operator_setting(1, true);
                    m_settings_texture_dirty = true;
                    continue;
                } else if (m_rom_menu_visible) {
                    const int count = static_cast<int>(m_rom_choices.size());
                    if (count > 0 && key == SDLK_UP)
                        select_rom_within_board(-1);
                    else if (count > 0 && key == SDLK_DOWN)
                        select_rom_within_board(1);
                    else if (count > 0 && key == SDLK_LEFT)
                        cycle_rom_board(-1);
                    else if (count > 0 && key == SDLK_RIGHT)
                        cycle_rom_board(1);
                    else if (count > 0 &&
                             (key == SDLK_RETURN || key == SDLK_SPACE)) {
                        m_selected_rom = m_rom_choices[m_rom_selection].path;
                        m_rom_selection_pending = true;
                        m_settings_visible = false;
                        m_rom_menu_visible = false;
                    }
                    m_settings_texture_dirty = true;
                    continue;
                }
                if (key == SDLK_UP)
                    m_settings_selection =
                        (m_settings_selection + settings_row_count - 1) %
                        settings_row_count;
                else if (key == SDLK_DOWN)
                    m_settings_selection =
                        (m_settings_selection + 1) % settings_row_count;
                else if (key == SDLK_LEFT)
                    adjust_setting(-1);
                else if (key == SDLK_RIGHT)
                    adjust_setting(1);
                else if (key == SDLK_RETURN || key == SDLK_SPACE)
                    adjust_setting(0);
                m_settings_texture_dirty = true;
                continue;
            }
            const SDL_Keymod modifiers = SDL_GetModState();
            if (!event.key.repeat && key == SDLK_RETURN &&
                (modifiers & SDL_KMOD_ALT) != 0) {
                m_display_settings.fullscreen =
                    !m_display_settings.fullscreen;
                apply_display_settings(m_display_settings);
                m_settings_changed = true;
                continue;
            }
            if (!event.key.repeat && (modifiers & SDL_KMOD_CTRL) != 0 &&
                (key == SDLK_EQUALS || key == SDLK_PLUS ||
                 key == SDLK_KP_PLUS || key == SDLK_MINUS ||
                 key == SDLK_KP_MINUS)) {
                const int direction = (key == SDLK_MINUS ||
                                       key == SDLK_KP_MINUS) ? -1 : 1;
                m_display_settings.window_width += direction * 20;
                apply_display_settings(m_display_settings);
                m_settings_changed = true;
                continue;
            }
        }
        if (m_settings_visible && event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            if (m_pause_menu_visible) {
                switch (event.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_DPAD_UP:
                    select_pause_menu_row(-1);
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                    select_pause_menu_row(1);
                    break;
                case SDL_GAMEPAD_BUTTON_SOUTH:
                    if (activate_pause_menu())
                        return arcade_host_action::return_to_menu;
                    break;
                case SDL_GAMEPAD_BUTTON_EAST:
                    resume_game();
                    break;
                default: break;
                }
                m_settings_texture_dirty = true;
                continue;
            } else if (m_operator_menu_visible) {
                switch (event.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_DPAD_UP:
                    select_operator_row(-1);
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                    select_operator_row(1);
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                    adjust_operator_setting(-1, false);
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                    adjust_operator_setting(1, false);
                    break;
                case SDL_GAMEPAD_BUTTON_SOUTH:
                    adjust_operator_setting(1, true);
                    break;
                case SDL_GAMEPAD_BUTTON_EAST:
                    show_pause_menu();
                    break;
                default: break;
                }
                m_settings_texture_dirty = true;
                continue;
            } else if (m_rom_menu_visible) {
                const int count = static_cast<int>(m_rom_choices.size());
                if (count > 0 && event.gbutton.button ==
                                     SDL_GAMEPAD_BUTTON_DPAD_UP)
                    select_rom_within_board(-1);
                else if (count > 0 && event.gbutton.button ==
                                          SDL_GAMEPAD_BUTTON_DPAD_DOWN)
                    select_rom_within_board(1);
                else if (count > 0 && event.gbutton.button ==
                                          SDL_GAMEPAD_BUTTON_DPAD_LEFT)
                    cycle_rom_board(-1);
                else if (count > 0 && event.gbutton.button ==
                                          SDL_GAMEPAD_BUTTON_DPAD_RIGHT)
                    cycle_rom_board(1);
                else if (count > 0 && event.gbutton.button ==
                                          SDL_GAMEPAD_BUTTON_SOUTH) {
                    m_selected_rom = m_rom_choices[m_rom_selection].path;
                    m_rom_selection_pending = true;
                    m_settings_visible = false;
                    m_rom_menu_visible = false;
                } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                    show_pause_menu();
                }
                m_settings_texture_dirty = true;
                continue;
            }
            switch (event.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP:
                m_settings_selection =
                    (m_settings_selection + settings_row_count - 1) %
                    settings_row_count;
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                m_settings_selection =
                    (m_settings_selection + 1) % settings_row_count;
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: adjust_setting(-1); break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: adjust_setting(1); break;
            case SDL_GAMEPAD_BUTTON_SOUTH: adjust_setting(0); break;
            case SDL_GAMEPAD_BUTTON_EAST: show_pause_menu(); break;
            default: break;
            }
            m_settings_texture_dirty = true;
        }
        if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
            (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ||
             event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
            int direction = event.gaxis.value < -menu_axis_threshold ? -1 :
                            event.gaxis.value > menu_axis_threshold ? 1 : 0;
            int& latch = event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ?
                m_menu_axis_x : m_menu_axis_y;
            if (!m_settings_visible) {
                latch = 0;
                continue;
            }
            if (direction == 0) {
                latch = 0;
                continue;
            }
            if (direction == latch) continue;
            latch = direction;
            if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
                if (m_pause_menu_visible)
                    select_pause_menu_row(direction);
                else if (m_operator_menu_visible)
                    select_operator_row(direction);
                else if (m_rom_menu_visible)
                    select_rom_within_board(direction);
                else
                    m_settings_selection =
                        (m_settings_selection +
                         (direction < 0 ? settings_row_count - 1 : 1)) %
                        settings_row_count;
            } else if (m_operator_menu_visible) {
                adjust_operator_setting(direction, false);
            } else if (m_rom_menu_visible) {
                cycle_rom_board(direction);
            } else if (!m_pause_menu_visible) {
                adjust_setting(direction);
            }
            m_settings_texture_dirty = true;
        }
    }
    update_lightgun_cursor();
    return arcade_host_action::continue_running;
}

void polygon_renderer_gpu::set_rom_choices(
    std::vector<rom_choice> choices) {
    m_rom_choices = std::move(choices);
    m_rom_selection = 0;
    for (std::size_t index = 0; index < m_rom_choices.size(); ++index) {
        if (m_rom_choices[index].label.find("[current]") != std::string::npos) {
            m_rom_selection = static_cast<int>(index);
            break;
        }
    }
    m_settings_texture_dirty = true;
}

void polygon_renderer_gpu::reset_session_ui() {
    resume_game();
    m_operator_actions.clear();
    m_controls_requested = false;
    m_rom_selection_pending = false;
}

void polygon_renderer_gpu::set_single_screen_only(bool enabled) {
    if (m_single_screen_only == enabled) return;
    m_single_screen_only = enabled;
    m_settings_texture_dirty = true;
    // The Vulkan/software presenters split their panes from the settings
    // they were last given, so the single-screen constraint has to reach
    // them as an output-mode change, not just a renderer-side flag.
    if (m_alternate_presenter)
        m_alternate_presenter->apply_settings(
            effective_presenter_settings(m_display_settings));
    refresh_output();
}

emulator_settings polygon_renderer_gpu::effective_presenter_settings(
        const emulator_settings& settings) const {
    emulator_settings effective = settings;
    if (m_single_screen_only) effective.output = output_mode::single;
    return effective;
}

void polygon_renderer_gpu::set_board_name(std::string short_name) {
    if (short_name == m_bezel_board) return;
    m_bezel_board = std::move(short_name);
    // The previous board's artwork must not frame this one.
    m_bezel_pixels.clear();
    if (m_alternate_presenter) m_alternate_presenter->set_bezel({});
    m_bezels.request(m_bezel_board);
}

void polygon_renderer_gpu::arm_title_capture(
        const std::string& short_name) {
    if (short_name.empty() || title_capture_exists(short_name)) return;
    // Thirty seconds of presented frames: past the self-tests and network
    // checks, into the title or attract, on every board here. The count
    // runs in the presenter, where every board's frames converge - the
    // CPU-side fallback below only serves the plain OpenGL path.
    if (m_alternate_presenter) {
        m_alternate_presenter->arm_title_capture(short_name, 1800);
        return;
    }
    m_title_capture_name = short_name;
    m_title_capture_countdown = 1800;
    whitty_wall_log::note("title capture armed for %s", short_name.c_str());
}

void polygon_renderer_gpu::set_window_visible(bool visible) {
    if (!m_ctx) return;
    SDL_Window* window = m_alternate_presenter ?
        static_cast<SDL_Window*>(m_alternate_presenter->window()) :
        static_cast<SDL_Window*>(m_ctx->window);
    if (window == nullptr) return;
    if (visible) {
        SDL_ShowWindow(window);
        // Raised as well as shown: coming back from a game that had the focus,
        // the launcher must be the thing being typed at again.
        SDL_RaiseWindow(window);
    } else {
        SDL_HideWindow(window);
    }
}

void polygon_renderer_gpu::set_cabinet_status(std::string status) {
    if (m_cabinet_status == status) return;
    m_cabinet_status = std::move(status);
    update_fps_texture(m_last_fps);
    if (m_ctx) {
        SDL_Window* window = m_alternate_presenter ?
            static_cast<SDL_Window*>(m_alternate_presenter->window()) :
            static_cast<SDL_Window*>(m_ctx->window);
        if (window) {
            const std::string title = m_cabinet_status.empty() ?
                "WhittyArcade" : "WhittyArcade - " + m_cabinet_status;
            SDL_SetWindowTitle(window, title.c_str());
            // Cabinet 2's window opens last and takes the desktop focus
            // with it, so the keyboard starts out driving player 2. The
            // moment the pair reports LINKED, cabinet 1 asks for the
            // focus back - once, so a player who deliberately switches
            // windows afterwards is left alone.
            if (!m_raised_when_linked &&
                m_cabinet_status.find("LINKED") != std::string::npos) {
                const char* node_text =
                    std::getenv("WHITTY_CABINET_NODE");
                if (node_text && std::atoi(node_text) == 1) {
                    SDL_RaiseWindow(window);
                    m_raised_when_linked = true;
                }
            }
        }
    }
}

void polygon_renderer_gpu::set_operator_menu(operator_menu_definition menu) {
    const int selected_id = m_operator_menu.rows.empty() ? -1 :
        m_operator_menu.rows[static_cast<std::size_t>(std::clamp(
            m_operator_selection, 0,
            static_cast<int>(m_operator_menu.rows.size()) - 1))].id;
    m_operator_menu = std::move(menu);
    m_operator_selection = 0;
    for (std::size_t index = 0; index < m_operator_menu.rows.size(); ++index) {
        if (m_operator_menu.rows[index].id == selected_id) {
            m_operator_selection = static_cast<int>(index);
            break;
        }
    }
    if (!m_operator_menu.rows.empty() &&
        !m_operator_menu.rows[static_cast<std::size_t>(m_operator_selection)].enabled)
        select_operator_row(1);
    m_settings_texture_dirty = true;
}

void polygon_renderer_gpu::select_operator_row(int direction) {
    const int count = static_cast<int>(m_operator_menu.rows.size());
    if (count <= 0) return;
    for (int attempt = 0; attempt < count; ++attempt) {
        m_operator_selection = (m_operator_selection +
            (direction < 0 ? count - 1 : 1)) % count;
        if (m_operator_menu.rows[static_cast<std::size_t>(
                m_operator_selection)].enabled)
            return;
    }
}

void polygon_renderer_gpu::adjust_operator_setting(int direction,
                                                    bool activate) {
    if (m_operator_menu.rows.empty()) return;
    operator_menu_row& row = m_operator_menu.rows[static_cast<std::size_t>(
        std::clamp(m_operator_selection, 0,
                   static_cast<int>(m_operator_menu.rows.size()) - 1))];
    if (!row.enabled) return;
    if (row.action) {
        if (activate) m_operator_actions.push_back({row.id, row.selected});
        return;
    }
    const int count = static_cast<int>(row.values.size());
    if (count <= 0) return;
    row.selected = (row.selected + (direction < 0 ? count - 1 : 1)) % count;
    m_operator_actions.push_back({row.id, row.selected});
}

arcade_board_type polygon_renderer_gpu::active_rom_board() const {
    if (m_rom_choices.empty()) return arcade_board_type::system22;
    const int index = std::clamp(
        m_rom_selection, 0, static_cast<int>(m_rom_choices.size()) - 1);
    return m_rom_choices[static_cast<std::size_t>(index)].board;
}

void polygon_renderer_gpu::select_rom_within_board(int direction) {
    if (m_rom_choices.empty()) return;
    const arcade_board_type board = active_rom_board();
    std::vector<int> indices;
    for (std::size_t index = 0; index < m_rom_choices.size(); ++index)
        if (m_rom_choices[index].board == board)
            indices.push_back(static_cast<int>(index));
    if (indices.empty()) return;
    const auto current = std::find(indices.begin(), indices.end(),
                                   m_rom_selection);
    int position = current == indices.end() ? 0 :
        static_cast<int>(current - indices.begin());
    position = (position + (direction < 0 ?
                static_cast<int>(indices.size()) - 1 : 1)) %
               static_cast<int>(indices.size());
    m_rom_selection = indices[static_cast<std::size_t>(position)];
}

void polygon_renderer_gpu::select_rom_board(arcade_board_type board) {
    if (active_rom_board() == board) return;
    const auto choice = std::find_if(
        m_rom_choices.begin(), m_rom_choices.end(),
        [board](const rom_choice& candidate) {
            return candidate.board == board;
        });
    if (choice != m_rom_choices.end())
        m_rom_selection = static_cast<int>(choice - m_rom_choices.begin());
}

void polygon_renderer_gpu::cycle_rom_board(int direction) {
    if (m_rom_choices.empty()) return;
    std::vector<arcade_board_type> boards;
    for (const rom_choice& choice : m_rom_choices) {
        if (std::find(boards.begin(), boards.end(), choice.board) == boards.end())
            boards.push_back(choice.board);
    }
    if (boards.size() < 2) return;
    const auto current = std::find(boards.begin(), boards.end(),
                                   active_rom_board());
    int position = current == boards.end() ? 0 :
        static_cast<int>(current - boards.begin());
    position = (position + (direction < 0 ?
                static_cast<int>(boards.size()) - 1 : 1)) %
               static_cast<int>(boards.size());
    select_rom_board(boards[static_cast<std::size_t>(position)]);
}

bool polygon_renderer_gpu::take_rom_selection(std::string& path) {
    if (!m_rom_selection_pending) return false;
    path = std::move(m_selected_rom);
    m_rom_selection_pending = false;
    return true;
}

bool polygon_renderer_gpu::take_operator_action(operator_menu_action& action) {
    if (m_operator_actions.empty()) return false;
    action = m_operator_actions.front();
    m_operator_actions.erase(m_operator_actions.begin());
    return true;
}

bool polygon_renderer_gpu::take_game_picker_request() {
    const bool requested = m_game_picker_requested;
    m_game_picker_requested = false;
    return requested;
}

bool polygon_renderer_gpu::take_controls_request() {
    const bool requested = m_controls_requested;
    m_controls_requested = false;
    return requested;
}

bool polygon_renderer_gpu::take_settings_change(emulator_settings& settings) {
    if (!m_settings_changed) return false;
    settings = m_display_settings;
    m_settings_changed = false;
    return true;
}

void polygon_renderer_gpu::apply_display_settings(
    const emulator_settings& settings) {
    emulator_settings applied = settings;
    m_settings_texture_dirty = true;
    if (m_headless || !m_ctx) {
        m_display_settings = applied;
        return;
    }
    if (settings.renderer != m_active_backend &&
        !configure_output_backend(settings.renderer, settings)) {
        std::fprintf(stderr, "Keeping %s output after %s switch failed\n",
                     renderer_backend_name(m_active_backend),
                     renderer_backend_name(settings.renderer));
        applied.renderer = m_active_backend;
    }
    m_display_settings = applied;
    m_ctx->make_current();
    SDL_Window* window = static_cast<SDL_Window*>(m_ctx->window);
    if (m_alternate_presenter) {
        m_alternate_presenter->apply_settings(
            effective_presenter_settings(settings));
        m_alternate_presenter->drawable_size(m_ctx->width, m_ctx->height);
    } else if (settings.fullscreen) {
        SDL_SetWindowMinimumSize(window, 1, 1);
        SDL_SetWindowMaximumSize(window, 16384, 16384);
        place_window_on_display(window, settings.display_index, false);
        SDL_SetWindowFullscreen(window, true);
    } else if (settings.wall_count > 1) {
        // Arcade wall: one column of several, each a different game.
        SDL_SetWindowFullscreen(window, false);
        if (!whitty_window::place_wall_slot(window, settings.wall_slot,
                                            settings.wall_count))
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED);
    } else if (settings.twin_one_screen) {
        // Two linked cabinets sharing one screen: this process owns one half
        // of the desktop, the companion process owns the other.
        SDL_SetWindowFullscreen(window, false);
        if (!place_window_on_cabinet_half(window,
                                          cabinet_node_from_environment()))
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED,
                                  SDL_WINDOWPOS_CENTERED);
    } else {
        SDL_SetWindowFullscreen(window, false);
        const auto size = bounded_window_size(window, settings.window_width);
        applied.window_width = size[0];
        m_display_settings.window_width = size[0];
        set_fixed_window_size(window, size[0]);
        const int target_display =
            settings.output == output_mode::dual &&
                    settings.twin_separate_monitors
                ? 0
                : settings.display_index;
        if (!place_window_on_display(window, target_display, true)) {
            // The requested display doesn't exist (typical: single-
            // monitor host running two linked_network processes).
            // Fall back to a per-cabinet half of the primary display
            // so the two windows don't stack on top of each other.
            const char* node_text = std::getenv("WHITTY_CABINET_NODE");
            const int node = node_text ? std::atoi(node_text) : 0;
            if (!place_window_on_cabinet_half(window, node))
                SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED,
                                      SDL_WINDOWPOS_CENTERED);
        }
        if (SDL_Window* window2 =
                static_cast<SDL_Window*>(m_ctx->window2)) {
            if (!settings.twin_separate_monitors ||
                !place_window_on_display(window2, 1, true)) {
                int x = SDL_WINDOWPOS_CENTERED;
                int y = SDL_WINDOWPOS_CENTERED;
                SDL_GetWindowPosition(window, &x, &y);
                SDL_SetWindowPosition(window2, x + 48, y + 48);
            }
        }
    }
    if (!m_alternate_presenter)
        SDL_GetWindowSizeInPixels(window, &m_ctx->width, &m_ctx->height);
    SDL_GL_SetSwapInterval(m_alternate_presenter ? 0 : (settings.vsync ? 1 : 0));
    glBindTexture(GL_TEXTURE_2D, m_output_texture);
    const GLint filter = settings.linear_filtering ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    if (m_external_frame_texture) {
        glBindTexture(GL_TEXTURE_2D, m_external_frame_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    }
}

bool polygon_renderer_gpu::configure_output_backend(
    renderer_backend backend, const emulator_settings& settings) {
    if (!m_ctx) return false;
    SDL_Window* gl_window = static_cast<SDL_Window*>(m_ctx->window);
    if (backend == renderer_backend::opengl) {
        if (m_alternate_presenter) {
            m_alternate_presenter->shutdown();
            m_alternate_presenter.reset();
            SDL_FlushEvent(SDL_EVENT_QUIT);
        }
        SDL_SetWindowTitle(gl_window, "WhittyArcade - OpenGL");
        SDL_ShowWindow(gl_window);
        m_ctx->make_current();
        m_active_backend = backend;
        update_fps_texture(m_last_fps);
        return true;
    }
    if (m_alternate_presenter &&
        m_alternate_presenter->backend() == backend) {
        m_active_backend = backend;
        return true;
    }

    auto replacement = std::make_unique<alternate_presenter>();
    const auto size = bounded_window_size(gl_window, settings.window_width);
    const int width = size[0];
    const int height = size[1];
    if (!replacement->initialize(backend, width, height, settings))
        return false;
    if (m_alternate_presenter) m_alternate_presenter->shutdown();
    m_alternate_presenter = std::move(replacement);
    SDL_FlushEvent(SDL_EVENT_QUIT);
    SDL_HideWindow(gl_window);
    m_ctx->make_current();
    m_active_backend = backend;
    update_fps_texture(m_last_fps);
    return true;
}

void polygon_renderer_gpu::show_pause_menu() {
    m_paused = true;
    m_settings_visible = true;
    m_pause_menu_visible = true;
    m_rom_menu_visible = false;
    m_operator_menu_visible = false;
    m_menu_axis_x = 0;
    m_menu_axis_y = 0;
    m_settings_texture_dirty = true;
}

void polygon_renderer_gpu::resume_game() {
    m_paused = false;
    m_settings_visible = false;
    m_pause_menu_visible = false;
    m_rom_menu_visible = false;
    m_operator_menu_visible = false;
    m_menu_axis_x = 0;
    m_menu_axis_y = 0;
    m_settings_texture_dirty = true;
}

void polygon_renderer_gpu::select_pause_menu_row(int direction) {
    m_pause_menu_selection =
        (m_pause_menu_selection +
         (direction < 0 ? pause_menu_row_count - 1 : 1)) %
        pause_menu_row_count;
}

bool polygon_renderer_gpu::activate_pause_menu() {
    m_pause_menu_visible = false;
    switch (m_pause_menu_selection) {
    case 0:
        resume_game();
        break;
    case 1:
        m_operator_menu_visible = true;
        break;
    case 2:
        break;
    case 3:
        m_pause_menu_visible = true;
        m_controls_requested = true;
        break;
    case 4:
        // The full cover browser runs as a modal launcher over the paused
        // game, replacing the old text-overlay ROM list.
        m_pause_menu_visible = true;
        m_game_picker_requested = true;
        break;
    case 5:
        return true;
    default:
        m_pause_menu_visible = true;
        break;
    }
    m_settings_texture_dirty = true;
    return false;
}

void polygon_renderer_gpu::adjust_setting(int direction) {
    const int step = direction == 0 ? 1 : direction;
    bool display_changed = false;
    switch (m_settings_selection) {
    case 0:
        m_display_settings.master_volume = std::clamp(
            m_display_settings.master_volume + step * 5, 0, 200);
        break;
    case 1:
        m_display_settings.music_volume = std::clamp(
            m_display_settings.music_volume + step * 5, 0, 100);
        break;
    case 2:
        m_display_settings.effects_volume = std::clamp(
            m_display_settings.effects_volume + step * 5, 0, 100);
        break;
    case 3:
        m_display_settings.fullscreen = !m_display_settings.fullscreen;
        display_changed = true;
        break;
    case 4: {
        m_display_settings.window_width = std::max(
            320, m_display_settings.window_width + step * 20);
        display_changed = true;
        break;
    }
    case 5:
        m_display_settings.vsync = !m_display_settings.vsync;
        display_changed = true;
        break;
    case 6:
        m_display_settings.integer_scaling =
            !m_display_settings.integer_scaling;
        display_changed = true;
        break;
    case 7:
        m_display_settings.linear_filtering =
            !m_display_settings.linear_filtering;
        display_changed = true;
        break;
    case 8:
        if (direction < 0) {
            m_display_settings.renderer =
                m_display_settings.renderer == renderer_backend::opengl ?
                    renderer_backend::software :
                m_display_settings.renderer == renderer_backend::vulkan ?
                    renderer_backend::opengl : renderer_backend::vulkan;
        } else {
            m_display_settings.renderer =
                m_display_settings.renderer == renderer_backend::opengl ?
                    renderer_backend::vulkan :
                m_display_settings.renderer == renderer_backend::vulkan ?
                    renderer_backend::software : renderer_backend::opengl;
        }
        display_changed = true;
        break;
    case 9:
        m_display_settings.show_renderer =
            !m_display_settings.show_renderer;
        update_fps_texture(m_last_fps);
        break;
    case 10:
        m_display_settings.show_fps = !m_display_settings.show_fps;
        update_fps_texture(m_last_fps);
        break;
    case 11:
        if (m_single_screen_only) return;
        m_display_settings.output =
            m_display_settings.output == output_mode::single ?
                output_mode::dual : output_mode::single;
        break;
    case 12:
        resume_game();
        return;
    case 13:
        m_game_picker_requested = true;
        m_settings_texture_dirty = true;
        return;
    case 14:
        m_pause_menu_visible = false;
        m_rom_menu_visible = false;
        m_operator_menu_visible = true;
        m_settings_texture_dirty = true;
        return;
    default:
        return;
    }
    if (display_changed) apply_display_settings(m_display_settings);
    m_settings_changed = true;
    m_settings_texture_dirty = true;
}

// Submit polygon data to GPU
void polygon_renderer_gpu::submit_polygons(const polygon_object* polys, int count) {
    if (m_headless || !m_initialized || count <= 0 || !polys) return;
    count = std::min(count, SYSTEM22_MAX_POLYGONS_PER_FRAME);
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    m_queued_polygons.assign(polys, polys + count);
    m_pending_polygons.store(count, std::memory_order_release);
}

// Submit texture data to GPU
void polygon_renderer_gpu::submit_textures(const uint8_t* texture_rom,
                                           size_t texture_size,
                                           const uint8_t* tilemap_rom,
                                           size_t tilemap_size,
                                           size_t region_offset,
                                           size_t bank_count,
                                           bool tile_high_bit_from_attr) {
    if (m_headless || !m_initialized || !texture_rom || !tilemap_rom) return;
    m_ctx->make_current();

    // A texture upload marks a newly constructed System 22 session. Do not
    // allow a retained shadow from the previous game to cross that boundary.
    m_temporal_shadow_polygons.clear();
    m_temporal_shadow_age = 2;

    constexpr std::size_t bank_size = 0x200000;
    if (bank_count == 0 || bank_count > 8 || region_offset > texture_size ||
        texture_size < region_offset + bank_size * bank_count || tilemap_size < 0x280000) {
        std::fprintf(stderr, "Texture region must use the 16 MiB System 22 layout\n");
        return;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    m_texture_address_base = static_cast<uint32_t>(region_offset);
    m_texture_tile_high_bit_from_attr = tile_high_bit_from_attr;
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_texture_buffer);
    for (std::size_t bank = 0; bank < bank_count; ++bank) {
        const std::size_t bank_offset = region_offset + bank * bank_size;
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
            0, 0, static_cast<GLint>(bank),
            SYSTEM22_TEXTURE_BANK_WIDTH, SYSTEM22_TEXTURE_BANK_HEIGHT, 1,
            GL_RED_INTEGER, GL_UNSIGNED_BYTE,
            texture_rom + bank_offset);
    }

    // The CCRM low ROM is a 1M-entry little-endian tile-index map. The CCRH
    // high ROM packs two four-bit transform attributes per byte.
    std::vector<uint16_t> tile_words(0x100000);
    for (std::size_t index = 0; index < tile_words.size(); ++index)
        tile_words[index] = static_cast<uint16_t>(tilemap_rom[index * 2] |
            (static_cast<uint16_t>(tilemap_rom[index * 2 + 1]) << 8));
    glBindTexture(GL_TEXTURE_2D, m_tilemap_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 4096,
                    GL_RED_INTEGER, GL_UNSIGNED_SHORT, tile_words.data());
    glBindTexture(GL_TEXTURE_2D, m_tileattr_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 512,
                    GL_RED_INTEGER, GL_UNSIGNED_BYTE, tilemap_rom + 0x200000);

    // The Vulkan scene pipeline samples its own copies. The bytes are the
    // same ones OpenGL just took, in the same layout, so both stay in step
    // without the board knowing which backend is live.
    if (m_alternate_presenter && native_system22_enabled()) {
        m_alternate_presenter->upload_scene_sheet(
            scene_sheet::texture_tiles, texture_rom + region_offset,
            bank_size * bank_count);
        m_alternate_presenter->upload_scene_sheet(
            scene_sheet::texture_map, tile_words.data(),
            tile_words.size() * sizeof(uint16_t));
        m_alternate_presenter->upload_scene_sheet(
            scene_sheet::texture_attr, tilemap_rom + 0x200000,
            static_cast<std::size_t>(1024) * 512);
    }
}

void polygon_renderer_gpu::submit_sprites(const uint8_t* sprite_rom,
                                           size_t sprite_size) {
    if (m_headless || !m_initialized || !sprite_rom) return;
    constexpr std::size_t layer_size = 0x100000;
    if (sprite_size != layer_size * 16) {
        std::fprintf(stderr,
                     "Sprite region must use the 16 MiB Super System 22 layout\n");
        return;
    }
    m_ctx->make_current();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_sprite_texture);
    for (std::size_t layer = 0; layer < 16; ++layer) {
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0,
                        static_cast<GLint>(layer), 1024, 1024, 1,
                        GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                        sprite_rom + layer * layer_size);
    }
    if (m_alternate_presenter && native_system22_enabled())
        m_alternate_presenter->upload_scene_sheet(
            scene_sheet::sprite_tiles, sprite_rom, sprite_size);
}

void polygon_renderer_gpu::submit_palette(const uint8_t* palette_ram, size_t size) {
    if (m_headless || !m_initialized || !palette_ram || size < 0x18000) return;
    m_ctx->make_current();
    // The OpenGL copy exists only to keep the fallback warm. The board
    // resubmits the palette every frame, so a decline repopulates it
    // immediately and skipping it while Vulkan owns the frame is free.
    if (opengl_textures_needed()) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, m_palette_texture);
        for (std::size_t component = 0; component < 3; ++component) {
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0,
                            static_cast<GLint>(component),
                            256, 128, 1, GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                            palette_ram + component * std::size_t{0x8000});
        }
    }
    // The three colour planes are contiguous at a 0x8000 stride, which is
    // exactly the layered image the Vulkan shader samples.
    if (m_alternate_presenter && native_system22_enabled())
        m_alternate_presenter->upload_scene_sheet(scene_sheet::palette,
                                                  palette_ram, 0x18000);
}

void polygon_renderer_gpu::submit_gamma(const uint8_t* gamma_proms, size_t size) {
    if (m_headless || !m_initialized || !gamma_proms || size < 0x300) return;
    m_ctx->make_current();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_gamma_texture);
    for (std::size_t component = 0; component < 3; ++component) {
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0,
                        static_cast<GLint>(component),
                        256, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                        gamma_proms + component * std::size_t{0x100});
    }
    // The three ramps are contiguous, which is the layered image the Vulkan
    // presentation pass samples.
    if (m_alternate_presenter && native_system22_enabled())
        m_alternate_presenter->upload_gamma_ramp(gamma_proms, 256 * 3);
}

void polygon_renderer_gpu::submit_text_layer(const uint8_t* character_ram,
                                              size_t character_size,
                                              const uint8_t* text_ram,
                                              size_t text_size,
                                              const uint8_t* text_attributes,
                                              const uint8_t* mixer,
                                              size_t mixer_size,
                                              bool super_system22) {
    if (m_headless || !m_initialized || !character_ram || !text_ram ||
        !text_attributes || !mixer)
        return;
    m_ctx->make_current();
    if (character_size < 0x20000 || text_size < 0x2000 || mixer_size < 8 ||
        (super_system22 && mixer_size < 0x20))
        return;

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (opengl_textures_needed()) {
        glBindTexture(GL_TEXTURE_2D, m_character_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 512, 256,
                        GL_RED_INTEGER, GL_UNSIGNED_BYTE, character_ram);
    }

    std::array<uint16_t, std::size_t{64} * 64> entries{};
    for (std::size_t index = 0; index < entries.size(); ++index)
        entries[index] = static_cast<uint16_t>(
            (static_cast<uint16_t>(text_ram[index * 2]) << 8) |
            text_ram[index * 2 + 1]);
    if (opengl_textures_needed()) {
        glBindTexture(GL_TEXTURE_2D, m_textmap_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, 64,
                        GL_RED_INTEGER, GL_UNSIGNED_SHORT, entries.data());
    }
    // The Vulkan text pipeline samples its own copies of both. The board
    // rewrites them as it runs, so these stream in with the frame rather
    // than stalling the queue.
    if (m_alternate_presenter && native_system22_enabled()) {
        m_alternate_presenter->upload_scene_sheet(
            scene_sheet::character_data, character_ram,
            static_cast<std::size_t>(512) * 256);
        m_alternate_presenter->upload_scene_sheet(
            scene_sheet::text_map, entries.data(),
            entries.size() * sizeof(uint16_t));
    }

    const int scroll_x = (static_cast<int>(
        (static_cast<uint16_t>(text_attributes[0]) << 8) |
        text_attributes[1]) - 0x35c) & 0x3ff;
    const int scroll_y = ((static_cast<uint16_t>(text_attributes[2]) << 8) |
                          text_attributes[3]) & 0x3ff;
    m_text_scroll_x = scroll_x;
    m_text_scroll_y = scroll_y;
    if (m_super_system22_video != super_system22) {
        m_temporal_shadow_polygons.clear();
        m_temporal_shadow_age = 2;
    }
    m_super_system22_video = super_system22;
    if (super_system22) {
        m_text_palette_base =
            (static_cast<int>(mixer[0x1b]) << 8) & 0x7f00;
        m_super_poly_fade_r = mixer[0x00];
        m_super_poly_fade_g = mixer[0x01];
        m_super_poly_fade_b = mixer[0x02];
        m_super_poly_fade_enabled =
            m_super_poly_fade_r != 0xff ||
            m_super_poly_fade_g != 0xff ||
            m_super_poly_fade_b != 0xff;
        m_super_poly_alpha_color = mixer[0x0f];
        m_super_poly_alpha_pen = mixer[0x10];
        m_super_poly_alpha_factor = mixer[0x11];
        m_super_screen_fade_r = mixer[0x16];
        m_super_screen_fade_g = mixer[0x17];
        m_super_screen_fade_b = mixer[0x18];
        m_super_screen_fade_factor = mixer[0x19];
        m_super_mixer_flags = mixer[0x1a];
        // The older board uses per-channel 8.8 multipliers. Keep that path
        // neutral while Super System 22's colour/factor blend is applied in
        // the individual polygon, sprite, text and background paths.
        m_screen_fade_r = m_screen_fade_g = m_screen_fade_b = 0x100;
        m_screen_fade_initialized = false;
    } else {
        m_text_palette_base = (static_cast<int>(mixer[7]) << 8) & 0x7f00;
        m_super_screen_fade_factor = 0;
        m_super_mixer_flags = 0;
    }
    if (!super_system22 && mixer_size >= 0x17) {
        const uint32_t fade_r = (static_cast<uint32_t>(mixer[0x11]) << 8) |
                                mixer[0x12];
        const uint32_t fade_g = (static_cast<uint32_t>(mixer[0x13]) << 8) |
                                mixer[0x14];
        const uint32_t fade_b = (static_cast<uint32_t>(mixer[0x15]) << 8) |
                                mixer[0x16];
        // Full Scale leaves these mixer registers cleared while bringing up
        // its display.  Treat an all-zero, never-programmed set as unity;
        // once the game has supplied a fade value, zero remains a valid
        // request for a deliberate fade to black.
        if (m_screen_fade_initialized || fade_r != 0 || fade_g != 0 || fade_b != 0) {
            m_screen_fade_r = fade_r;
            m_screen_fade_g = fade_g;
            m_screen_fade_b = fade_b;
            m_screen_fade_initialized = true;
        }
    }
}

void polygon_renderer_gpu::render_scene(const view_matrix& view, const rgba_color& fog_color) {
    // Check if we have a display
    if (m_headless) {
        // Headless mode - just update frame counter
        return;
    }
    m_ctx->make_current();

    // Upload camera uniform
    glBindBuffer(GL_UNIFORM_BUFFER, m_camera_ubo);

    float camera_data[20]{};
    memcpy(camera_data, view.m, 16 * sizeof(float));
    camera_data[16] = normalized_byte(fog_color.r);
    camera_data[17] = normalized_byte(fog_color.g);
    camera_data[18] = normalized_byte(fog_color.b);
    camera_data[19] = normalized_byte(fog_color.a);

    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(camera_data), camera_data);

    std::vector<polygon_object> polygons;
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        polygons.swap(m_queued_polygons);
        m_pending_polygons.store(0, std::memory_order_release);
    }

    // Several System 22 games deliver projected car-shadow meshes in
    // alternating frame packets. Presenting that packet cadence directly
    // visibly flashes on a modern display. Match only the measured shadow
    // materials and carry those quads into the following frame; the car, HUD
    // and track remain single-frame images.
    const auto is_temporal_shadow = [this](const polygon_object& polygon) {
        return system22_temporal_shadow_material(
            polygon, m_super_system22_video);
    };
    std::vector<polygon_object> current_shadow;
    std::copy_if(polygons.begin(), polygons.end(),
                 std::back_inserter(current_shadow),
                 is_temporal_shadow);
    if (!current_shadow.empty()) {
        m_temporal_shadow_polygons = std::move(current_shadow);
        m_temporal_shadow_age = 0;
    } else if (m_temporal_shadow_age == 0 &&
               !m_temporal_shadow_polygons.empty()) {
        polygons.insert(polygons.end(),
                        m_temporal_shadow_polygons.begin(),
                        m_temporal_shadow_polygons.end());
        m_temporal_shadow_age = 1;
    } else {
        if (m_temporal_shadow_age < 2)
            ++m_temporal_shadow_age;
        if (m_temporal_shadow_age >= 2)
            m_temporal_shadow_polygons.clear();
    }

    std::stable_sort(polygons.begin(), polygons.end(),
        [](const polygon_object& left, const polygon_object& right) {
            return left.zsort > right.zsort;
        });
    // The hardware radix tree prepends leaves with an identical 24-bit Z
    // key, so equal-depth polygons render in reverse submission order.
    for (auto first = polygons.begin(); first != polygons.end();) {
        auto last = first + 1;
        while (last != polygons.end() && last->zsort == first->zsort) ++last;
        std::reverse(first, last);
        first = last;
    }

    std::vector<draw_vertex> vertices;
    vertices.reserve(polygons.size() * 12);
    for (const polygon_object& polygon : polygons) append_quad(vertices, polygon);

    // Clear and render. OpenGL is touched only by this context-owning thread;
    // submit_polygons itself is safe for the CPU/DSP producer thread.
    glBindFramebuffer(GL_FRAMEBUFFER, m_scene_framebuffer);
    glViewport(0, 0, SYSTEM22_SCREEN_WIDTH, SYSTEM22_SCREEN_HEIGHT);
    // With Vulkan owning the window the whole scene rasterises there: the
    // same vertices, the same uniforms, straight into the image the post pass
    // samples. That removes this board's glReadPixels round trip and the CPU
    // rescale behind it. The native path declines when it is unavailable, and
    // the OpenGL route below then runs exactly as before.
    // Opt-in while this path is still being brought up: System 22 has a
    // proven OpenGL route and there is no reason to risk it by default.
    // Set WHITTY_VK_SYSTEM22=1 to rasterise natively instead.
    if (m_alternate_presenter && native_system22_enabled()) {
        system22_scene scene;
        scene.width = SYSTEM22_SCREEN_WIDTH;
        scene.height = SYSTEM22_SCREEN_HEIGHT;
        scene.vertices = vertices.data();
        scene.vertex_count = vertices.size();
        scene.uniforms = build_system22_uniforms(view);
        scene.draw_text = (m_system22_layer_mask & 0x04) != 0;
        scene.text_uniforms = build_system22_text_uniforms();
        // The OpenGL route ends in present_texture(..., board_color = true),
        // so the same grading has to reach the Vulkan presentation pass.
        scene.apply_board_color = true;
        scene.super_system22 = m_super_system22_video;
        scene.screen_fade[0] = m_screen_fade_r;
        scene.screen_fade[1] = m_screen_fade_g;
        scene.screen_fade[2] = m_screen_fade_b;
        // Presentation has to succeed too. Returning on a rasterised-but-
        // unpresented frame would leave the window never shown and no way
        // back to OpenGL, so both halves are required before this path
        // claims the frame.
        if (present_native_scene(
                m_alternate_presenter->render_system22_scene(scene),
                "System 22", SYSTEM22_SCREEN_WIDTH, SYSTEM22_SCREEN_HEIGHT))
            return;
    }

    glClearColor(normalized_byte(fog_color.r), normalized_byte(fog_color.g),
                 normalized_byte(fog_color.b), 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (!vertices.empty()) {
        // Super System 22 uses the mixer alpha registers for translucent car
        // shadows and effects. Dual-source blending supplies that opacity
        // while independently replacing framebuffer alpha with text priority.
        if (m_super_system22_video) {
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA,
                                GL_ONE, GL_ZERO);
        } else {
            glDisable(GL_BLEND);
        }
        glUseProgram(m_graphics_program);
        glUniformMatrix4fv(glGetUniformLocation(m_graphics_program, "view_matrix"),
                           1, GL_TRUE, &view.m[0][0]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, m_texture_buffer);
        glUniform1i(glGetUniformLocation(m_graphics_program, "texture_tiles"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_tilemap_texture);
        glUniform1i(glGetUniformLocation(m_graphics_program, "texture_map"), 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_tileattr_texture);
        glUniform1i(glGetUniformLocation(m_graphics_program, "texture_attr"), 2);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D_ARRAY, m_palette_texture);
        glUniform1i(glGetUniformLocation(m_graphics_program, "palette_data"), 3);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D_ARRAY, m_sprite_texture);
        glUniform1i(glGetUniformLocation(m_graphics_program, "sprite_tiles"), 4);
        glUniform1ui(glGetUniformLocation(m_graphics_program, "texture_address_base"),
                     m_texture_address_base);
        glUniform1ui(glGetUniformLocation(m_graphics_program,
                                           "texture_tile_high_bit_from_attr"),
                     m_texture_tile_high_bit_from_attr ? 1u : 0u);
        glUniform1i(glGetUniformLocation(m_graphics_program,
                                         "super_system22"),
                    m_super_system22_video ? 1 : 0);
        glUniform3ui(glGetUniformLocation(m_graphics_program,
                                          "super_fade_color"),
                     m_super_screen_fade_r, m_super_screen_fade_g,
                     m_super_screen_fade_b);
        glUniform1ui(glGetUniformLocation(m_graphics_program,
                                          "super_fade_factor"),
                     m_super_screen_fade_factor);
        glUniform1ui(glGetUniformLocation(m_graphics_program,
                                          "super_mixer_flags"),
                     m_super_mixer_flags);
        glUniform3ui(glGetUniformLocation(m_graphics_program,
                                          "super_poly_fade_color"),
                     m_super_poly_fade_r, m_super_poly_fade_g,
                     m_super_poly_fade_b);
        glUniform1i(glGetUniformLocation(m_graphics_program,
                                         "super_poly_fade_enabled"),
                    m_super_poly_fade_enabled ? 1 : 0);
        glUniform1ui(glGetUniformLocation(m_graphics_program,
                                          "super_poly_alpha_color"),
                     m_super_poly_alpha_color);
        glUniform1ui(glGetUniformLocation(m_graphics_program,
                                          "super_poly_alpha_pen"),
                     m_super_poly_alpha_pen);
        glUniform1ui(glGetUniformLocation(m_graphics_program,
                                          "super_poly_alpha_factor"),
                     m_super_poly_alpha_factor);
        glBindVertexArray(m_vertex_array);
        glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
        const auto vertex_bytes =
            static_cast<GLsizeiptr>(vertices.size() * sizeof(draw_vertex));
        glBufferSubData(GL_ARRAY_BUFFER, 0, vertex_bytes, vertices.data());
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
    }

    // Transparent text pixels are discarded by the shader. For every other
    // pixel, destination alpha 0 replaces the scene with text and alpha 1
    // preserves a polygon carrying the hardware's over-character flag.
    if ((m_system22_layer_mask & 0x04) != 0) {
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_DST_ALPHA);
        glUseProgram(m_text_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_character_texture);
        glUniform1i(glGetUniformLocation(m_text_program, "character_data"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_textmap_texture);
        glUniform1i(glGetUniformLocation(m_text_program, "text_map"), 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D_ARRAY, m_palette_texture);
        glUniform1i(glGetUniformLocation(m_text_program, "palette_data"), 2);
        glUniform2i(glGetUniformLocation(m_text_program, "text_scroll"),
                    m_text_scroll_x, m_text_scroll_y);
        glUniform1i(glGetUniformLocation(m_text_program, "text_palette_base"),
                    m_text_palette_base);
        glUniform1i(glGetUniformLocation(m_text_program, "super_system22"),
                    m_super_system22_video ? 1 : 0);
        glUniform3ui(glGetUniformLocation(m_text_program, "super_fade_color"),
                     m_super_screen_fade_r, m_super_screen_fade_g,
                     m_super_screen_fade_b);
        glUniform1ui(glGetUniformLocation(m_text_program, "super_fade_factor"),
                     m_super_screen_fade_factor);
        glUniform1ui(glGetUniformLocation(m_text_program, "super_mixer_flags"),
                     m_super_mixer_flags);
        glBindVertexArray(m_text_vertex_array);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    present_texture(m_output_texture, SYSTEM22_SCREEN_WIDTH,
                    SYSTEM22_SCREEN_HEIGHT, true, false);
}

void polygon_renderer_gpu::present_texture(uint32_t texture, int source_width,
                                            int source_height,
                                            bool apply_board_color,
                                            bool flip_y,
                                            int display_width,
                                            int display_height) {
    if (m_headless || !m_ctx || texture == 0 ||
        source_width <= 0 || source_height <= 0)
        return;
    recover_from_device_loss();

    if (display_width <= 0) display_width = source_width;
    if (display_height <= 0) display_height = source_height;

    // For ordinary two-machine play Player 1 owns the emulated game. Capture
    // its final board texture at 30 Hz; Player 2 uploads and presents that
    // picture while its controls continue travelling back over the input
    // link. Native linked cabinets (such as Sega Rally) never enable this.
    if (m_network_video && !m_settings_visible) {
        if (m_network_video->sender()) {
            if ((++m_network_capture_divider & 1U) == 0) {
                network_video_frame frame;
                frame.width = source_width;
                frame.height = source_height;
                frame.display_width = display_width;
                frame.display_height = display_height;
                frame.flip_y = flip_y;
                frame.apply_board_color = apply_board_color;
                frame.sequence = ++m_network_video_sequence;
                frame.rgba.resize(static_cast<std::size_t>(source_width) *
                                  source_height * 4);
                glBindTexture(GL_TEXTURE_2D, texture);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA,
                              GL_UNSIGNED_BYTE, frame.rgba.data());
                m_network_video->submit(std::move(frame));
            }
        } else if (m_network_video->receiver()) {
            network_video_frame frame;
            if (m_network_video->take_received(frame)) {
                if (!m_network_frame_texture)
                    glGenTextures(1, &m_network_frame_texture);
                glBindTexture(GL_TEXTURE_2D, m_network_frame_texture);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                                GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                                GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                                GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                                GL_CLAMP_TO_EDGE);
                if (frame.width != m_network_frame_width ||
                    frame.height != m_network_frame_height) {
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frame.width,
                                 frame.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                 frame.rgba.data());
                    m_network_frame_width = frame.width;
                    m_network_frame_height = frame.height;
                } else {
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width,
                                    frame.height, GL_RGBA, GL_UNSIGNED_BYTE,
                                    frame.rgba.data());
                }
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                m_network_frame_display_width = frame.display_width;
                m_network_frame_display_height = frame.display_height;
                m_network_frame_flip_y = frame.flip_y;
                m_network_frame_board_color = frame.apply_board_color;
                m_network_frame_valid = true;
            }
            if (m_network_frame_valid) {
                texture = m_network_frame_texture;
                source_width = m_network_frame_width;
                source_height = m_network_frame_height;
                display_width = m_network_frame_display_width;
                display_height = m_network_frame_display_height;
                flip_y = m_network_frame_flip_y;
                apply_board_color = m_network_frame_board_color;
            }
        }
    }
    m_last_present_texture = texture;
    m_last_present_width = source_width;
    m_last_present_height = source_height;
    m_last_present_display_width = display_width;
    m_last_present_display_height = display_height;
    m_last_present_board_color = apply_board_color;
    m_last_present_flip_y = flip_y;

    // Alternate presenters read from an explicit framebuffer. A hidden
    // NVIDIA/XWayland default framebuffer can become undefined when another
    // SDL window replaces it, which previously produced a persistent black
    // frame after a live backend switch. Keep that framebuffer at the arcade
    // board's native resolution: reading a full desktop-sized image back from
    // OpenGL merely so Vulkan/software can scale it again is both redundant
    // and exceptionally expensive on split-GPU systems.
    if (m_alternate_presenter) {
        int presentation_width = source_width;
        int presentation_height = source_height;
        if (m_settings_visible) {
            // Settings and ROM selection are host UI, not arcade pixels.
            // Render those overlays at the visible drawable resolution so
            // they have exactly the same size and sharpness after switching
            // between OpenGL, Vulkan and software. Normal gameplay keeps the
            // cheaper native-resolution readback below.
            m_alternate_presenter->drawable_size(
                presentation_width, presentation_height);
        }
        if (!m_present_framebuffer) glGenFramebuffers(1, &m_present_framebuffer);
        if (!m_present_texture) glGenTextures(1, &m_present_texture);
        if (m_present_width != presentation_width ||
            m_present_height != presentation_height) {
            glBindTexture(GL_TEXTURE_2D, m_present_texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, presentation_width,
                         presentation_height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            m_present_width = presentation_width;
            m_present_height = presentation_height;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, m_present_framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, m_present_texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "Presentation framebuffer is incomplete\n");
            return;
        }
        m_ctx->width = presentation_width;
        m_ctx->height = presentation_height;
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        // Query every frame: compositors can change the drawable scale without
        // a matching logical-size event while moving or resizing.
        SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(m_ctx->window),
                               &m_ctx->width, &m_ctx->height);
    }
    m_ctx->width = std::max(m_ctx->width, 1);
    m_ctx->height = std::max(m_ctx->height, 1);

    // The scene target is native resolution, while the default framebuffer
    // follows the live host drawable. Restore the latter before presentation.
    glViewport(0, 0, m_ctx->width, m_ctx->height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    // Split the drawable into one pane (normal) or two side-by-side panes
    // (dual output). Both panes present the identical game image so a second
    // player can watch their own screen area on an ultrawide, or on two
    // monitors joined as one spanned desktop. Each pane letterboxes the game
    // independently within its half.
    struct present_pane { int x, y, w, h; };
    std::array<present_pane, 2> panes{{{0, 0, m_ctx->width, m_ctx->height},
                                       {0, 0, 0, 0}}};
    int pane_count = 1;
    // Dual output splits the final window/drawable, so it only applies to the
    // direct OpenGL present. With a Vulkan/software presenter this pane loop
    // draws into the native-resolution intermediate framebuffer (which the
    // presenter then scales to the window); splitting there would wreck the
    // image, so keep it single and let the presenter own the final letterbox.
    // The pause/settings/ROM menu is host UI, not a game frame: show it single
    // and full so it stays crisp and readable instead of being shrunk into a
    // dual-output half.
    const bool dual_output = !m_single_screen_only && !m_alternate_presenter &&
        !m_settings_visible &&
        m_display_settings.output == output_mode::dual;
    // Windowed dual output gives each player an independent window (rendered
    // after the main swap below, so it can sit on its own monitor). Fullscreen
    // instead divides the one fullscreen surface down the middle, so a spanned
    // two-monitor desktop shows one player per screen. Each half letterboxes
    // the board independently at its true aspect.
    const bool second_window = dual_output && !m_display_settings.fullscreen;
    if (dual_output && m_display_settings.fullscreen) {
        // Leave a gutter down the centre so the two arcade screens never
        // touch; each half then letterboxes its game at its true aspect.
        const int gap = std::max(m_ctx->width / 40, 24);
        const int left_w = std::clamp((m_ctx->width - gap) / 2, 1,
                                      m_ctx->width - 1);
        panes[0] = {0, 0, left_w, m_ctx->height};
        panes[1] = {left_w + gap, 0, m_ctx->width - left_w - gap,
                    m_ctx->height};
        pane_count = 2;
    }

    // Fullscreen uses all available space while preserving the original
    // monitor aspect. Integer scaling remains available for windowed play.
    const bool pane_integer =
        m_display_settings.integer_scaling && !m_display_settings.fullscreen;
    // Alternate presenters receive the monitor aspect separately. Keep their
    // OpenGL readback texture at the board's native pixel geometry and avoid
    // baking letterbox bars into it before Vulkan/software scales it.
    const int fit_display_width =
        m_alternate_presenter ? m_ctx->width : display_width;
    const int fit_display_height =
        m_alternate_presenter ? m_ctx->height : display_height;
    const auto fit_pane = [&](const present_pane& pane) {
        const int frame_margin = dual_output ?
            std::clamp(std::min(pane.w, pane.h) / 32, 12, 30) : 0;
        const present_pane content{
            pane.x + frame_margin, pane.y + frame_margin,
            std::max(pane.w - frame_margin * 2, 1),
            std::max(pane.h - frame_margin * 2, 1),
        };
        int ow = content.w;
        int oh = content.h;
        if (dual_output) {
            // Twin Screen preserves the board's native raster with a single
            // integer scale on X and Y. Galaga is therefore exactly 224x288
            // times N, never stretched to the nominal 3:4 monitor shape.
            const int scale = std::min(content.w / source_width,
                                       content.h / source_height);
            if (scale >= 1) {
                ow = source_width * scale;
                oh = source_height * scale;
            } else if (ow * source_height > oh * source_width) {
                ow = oh * source_width / source_height;
            } else {
                oh = ow * source_height / source_width;
            }
        } else if (pane_integer) {
            const int scale = std::min(content.w / source_width,
                                       content.h / source_height);
            if (scale >= 1) {
                ow = source_width * scale;
                oh = source_height * scale;
            }
            if (ow * fit_display_height > oh * fit_display_width)
                ow = oh * fit_display_width / fit_display_height;
            else
                oh = ow * fit_display_height / fit_display_width;
        } else {
            if (ow * fit_display_height > oh * fit_display_width)
                ow = oh * fit_display_width / fit_display_height;
            else
                oh = ow * fit_display_height / fit_display_width;
        }
        return present_pane{content.x + (content.w - ow) / 2,
                            content.y + (content.h - oh) / 2, ow, oh};
    };
    const auto draw_cabinet_frame = [&](const present_pane& fit,
                                        int surface_width,
                                        int surface_height) {
        if (!dual_output) return;
        const auto clear_rect = [&](int amount, float red, float green,
                                    float blue) {
            const int x = std::max(fit.x - amount, 0);
            const int y = std::max(fit.y - amount, 0);
            const int right =
                std::min(fit.x + fit.w + amount, surface_width);
            const int top =
                std::min(fit.y + fit.h + amount, surface_height);
            if (right <= x || top <= y) return;
            glEnable(GL_SCISSOR_TEST);
            glScissor(x, y, right - x, top - y);
            glClearColor(red, green, blue, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
        };
        clear_rect(10, 0.05f, 0.11f, 0.15f);
        clear_rect(3, 0.22f, 0.78f, 1.0f);
    };

    // Shared post-process state, set once for all panes.
    glDisable(GL_BLEND);
    glUseProgram(m_post_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    const GLint presentation_filter = m_display_settings.linear_filtering ?
        GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    presentation_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    presentation_filter);
    glUniform1i(glGetUniformLocation(m_post_program, "scene_color"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_gamma_texture);
    glUniform1i(glGetUniformLocation(m_post_program, "gamma_data"), 1);
    glUniform3ui(glGetUniformLocation(m_post_program, "screen_fade"),
                 m_screen_fade_r, m_screen_fade_g, m_screen_fade_b);
    glUniform1i(glGetUniformLocation(m_post_program, "super_system22"),
                m_super_system22_video ? 1 : 0);
    glUniform1i(glGetUniformLocation(m_post_program, "apply_board_color"),
                apply_board_color ? 1 : 0);
    glUniform1i(glGetUniformLocation(m_post_program, "flip_y"), flip_y ? 1 : 0);
    glUniform1i(glGetUniformLocation(m_post_program, "linear_filtering"),
                m_display_settings.linear_filtering ? 1 : 0);
    const GLint output_size_loc =
        glGetUniformLocation(m_post_program, "output_size");
    const GLint output_origin_loc =
        glGetUniformLocation(m_post_program, "output_origin");
    glBindVertexArray(m_post_vertex_array);
    for (int pane = 0; pane < pane_count; ++pane) {
        const present_pane fit = fit_pane(panes[pane]);
        if (pane == 0) {
            // Publish where the picture actually landed. A light gun has to
            // map the pointer onto the game's image, and the rules that put it
            // there (letterbox to the monitor aspect, integer scaling, the
            // twin-screen margins) live here -- anything reconstructing that
            // from the window size alone gets a different rectangle and aims
            // the gun at the wrong place. Y is flipped into top-down window
            // coordinates, which is what pointer positions use.
            m_picture_rect_x = fit.x;
            m_picture_rect_y = m_ctx->height - (fit.y + fit.h);
            m_picture_rect_width = fit.w;
            m_picture_rect_height = fit.h;
            m_picture_drawable_width = m_ctx->width;
            m_picture_drawable_height = m_ctx->height;
        }
        draw_cabinet_frame(fit, m_ctx->width, m_ctx->height);
        glViewport(fit.x, fit.y, fit.w, fit.h);
        glUniform2i(output_size_loc, fit.w, fit.h);
        glUniform2i(output_origin_loc, fit.x, fit.y);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // In the fullscreen split, both players share one surface with no window
    // titles to tell them apart, so label each half in its bottom-left corner.
    if (pane_count == 2 || second_window) {
        ensure_player_labels();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(m_settings_program);
        glActiveTexture(GL_TEXTURE0);
        const GLint tex_loc =
            glGetUniformLocation(m_settings_program, "settings_texture");
        glUniform1i(tex_loc, 0);
        glBindVertexArray(m_settings_vertex_array);
        const int label_count = pane_count == 2 ? 2 : 1;
        for (int pane = 0; pane < label_count; ++pane) {
            if (!m_player_label_texture[pane]) continue;
            glViewport(panes[pane].x + 16, panes[pane].y + 16,
                       m_player_label_w[pane], m_player_label_h[pane]);
            glBindTexture(GL_TEXTURE_2D, m_player_label_texture[pane]);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glDisable(GL_BLEND);
    }

    const bool status_overlay_visible = tick_status_overlay();
    if (status_overlay_visible) {
        // OpenGL draws directly in host pixels. Alternate backends receive
        // the same panel separately below and composite it after scaling the
        // native game frame, keeping its physical size backend-independent.
        if (!m_alternate_presenter || m_settings_visible)
            draw_fps_overlay();
    }
    if (m_settings_visible) draw_settings_overlay();

    if (m_alternate_presenter) {
        m_present_readback.resize(
            static_cast<std::size_t>(m_ctx->width) * m_ctx->height * 4);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, m_ctx->width, m_ctx->height, GL_RGBA,
                     GL_UNSIGNED_BYTE, m_present_readback.data());
        const bool separate_status_overlay =
            status_overlay_visible && !m_settings_visible;
        m_alternate_presenter->present_rgba_bottom_up(
            m_present_readback.data(), m_ctx->width, m_ctx->height,
            separate_status_overlay ? m_fps_pixels.data() : nullptr,
            separate_status_overlay ? m_fps_texture_width : 0,
            separate_status_overlay ? m_fps_texture_height : 0,
            m_settings_visible,
            m_settings_visible ? m_ctx->width : display_width,
            m_settings_visible ? m_ctx->height : display_height);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    } else {
        SDL_GL_SwapWindow(static_cast<SDL_Window*>(m_ctx->window));
        maintain_gl_wall_placement();
        // Windowed dual output: mirror the same frame into an independent
        // Player 2 window, either beside Player 1 on the same desktop or
        // centred on the second physical display. The window shares this GL
        // context, so the game texture and post-process program are reused.
        if (second_window) {
            SDL_Window* w1 = static_cast<SDL_Window*>(m_ctx->window);
            SDL_GLContext ctx = static_cast<SDL_GLContext>(m_ctx->gl_context);
            if (!m_ctx->window2) {
                if (SDL_Window* w2 = SDL_CreateWindow(
                        "WhittyArcade - Player 2", m_ctx->width, m_ctx->height,
                        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE)) {
                    if (!m_display_settings.twin_separate_monitors ||
                        !place_window_on_display(w2, 1, true)) {
                        if (!m_display_settings.twin_separate_monitors) {
                            whitty_window::arrange_twin_windows(
                                w1, w2, m_display_settings.window_width);
                        } else {
                            int x = SDL_WINDOWPOS_CENTERED;
                            int y = SDL_WINDOWPOS_CENTERED;
                            SDL_GetWindowPosition(w1, &x, &y);
                            SDL_SetWindowPosition(w2, x + 48, y + 48);
                        }
                    }
                    m_ctx->window2 = w2;
                    if (m_display_settings.twin_separate_monitors)
                        SDL_SetWindowTitle(w1, "WhittyArcade - Player 1");
                }
            }
            if (SDL_Window* w2 = static_cast<SDL_Window*>(m_ctx->window2)) {
                if (!m_display_settings.twin_separate_monitors &&
                    m_ctx->twin_layout_attempts < 6) {
                    SDL_PumpEvents();
                    whitty_window::arrange_twin_windows(
                        w1, w2, m_display_settings.window_width);
                    ++m_ctx->twin_layout_attempts;
                }
                SDL_GL_MakeCurrent(w2, ctx);
                // The Player 1 swap above has already waited for vblank.
                // Letting this one wait as well serialises two vblanks into
                // one frame, which pins windowed dual output to half the
                // refresh rate no matter how fast the machine is. The mirror
                // rides Player 1's cadence with its interval released.
                if (m_display_settings.vsync) SDL_GL_SetSwapInterval(0);
                int w2w = 1;
                int w2h = 1;
                SDL_GetWindowSizeInPixels(w2, &w2w, &w2h);
                w2w = std::max(w2w, 1);
                w2h = std::max(w2h, 1);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDisable(GL_BLEND);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                const present_pane p2 = fit_pane({0, 0, w2w, w2h});
                draw_cabinet_frame(p2, w2w, w2h);
                glUseProgram(m_post_program);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture);
                glViewport(p2.x, p2.y, p2.w, p2.h);
                glUniform2i(output_size_loc, p2.w, p2.h);
                glUniform2i(output_origin_loc, p2.x, p2.y);
                glBindVertexArray(m_post_vertex_array);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                ensure_player_labels();
                if (m_player_label_texture[1]) {
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    glUseProgram(m_settings_program);
                    glActiveTexture(GL_TEXTURE0);
                    glUniform1i(
                        glGetUniformLocation(
                            m_settings_program, "settings_texture"), 0);
                    glBindVertexArray(m_settings_vertex_array);
                    glViewport(16, 16, m_player_label_w[1],
                               m_player_label_h[1]);
                    glBindTexture(GL_TEXTURE_2D,
                                  m_player_label_texture[1]);
                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                    glDisable(GL_BLEND);
                }
                SDL_GL_SwapWindow(w2);
                SDL_GL_MakeCurrent(w1, ctx);
                if (m_display_settings.vsync) SDL_GL_SetSwapInterval(1);
            }
        } else if (m_ctx->window2) {
            // Left windowed dual output - tear the second window down.
            SDL_DestroyWindow(static_cast<SDL_Window*>(m_ctx->window2));
            m_ctx->window2 = nullptr;
            m_ctx->twin_layout_attempts = 0;
            SDL_SetWindowTitle(static_cast<SDL_Window*>(m_ctx->window),
                               "WhittyArcade");
        }
    }
}

// Packs the loose System 22 uniforms into the std140 block the Vulkan shaders
// read. The OpenGL path sets the same values one glUniform call at a time;
// this is the only place the two can drift, so it reads them from the same
// members in the same order.
system22_uniform_block polygon_renderer_gpu::build_system22_uniforms(
        const view_matrix& view) const {
    system22_uniform_block block;
    // The GL path uploads the matrix transposed (glUniformMatrix4fv with
    // transpose = GL_TRUE); std140 has no such switch, so transpose here.
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            block.view_matrix[column * 4 + row] = view.m[row][column];
    block.super_fade_color[0] = m_super_screen_fade_r;
    block.super_fade_color[1] = m_super_screen_fade_g;
    block.super_fade_color[2] = m_super_screen_fade_b;
    block.super_poly_fade_color[0] = m_super_poly_fade_r;
    block.super_poly_fade_color[1] = m_super_poly_fade_g;
    block.super_poly_fade_color[2] = m_super_poly_fade_b;
    block.texture_control[0] = m_texture_address_base;
    block.texture_control[1] = m_texture_tile_high_bit_from_attr ? 1u : 0u;
    block.texture_control[2] = m_super_system22_video ? 1u : 0u;
    block.texture_control[3] = m_super_screen_fade_factor;
    block.mixer_control[0] = m_super_mixer_flags;
    block.mixer_control[1] = m_super_poly_fade_enabled ? 1u : 0u;
    block.mixer_control[2] = m_super_poly_alpha_color;
    block.mixer_control[3] = m_super_poly_alpha_pen;
    block.alpha_control[0] = m_super_poly_alpha_factor;
    return block;
}

// The text layer's own std140 block. Its fade is driven by the same screen
// mixer as the polygon layer, hence the shared colour and factor.
system22_text_uniform_block
polygon_renderer_gpu::build_system22_text_uniforms() const {
    system22_text_uniform_block block;
    block.text_scroll[0] = m_text_scroll_x;
    block.text_scroll[1] = m_text_scroll_y;
    block.text_scroll[2] = m_text_palette_base;
    block.super_fade_color[0] = m_super_screen_fade_r;
    block.super_fade_color[1] = m_super_screen_fade_g;
    block.super_fade_color[2] = m_super_screen_fade_b;
    block.super_control[0] = m_super_system22_video ? 1u : 0u;
    block.super_control[1] = m_super_screen_fade_factor;
    block.super_control[2] = m_super_mixer_flags;
    return block;
}

// Advances the frame counter that drives the on-screen rate readout and
// refreshes its texture twice a second. Returns whether the status panel
// should be shown at all. Shared by the OpenGL and native Vulkan paths so the
// measured rate means the same thing on both.
// Bundles the host panels the Vulkan path composites over a board frame. Each
// is left null when it should not be shown, which the presenter reads as "no
// change" - so a panel is only re-uploaded when its bitmap has actually been
// rebuilt.
board_overlays polygon_renderer_gpu::collect_board_overlays() {
    poll_bezel();
    board_overlays panels;
    if (tick_status_overlay()) {
        panels.status = m_fps_pixels.data();
        panels.status_width = m_fps_texture_width;
        panels.status_height = m_fps_texture_height;
    } else {
        // An empty panel clears whatever the slot last held.
        panels.status = m_blank_overlay.data();
        panels.status_width = 1;
        panels.status_height = 1;
    }
    if (m_settings_visible) {
        if (m_settings_texture_dirty) update_settings_texture();
        if (!m_settings_pixels.empty()) {
            panels.menu = m_settings_pixels.data();
            panels.menu_width = settings_width;
            panels.menu_height = settings_height;
        }
    } else {
        panels.menu = m_blank_overlay.data();
        panels.menu_width = 1;
        panels.menu_height = 1;
    }
    ensure_player_labels();
    const bool labels_wanted =
        m_display_settings.output == output_mode::dual && !m_settings_visible;
    for (int pane = 0; pane < 2; ++pane) {
        const std::size_t index = static_cast<std::size_t>(pane);
        if (labels_wanted && !m_player_label_pixels[index].empty()) {
            panels.player_label[index] = m_player_label_pixels[index].data();
            panels.player_label_width[index] = m_player_label_w[pane];
            panels.player_label_height[index] = m_player_label_h[pane];
        } else {
            panels.player_label[index] = m_blank_overlay.data();
            panels.player_label_width[index] = 1;
            panels.player_label_height[index] = 1;
        }
    }
    return panels;
}

// Presents a scene the native path has just rasterised, reporting once if
// either half declines. Both board families need exactly this, and a silent
// refusal here is indistinguishable from a hang.
bool polygon_renderer_gpu::present_native_scene(bool rasterised,
                                                const char* board, int width,
                                                int height) {
    static int native_entries = 0;
    if (log_first(native_entries))
        whitty_wall_log::note("present_native_scene(%s): rasterised=%d "
                              "alternate=%d", board, rasterised ? 1 : 0,
                              m_alternate_presenter ? 1 : 0);
    const bool presented = rasterised &&
        m_alternate_presenter->render_board_frame(
            nullptr, 0, 0, true, collect_board_overlays(), m_settings_visible,
            width, height);
    if (!presented && !m_native_decline_reported) {
        m_native_decline_reported = true;
        std::fprintf(stderr,
                     "%s native path declined: scene=%s present=%s - falling "
                     "back to OpenGL\n",
                     board, rasterised ? "ok" : "FAILED",
                     presented ? "ok" : "FAILED");
    }
    m_native_frame_live = presented;
    return presented;
}

// Installs the running board's bezel once the fetch finishes. Called from the
// frame path because that is the only place guaranteed to run while a board is
// live; it does nothing at all until a bezel actually arrives.
void polygon_renderer_gpu::poll_bezel() {
    if (!m_alternate_presenter) return;
    for (bezel::ready_bezel& ready : m_bezels.take_ready()) {
        if (ready.short_name != m_bezel_board) continue;
        // Held because the presenter uploads from this memory and the
        // library hands ownership over.
        m_bezel_pixels = std::move(ready.rgba);
        bezel_frame frame;
        frame.pixels = m_bezel_pixels.data();
        frame.width = ready.width;
        frame.height = ready.height;
        frame.cutout_x = ready.window.x;
        frame.cutout_y = ready.window.y;
        frame.cutout_width = ready.window.width;
        frame.cutout_height = ready.window.height;
        m_alternate_presenter->set_bezel(frame);
    }
}

bool polygon_renderer_gpu::tick_status_overlay() {
    const bool visible = !m_cabinet_status.empty() ||
        m_display_settings.show_fps || m_display_settings.show_renderer;
    if (!visible) return false;
    ++m_fps_frame_count;
    const auto now = std::chrono::steady_clock::now();
    if (m_fps_epoch.time_since_epoch().count() == 0) m_fps_epoch = now;
    const double elapsed =
        std::chrono::duration<double>(now - m_fps_epoch).count();
    if (elapsed >= 0.5) {
        m_last_fps = static_cast<double>(m_fps_frame_count) / elapsed;
        update_fps_texture(m_last_fps);
        m_fps_frame_count = 0;
        m_fps_epoch = now;
    }
    return true;
}

void polygon_renderer_gpu::refresh_output() {
    // A paused or menu-dismissed frame on the native Vulkan path is redrawn
    // from the image already resident on the GPU - there is no OpenGL texture
    // to replay.
    if (m_native_frame_live && m_alternate_presenter && !m_settings_visible &&
        m_alternate_presenter->repeat_board_frame())
        return;
    if (!m_ctx || !m_last_present_texture) return;
    m_ctx->make_current();
    present_texture(m_last_present_texture, m_last_present_width,
                    m_last_present_height, m_last_present_board_color,
                    m_last_present_flip_y, m_last_present_display_width,
                    m_last_present_display_height);
}

// A lost Vulkan device never comes back, and every further submit fails - a
// column in that state used to run invisibly forever. The OpenGL context is
// alive and has been rendering the same frames all along, so drop the dead
// presenter, show the GL window, and carry on: degraded presentation beats a
// game that plays to nobody.
// Keeps a wall column's GL window in its slot, the same maintenance the
// Vulkan presenter performs for its window. This exists for the device-loss
// failover: a column presenting through OpenGL would otherwise never be
// re-placed and ends up covering its neighbours.
void polygon_renderer_gpu::maintain_gl_wall_placement() {
    if (m_display_settings.wall_count <= 1 || !m_ctx) return;
    if (--m_gl_wall_countdown > 0) return;
    m_gl_wall_countdown = 30;
    const int focused = whitty_window::read_wall_focus();
    bool needed = !m_gl_wall_placed || focused != m_gl_wall_focus;
    if (!needed) {
        int x = 0, y = 0, width = 0, height = 0;
        if (whitty_window::compositor_window_geometry(x, y, width, height))
            needed = x != m_gl_wall_x || width != m_gl_wall_width ||
                     height != m_gl_wall_height;
    }
    if (!needed) return;
    m_gl_wall_focus = focused;
    m_gl_wall_placed = whitty_window::place_wall_slot(
        static_cast<SDL_Window*>(m_ctx->window), m_display_settings.wall_slot,
        m_display_settings.wall_count);
    if (m_gl_wall_placed) {
        int y = 0;
        whitty_window::compositor_window_geometry(m_gl_wall_x, y,
                                                  m_gl_wall_width,
                                                  m_gl_wall_height);
        whitty_wall_log::note("gl window placed at %d %dx%d", m_gl_wall_x,
                              m_gl_wall_width, m_gl_wall_height);
    }
}

void polygon_renderer_gpu::recover_from_device_loss() {
    if (!m_alternate_presenter || !m_alternate_presenter->device_lost())
        return;
    whitty_wall_log::note(
        "vulkan device lost - switching to OpenGL presentation");
    std::fprintf(stderr,
                 "Vulkan device lost; presenting through OpenGL instead\n");
    m_alternate_presenter->shutdown();
    m_alternate_presenter.reset();
    m_native_frame_live = false;
    m_active_backend = renderer_backend::opengl;
    if (!m_ctx) return;
    SDL_Window* window = static_cast<SDL_Window*>(m_ctx->window);
    SDL_ShowWindow(window);
    SDL_GetWindowSizeInPixels(window, &m_ctx->width, &m_ctx->height);
    m_ctx->width = std::max(m_ctx->width, 1);
    m_ctx->height = std::max(m_ctx->height, 1);
    // Placement starts over for the GL window, and keeps being maintained
    // from the GL present path from here on.
    m_gl_wall_placed = false;
    m_gl_wall_countdown = 0;
    maintain_gl_wall_placement();
}

void polygon_renderer_gpu::present_rgba_frame(const uint8_t* pixels,
                                              int width, int height,
                                              int display_width,
                                              int display_height) {
    static int rgba_entries = 0;
    if (log_first(rgba_entries))
        whitty_wall_log::note("present_rgba_frame: pixels=%d %dx%d headless=%d "
                              "ctx=%d alternate=%d",
                              pixels ? 1 : 0, width, height, m_headless ? 1 : 0,
                              m_ctx ? 1 : 0, m_alternate_presenter ? 1 : 0);
    if (!pixels || width <= 0 || height <= 0 || m_headless || !m_ctx) return;
    if (m_title_capture_countdown > 0 && !m_title_capture_name.empty() &&
        --m_title_capture_countdown == 0) {
        // The board's own frame, before bezels or overlays: the launcher
        // icon is the game as it draws itself.
        if (title_capture_write(m_title_capture_name, pixels, width,
                                height, /*top_down=*/true))
            whitty_wall_log::note("title captured for %s",
                                  m_title_capture_name.c_str());
        m_title_capture_name.clear();
        m_title_capture_countdown = -1;
    }
    // The panel bitmaps are still authored through SDL_ttf into OpenGL
    // textures, so the context has to be current before they are collected -
    // even on the native path, which only wants their host-side pixels.
    m_ctx->make_current();
    recover_from_device_loss();

    // Boards that hand over a finished raster have no reason to visit OpenGL
    // when Vulkan owns the window: the pixels go straight into a sampled
    // image and the Vulkan post pipeline scales them. This skips a texture
    // upload, a framebuffer pass, a full glReadPixels and a CPU rescale. The
    // native path declines when it is unavailable, and the OpenGL route below
    // then runs exactly as before.
    if (m_alternate_presenter) {
        if (m_alternate_presenter->render_board_frame(
                pixels, width, height, true, collect_board_overlays(),
                m_settings_visible, display_width, display_height)) {
            m_native_frame_live = true;
            return;
        }
        m_native_frame_live = false;
    }

    if (!m_external_frame_texture)
        glGenTextures(1, &m_external_frame_texture);
    glBindTexture(GL_TEXTURE_2D, m_external_frame_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const GLint filter = m_display_settings.linear_filtering ?
        GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    if (width != m_external_frame_width || height != m_external_frame_height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_external_frame_width = width;
        m_external_frame_height = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    present_texture(m_external_frame_texture, width, height, false, true,
                    display_width, display_height);
}

void polygon_renderer_gpu::present_model2_frame(model2_gpu_frame frame) {
    constexpr std::size_t pixel_count =
        std::size_t{model2_gpu_frame::width} * model2_gpu_frame::height;
    const bool texture_packet = !frame.texture_sheet_0.empty() ||
                                !frame.texture_sheet_1.empty();
    const bool color_packet = !frame.luma.empty() || !frame.palette.empty() ||
                              !frame.color_translation.empty();
    if (m_headless || !m_ctx || !m_model2_program ||
        frame.base_rgba.size() != pixel_count ||
        frame.foreground_rgba.size() != pixel_count ||
        (texture_packet && (frame.texture_sheet_0.size() != 0x100000 ||
                            frame.texture_sheet_1.size() != 0x100000)) ||
        (color_packet && (frame.luma.size() < 0x8000 ||
                          frame.palette.size() < 0x4000 ||
                          frame.color_translation.size() < 0xc000)))
        return;
    m_ctx->make_current();

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // Both layers arrive every frame, so the OpenGL copies can be skipped
    // while Vulkan owns the picture - this is the largest of the duplicated
    // uploads at roughly 1.5 MB a frame.
    if (opengl_textures_needed()) {
        glBindTexture(GL_TEXTURE_2D, m_model2_base_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, model2_gpu_frame::width,
                        model2_gpu_frame::height, GL_RGBA, GL_UNSIGNED_BYTE,
                        frame.base_rgba.data());
        glBindTexture(GL_TEXTURE_2D, m_model2_foreground_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, model2_gpu_frame::width,
                        model2_gpu_frame::height, GL_RGBA, GL_UNSIGNED_BYTE,
                        frame.foreground_rgba.data());
    }
    // The Vulkan scene pass samples its own copies of the two 2D layers.
    // They are rewritten every frame, so they stream in with it. Sizes are
    // BYTES, not pixels: a streaming sheet refuses a short upload outright,
    // so passing the uint32 element count here left both layers permanently
    // black - polygons over nothing.
    if (m_alternate_presenter && native_model2_enabled()) {
        m_alternate_presenter->upload_scene_sheet(
            scene_sheet::model2_base, frame.base_rgba.data(),
            frame.base_rgba.size() * sizeof(uint32_t));
        m_alternate_presenter->upload_scene_sheet(
            scene_sheet::model2_foreground, frame.foreground_rgba.data(),
            frame.foreground_rgba.size() * sizeof(uint32_t));
    }
    if (texture_packet) {
        glBindTexture(GL_TEXTURE_2D_ARRAY, m_model2_sheet_texture);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 1024, 1024, 1,
                        GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                        frame.texture_sheet_0.data());
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 1, 1024, 1024, 1,
                        GL_RED_INTEGER, GL_UNSIGNED_BYTE,
                        frame.texture_sheet_1.data());
        // Both layers are contiguous in the Vulkan array image.
        if (m_alternate_presenter && native_model2_enabled()) {
            std::vector<uint8_t> both(frame.texture_sheet_0.size() +
                                      frame.texture_sheet_1.size());
            std::memcpy(both.data(), frame.texture_sheet_0.data(),
                        frame.texture_sheet_0.size());
            std::memcpy(both.data() + frame.texture_sheet_0.size(),
                        frame.texture_sheet_1.data(),
                        frame.texture_sheet_1.size());
            m_alternate_presenter->upload_scene_sheet(
                scene_sheet::model2_sheets, both.data(), both.size());
        }
    }
    if (color_packet) {
        glBindTexture(GL_TEXTURE_2D, m_model2_luma_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 128,
                        GL_RED_INTEGER, GL_UNSIGNED_BYTE, frame.luma.data());

        const std::vector<uint8_t> colors = model2_build_color_table(frame);
        glBindTexture(GL_TEXTURE_2D, m_model2_color_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 64,
                        GL_RGB, GL_UNSIGNED_BYTE, colors.data());
        if (m_alternate_presenter && native_model2_enabled()) {
            m_alternate_presenter->upload_scene_sheet(
                scene_sheet::model2_luma, frame.luma.data(),
                frame.luma.size());
            // Packed three-byte RGB, widened on the way in.
            m_alternate_presenter->upload_model2_color_table(colors.data(),
                                                             colors.size());
        }
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

    const std::vector<model2_draw_vertex> vertices =
        model2_build_draw_vertices(frame);

    // With Vulkan owning the window the whole Model 2 frame rasterises there:
    // base layer, polygons and foreground layer straight into the image the
    // post pass samples, with no glReadPixels round trip behind it.
    if (m_alternate_presenter && native_model2_enabled()) {
        model2_scene scene;
        scene.width = model2_gpu_frame::width;
        scene.height = model2_gpu_frame::height;
        scene.vertices = vertices.data();
        scene.vertex_count = vertices.size();
        scene.crtc_offset[0] = static_cast<float>(frame.horizontal_offset);
        scene.crtc_offset[1] = static_cast<float>(frame.vertical_offset);
        if (present_native_scene(
                m_alternate_presenter->render_model2_scene(scene), "Model 2",
                model2_gpu_frame::width, model2_gpu_frame::height))
            return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_model2_framebuffer);
    glViewport(0, 0, model2_gpu_frame::width, model2_gpu_frame::height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(m_model2_overlay_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_model2_base_texture);
    glUniform1i(glGetUniformLocation(m_model2_overlay_program,
                                     "layer_texture"), 0);
    glBindVertexArray(m_model2_overlay_array);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (!vertices.empty()) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glUseProgram(m_model2_program);
        glUniform2f(glGetUniformLocation(m_model2_program, "crtc_offset"),
                    static_cast<float>(frame.horizontal_offset),
                    static_cast<float>(frame.vertical_offset));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, m_model2_sheet_texture);
        glUniform1i(glGetUniformLocation(m_model2_program, "texture_sheets"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_model2_luma_texture);
        glUniform1i(glGetUniformLocation(m_model2_program, "luma_table"), 1);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_model2_color_texture);
        glUniform1i(glGetUniformLocation(m_model2_program, "color_table"), 2);
        glBindVertexArray(m_model2_vertex_array);
        glBindBuffer(GL_ARRAY_BUFFER, m_model2_vertex_buffer);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        static_cast<GLsizeiptr>(vertices.size() *
                                               sizeof(model2_draw_vertex)),
                        vertices.data());
        // The ordered depth values emulate Model 2's one-bit fill buffer:
        // newer windows and earlier Z buckets claim pixels first, while
        // discarded transparent texels remain available to later polygons.
        glDrawArrays(GL_TRIANGLES, 0,
                     static_cast<GLsizei>(vertices.size()));
    }

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(m_model2_overlay_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_model2_foreground_texture);
    glUniform1i(glGetUniformLocation(m_model2_overlay_program,
                                     "layer_texture"), 0);
    glBindVertexArray(m_model2_overlay_array);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);

    present_texture(m_model2_scene_texture, model2_gpu_frame::width,
                    model2_gpu_frame::height, false, false, 4, 3);
}

void polygon_renderer_gpu::read_framebuffer(uint32_t* output) {
    if (!output) return;
    if (m_headless) {
        std::fill(output, output +
                  std::size_t{SYSTEM22_SCREEN_WIDTH} *
                      SYSTEM22_SCREEN_HEIGHT,
                  0);
        return;
    }
    m_ctx->make_current();
    glReadBuffer(m_alternate_presenter ? GL_BACK : GL_FRONT);
    glReadPixels(0, 0, SYSTEM22_SCREEN_WIDTH, SYSTEM22_SCREEN_HEIGHT,
                 GL_BGRA, GL_UNSIGNED_BYTE, output);
    glReadBuffer(GL_BACK);
}

bool polygon_renderer_gpu::create_graphics_pipeline() {
    const uint32_t vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
    const uint32_t fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);
    if (!vertex_shader || !fragment_shader) {
        if (vertex_shader) glDeleteShader(vertex_shader);
        if (fragment_shader) glDeleteShader(fragment_shader);
        return false;
    }

    m_graphics_program = glCreateProgram();
    glAttachShader(m_graphics_program, vertex_shader);
    glAttachShader(m_graphics_program, fragment_shader);
    glLinkProgram(m_graphics_program);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    GLint linked = GL_FALSE;
    glGetProgramiv(m_graphics_program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[2048]{};
        glGetProgramInfoLog(m_graphics_program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "OpenGL program link failed: %s\n", log);
        destroy_graphics_pipeline();
        return false;
    }

    glGenVertexArrays(1, &m_vertex_array);
    glBindVertexArray(m_vertex_array);
    glGenBuffers(1, &m_vertex_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
    constexpr std::size_t vertex_buffer_bytes =
        std::size_t{SYSTEM22_MAX_POLYGONS_PER_FRAME} * 12 *
        sizeof(draw_vertex);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertex_buffer_bytes), nullptr,
                 GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(draw_vertex),
                          reinterpret_cast<void*>(offsetof(draw_vertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(draw_vertex),
                          reinterpret_cast<void*>(offsetof(draw_vertex, u_over_z)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, sizeof(draw_vertex),
                          reinterpret_cast<void*>(offsetof(draw_vertex, v_over_z)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(draw_vertex),
                          reinterpret_cast<void*>(offsetof(draw_vertex, brightness_over_z)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(draw_vertex),
                          reinterpret_cast<void*>(offsetof(draw_vertex, one_over_z)));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 2, GL_FLOAT, GL_FALSE, sizeof(draw_vertex),
                          reinterpret_cast<void*>(
                              offsetof(draw_vertex, palette_base)));
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, sizeof(draw_vertex),
                          reinterpret_cast<void*>(offsetof(draw_vertex, color_mode)));
    glEnableVertexAttribArray(7);
    glVertexAttribPointer(7, 1, GL_FLOAT, GL_FALSE, sizeof(draw_vertex),
                          reinterpret_cast<void*>(offsetof(draw_vertex, texture_bank)));
    glEnableVertexAttribArray(8);
    glVertexAttribPointer(8, 1, GL_FLOAT, GL_FALSE, sizeof(draw_vertex),
                          reinterpret_cast<void*>(offsetof(draw_vertex, direct)));
    glEnableVertexAttribArray(9);
    glVertexAttribPointer(9, 1, GL_FLOAT, GL_FALSE, sizeof(draw_vertex),
                          reinterpret_cast<void*>(offsetof(draw_vertex, fog)));
    glEnableVertexAttribArray(10);
    glVertexAttribPointer(10, 3, GL_FLOAT, GL_FALSE, sizeof(draw_vertex),
                          reinterpret_cast<void*>(offsetof(draw_vertex, fog_color_r)));
    glEnableVertexAttribArray(11);
    glVertexAttribPointer(11, 2, GL_FLOAT, GL_FALSE, sizeof(draw_vertex),
                          reinterpret_cast<void*>(offsetof(draw_vertex, viewport_x)));
    glEnableVertexAttribArray(12);
    glVertexAttribPointer(12, 4, GL_FLOAT, GL_FALSE, sizeof(draw_vertex),
                          reinterpret_cast<void*>(offsetof(draw_vertex, clip_left)));
    glEnableVertexAttribArray(13);
    glVertexAttribPointer(13, 3, GL_FLOAT, GL_FALSE, sizeof(draw_vertex),
                          reinterpret_cast<void*>(offsetof(draw_vertex, sprite)));
    return glGetError() == GL_NO_ERROR;
}

bool polygon_renderer_gpu::create_model2_pipeline() {
    const uint32_t vertex_shader =
        compile_shader(GL_VERTEX_SHADER, model2_vertex_shader_source);
    const uint32_t fragment_shader =
        compile_shader(GL_FRAGMENT_SHADER, model2_fragment_shader_source);
    const uint32_t overlay_vertex = compile_shader(
        GL_VERTEX_SHADER, model2_overlay_vertex_shader_source);
    const uint32_t overlay_fragment = compile_shader(
        GL_FRAGMENT_SHADER, model2_overlay_fragment_shader_source);
    if (!vertex_shader || !fragment_shader || !overlay_vertex ||
        !overlay_fragment) {
        if (vertex_shader) glDeleteShader(vertex_shader);
        if (fragment_shader) glDeleteShader(fragment_shader);
        if (overlay_vertex) glDeleteShader(overlay_vertex);
        if (overlay_fragment) glDeleteShader(overlay_fragment);
        return false;
    }

    const auto link = [](uint32_t vertex, uint32_t fragment) {
        const uint32_t program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (linked == GL_TRUE) return program;
        char log[2048]{};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "OpenGL Model 2 program link failed: %s\n", log);
        glDeleteProgram(program);
        return 0U;
    };
    m_model2_program = link(vertex_shader, fragment_shader);
    m_model2_overlay_program = link(overlay_vertex, overlay_fragment);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    glDeleteShader(overlay_vertex);
    glDeleteShader(overlay_fragment);
    if (!m_model2_program || !m_model2_overlay_program) {
        destroy_model2_pipeline();
        return false;
    }

    glGenVertexArrays(1, &m_model2_vertex_array);
    glBindVertexArray(m_model2_vertex_array);
    glGenBuffers(1, &m_model2_vertex_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, m_model2_vertex_buffer);
    constexpr std::size_t vertex_buffer_bytes =
        std::size_t{32768} * 6 * sizeof(model2_draw_vertex);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertex_buffer_bytes), nullptr,
                 GL_DYNAMIC_DRAW);
    const auto attribute = [](unsigned location, int components,
                              std::size_t offset) {
        glEnableVertexAttribArray(location);
        glVertexAttribPointer(location, components, GL_FLOAT, GL_FALSE,
                              sizeof(model2_draw_vertex),
                              reinterpret_cast<void*>(offset));
    };
    attribute(0, 3, offsetof(model2_draw_vertex, x));
    attribute(1, 2, offsetof(model2_draw_vertex, u));
    attribute(2, 2, offsetof(model2_draw_vertex, center_x));
    attribute(3, 4, offsetof(model2_draw_vertex, clip_left));
    attribute(4, 1, offsetof(model2_draw_vertex, header0));
    attribute(5, 1, offsetof(model2_draw_vertex, header1));
    attribute(6, 1, offsetof(model2_draw_vertex, header2));
    attribute(7, 1, offsetof(model2_draw_vertex, color_base));
    attribute(8, 1, offsetof(model2_draw_vertex, luma));
    attribute(9, 1, offsetof(model2_draw_vertex, renderer));
    attribute(10, 1, offsetof(model2_draw_vertex, texture_lod));
    attribute(11, 1, offsetof(model2_draw_vertex, draw_priority));

    glGenVertexArrays(1, &m_model2_overlay_array);
    return glGetError() == GL_NO_ERROR;
}

bool polygon_renderer_gpu::create_text_pipeline() {
    const uint32_t vertex_shader =
        compile_shader(GL_VERTEX_SHADER, text_vertex_shader_source);
    const uint32_t fragment_shader =
        compile_shader(GL_FRAGMENT_SHADER, text_fragment_shader_source);
    if (!vertex_shader || !fragment_shader) {
        if (vertex_shader) glDeleteShader(vertex_shader);
        if (fragment_shader) glDeleteShader(fragment_shader);
        return false;
    }

    m_text_program = glCreateProgram();
    glAttachShader(m_text_program, vertex_shader);
    glAttachShader(m_text_program, fragment_shader);
    glLinkProgram(m_text_program);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    GLint linked = GL_FALSE;
    glGetProgramiv(m_text_program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[2048]{};
        glGetProgramInfoLog(m_text_program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "OpenGL text program link failed: %s\n", log);
        destroy_text_pipeline();
        return false;
    }

    glGenVertexArrays(1, &m_text_vertex_array);
    return glGetError() == GL_NO_ERROR;
}

bool polygon_renderer_gpu::create_post_pipeline() {
    const uint32_t vertex_shader =
        compile_shader(GL_VERTEX_SHADER, post_vertex_shader_source);
    const uint32_t fragment_shader =
        compile_shader(GL_FRAGMENT_SHADER, post_fragment_shader_source);
    if (!vertex_shader || !fragment_shader) {
        if (vertex_shader) glDeleteShader(vertex_shader);
        if (fragment_shader) glDeleteShader(fragment_shader);
        return false;
    }

    m_post_program = glCreateProgram();
    glAttachShader(m_post_program, vertex_shader);
    glAttachShader(m_post_program, fragment_shader);
    glLinkProgram(m_post_program);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    GLint linked = GL_FALSE;
    glGetProgramiv(m_post_program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[2048]{};
        glGetProgramInfoLog(m_post_program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "OpenGL post-process program link failed: %s\n", log);
        destroy_post_pipeline();
        return false;
    }

    glGenVertexArrays(1, &m_post_vertex_array);
    return glGetError() == GL_NO_ERROR;
}

bool polygon_renderer_gpu::create_settings_overlay() {
    const uint32_t vertex_shader =
        compile_shader(GL_VERTEX_SHADER, settings_vertex_shader_source);
    const uint32_t fragment_shader =
        compile_shader(GL_FRAGMENT_SHADER, settings_fragment_shader_source);
    if (!vertex_shader || !fragment_shader) {
        if (vertex_shader) glDeleteShader(vertex_shader);
        if (fragment_shader) glDeleteShader(fragment_shader);
        return false;
    }

    m_settings_program = glCreateProgram();
    glAttachShader(m_settings_program, vertex_shader);
    glAttachShader(m_settings_program, fragment_shader);
    glLinkProgram(m_settings_program);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    GLint linked = GL_FALSE;
    glGetProgramiv(m_settings_program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[2048]{};
        glGetProgramInfoLog(m_settings_program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "OpenGL settings program link failed: %s\n", log);
        destroy_settings_overlay();
        return false;
    }

    if (!TTF_Init()) {
        std::fprintf(stderr, "SDL_ttf initialization failed: %s\n", SDL_GetError());
        destroy_settings_overlay();
        return false;
    }
    m_ttf_initialized = true;
    for (const std::filesystem::path& path : whitty_platform::font_paths()) {
        m_settings_font = TTF_OpenFont(path.string().c_str(), 20);
        if (m_settings_font) break;
    }
    if (!m_settings_font) {
        std::fprintf(stderr, "Could not open a settings font: %s\n", SDL_GetError());
        destroy_settings_overlay();
        return false;
    }

    glGenVertexArrays(1, &m_settings_vertex_array);
    glGenTextures(1, &m_settings_texture);
    glBindTexture(GL_TEXTURE_2D, m_settings_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &m_fps_texture);
    glBindTexture(GL_TEXTURE_2D, m_fps_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    update_fps_texture(0.0);
    update_settings_texture();
    return glGetError() == GL_NO_ERROR;
}

void polygon_renderer_gpu::update_settings_texture() {
    if (!m_settings_font || !m_settings_texture) return;
    SDL_Surface* panel = SDL_CreateSurface(
        settings_width, settings_height, SDL_PIXELFORMAT_RGBA32);
    if (!panel) return;

    const SDL_PixelFormatDetails* panel_fmt =
        SDL_GetPixelFormatDetails(panel->format);
    const auto color = [panel_fmt](uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        return SDL_MapRGBA(panel_fmt, nullptr, r, g, b, a);
    };
    SDL_FillSurfaceRect(panel, nullptr, color(8, 13, 20, 238));
    const SDL_Rect top_border{0, 0, settings_width, 4};
    const SDL_Rect bottom_border{0, settings_height - 4, settings_width, 4};
    SDL_FillSurfaceRect(panel, &top_border, color(56, 198, 255, 255));
    SDL_FillSurfaceRect(panel, &bottom_border, color(56, 198, 255, 255));

    const auto draw_text = [this, panel](int x, int y, const std::string& text,
                                         SDL_Color text_color) {
        SDL_Surface* rendered =
            TTF_RenderText_Blended(m_settings_font, text.c_str(), text.size(),
                                   text_color);
        if (!rendered) return;
        SDL_Rect destination{x, y, rendered->w, rendered->h};
        SDL_BlitSurface(rendered, nullptr, panel, &destination);
        SDL_DestroySurface(rendered);
    };

    const SDL_Color white{238, 245, 250, 255};
    const SDL_Color muted{159, 177, 190, 255};
    const SDL_Color accent{70, 208, 255, 255};
    if (m_pause_menu_visible) {
        draw_text(24, 16, "PAUSED", accent);
        draw_text(24, 50,
                  "Choose an option with the keyboard, D-pad or left stick.",
                  muted);
        const std::array<std::string, pause_menu_row_count> labels{{
            "Play / Resume game",
            "Operator / DIP settings",
            "Emulator settings",
            "Controllers / Keyboard",
            "Change game / ROM selector",
            "Return to arcade selector",
        }};
        constexpr int row_top = 96;
        constexpr int row_height = 48;
        for (int row = 0; row < pause_menu_row_count; ++row) {
            const int y = row_top + row * row_height;
            if (row == m_pause_menu_selection) {
                const SDL_Rect highlight{16, y - 6, settings_width - 32,
                                         row_height - 4};
                SDL_FillSurfaceRect(panel, &highlight, color(19, 75, 101, 245));
                draw_text(25, y, ">", accent);
            }
            draw_text(45, y, labels[static_cast<std::size_t>(row)], white);
        }
        draw_text(24, 472,
                  "STICK / D-PAD: SELECT  A: OPEN  B OR P: PLAY",
                  muted);
    } else if (m_operator_menu_visible) {
        draw_text(24, 16, m_operator_menu.title, accent);
        std::string description = m_operator_menu.description;
        if (description.size() > 68)
            description = description.substr(0, 65) + "...";
        draw_text(24, 50, description, muted);

        constexpr int row_height = 32;
        constexpr int content_top = 88;
        constexpr int visible_rows = 11;
        const int row_count = static_cast<int>(m_operator_menu.rows.size());
        const int first_row = std::clamp(
            m_operator_selection - visible_rows + 1, 0,
            std::max(0, row_count - visible_rows));
        const int last_row = std::min(row_count, first_row + visible_rows);
        for (int index = first_row; index < last_row; ++index) {
            const operator_menu_row& row = m_operator_menu.rows[
                static_cast<std::size_t>(index)];
            const int y = content_top + (index - first_row) * row_height;
            if (index == m_operator_selection && row.enabled) {
                const SDL_Rect highlight{16, y - 3, settings_width - 32,
                                         row_height - 2};
                SDL_FillSurfaceRect(panel, &highlight, color(19, 75, 101, 245));
                draw_text(25, y, ">", accent);
            }
            draw_text(45, y, row.label, row.enabled ? white : muted);
            if (!row.values.empty()) {
                const int selected = std::clamp(
                    row.selected, 0, static_cast<int>(row.values.size()) - 1);
                std::string value = row.values[static_cast<std::size_t>(selected)];
                if (value.size() > 24) value = value.substr(0, 21) + "...";
                draw_text(330, y, value, row.enabled ? accent : muted);
            } else if (row.action && row.enabled) {
                draw_text(486, y, "ENTER", accent);
            }
        }
        draw_text(24, 448, "B / BACKSPACE: PAUSE MENU", muted);
        draw_text(24, 476,
                  "STICK / D-PAD: CHANGE  A: APPLY", muted);
    } else if (m_rom_menu_visible) {
        draw_text(24, 16, "GO ARCADE / GAMES", accent);
        draw_text(330, 18, "B / BACKSPACE: BACK", muted);
        if (m_rom_choices.empty()) {
            draw_text(32, 76, "No supported arcade ROM sets found.", muted);
        } else {
            constexpr int row_height = 32;
            constexpr int content_top = 94;
            constexpr int content_bottom = 458;
            const arcade_board_type active_board = active_rom_board();
            const auto board_count = [this](arcade_board_type board) {
                return std::count_if(
                    m_rom_choices.begin(), m_rom_choices.end(),
                    [board](const rom_choice& choice) {
                        return choice.board == board;
                    });
            };
            const SDL_Rect board_tab{16, 48, settings_width - 32, 34};
            SDL_FillSurfaceRect(panel, &board_tab, color(19, 75, 101, 245));
            draw_text(26, 54,
                      "<  " + std::string(arcade_board(active_board).menu_name) +
                          "  (" +
                          std::to_string(board_count(active_board)) + ")  >",
                      accent);

            std::vector<std::size_t> visible_choices;
            for (std::size_t index = 0; index < m_rom_choices.size(); ++index) {
                if (m_rom_choices[index].board == active_board)
                    visible_choices.push_back(index);
            }
            const auto selected = std::find(
                visible_choices.begin(), visible_choices.end(),
                static_cast<std::size_t>(m_rom_selection));
            const int selected_row = selected == visible_choices.end() ? 0 :
                static_cast<int>(selected - visible_choices.begin());
            const int visible_rows =
                (content_bottom - content_top) / row_height;
            const int first_row = std::clamp(
                selected_row - visible_rows + 1, 0,
                std::max(0, static_cast<int>(visible_choices.size()) -
                              visible_rows));
            const int last_row = std::min(
                static_cast<int>(visible_choices.size()),
                first_row + visible_rows);
            for (int row = first_row; row < last_row; ++row) {
                const std::size_t index = visible_choices[row];
                const rom_choice& choice = m_rom_choices[index];
                const int y = content_top + (row - first_row) * row_height;
                if (static_cast<int>(index) == m_rom_selection) {
                    const SDL_Rect highlight{16, y - 3, settings_width - 32,
                                             row_height - 2};
                    SDL_FillSurfaceRect(panel, &highlight, color(19, 75, 101, 245));
                    draw_text(25, y, ">", accent);
                }
                std::string label = choice.label;
                if (label.size() > 48) label = label.substr(0, 45) + "...";
                draw_text(45, y, label, white);
            }
        }
        draw_text(24, 472,
                  "STICK / D-PAD: BOARD / GAME  A: LOAD", muted);
    } else {
        draw_text(24, 16, "EMULATOR SETTINGS", accent);
        draw_text(330, 18, "B / BACKSPACE: BACK", muted);

        const std::array<std::string, settings_row_count> labels{
            "Master volume", "Music", "Effects / speech", "Fullscreen",
            "Window size", "VSync", "Integer scaling",
            "Filtering", "Output backend", "Show renderer", "Show FPS",
            "Screen output",
            "Play / Resume game", "Go Arcade / Games", "Operator / DIP settings",
        };
        for (int row = 0; row < settings_row_count; ++row) {
            const int y = settings_row_top + row * settings_row_height;
            if (row == m_settings_selection) {
                const SDL_Rect highlight{16, y - 3, settings_width - 32,
                                         settings_row_height - 1};
                SDL_FillSurfaceRect(panel, &highlight, color(19, 75, 101, 245));
                draw_text(25, y, ">", accent);
            }
            draw_text(45, y, labels[row], white);

            if (row <= 2) {
                const int value = row == 0 ? m_display_settings.master_volume :
                                  row == 1 ? m_display_settings.music_volume :
                                             m_display_settings.effects_volume;
                const int maximum = row == 0 ? 200 : 100;
                const SDL_Rect meter{300, y + 7, 145, 10};
                const SDL_Rect fill{300, y + 7, 145 * value / maximum, 10};
                SDL_FillSurfaceRect(panel, &meter, color(46, 57, 65, 255));
                SDL_FillSurfaceRect(panel, &fill, color(56, 198, 255, 255));
                draw_text(462, y, std::to_string(value) + "%", white);
                continue;
            }

            std::string value;
            switch (row) {
            case 3: value = m_display_settings.fullscreen ? "ON" : "OFF"; break;
            case 4:
                value = std::to_string(m_display_settings.window_width) +
                        " x " +
                        std::to_string(m_display_settings.window_width * 3 / 4);
                break;
            case 5: value = m_display_settings.vsync ? "ON" : "OFF"; break;
            case 6: value = m_display_settings.integer_scaling ? "ON" : "OFF"; break;
            case 7: value = m_display_settings.linear_filtering ? "LINEAR" : "PIXEL SHARP"; break;
            case 8: {
                value = renderer_backend_name(m_display_settings.renderer);
                std::transform(value.begin(), value.end(), value.begin(),
                               [](unsigned char c) {
                                   return static_cast<char>(std::toupper(c));
                               });
                break;
            }
            case 9: value = m_display_settings.show_renderer ? "ON" : "OFF"; break;
            case 10: value = m_display_settings.show_fps ? "ON" : "OFF"; break;
            case 11:
                value = m_single_screen_only ? "SINGLE (GAME)" :
                        m_display_settings.output == output_mode::dual ?
                            "DUAL SCREEN" : "SINGLE";
                break;
            case 12: value = m_paused ? "PAUSED" : "RUNNING"; break;
            default: break;
            }
            if (!value.empty())
                draw_text(300, y, value, white);
        }
        draw_text(24, 472,
                  "STICK / D-PAD: CHANGE  A: APPLY",
                  muted);
    }

    glBindTexture(GL_TEXTURE_2D, m_settings_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, settings_width, settings_height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, panel->pixels);
    // Keep a tightly packed host copy for the Vulkan path, which uploads the
    // panel itself rather than reading it back out of an OpenGL texture.
    m_settings_pixels.resize(
        static_cast<std::size_t>(settings_width) * settings_height * 4);
    for (int y = 0; y < settings_height; ++y)
        std::memcpy(m_settings_pixels.data() +
                        static_cast<std::size_t>(y) * settings_width * 4,
                    static_cast<const uint8_t*>(panel->pixels) +
                        static_cast<std::size_t>(y) * panel->pitch,
                    static_cast<std::size_t>(settings_width) * 4);
    SDL_DestroySurface(panel);
    m_settings_texture_dirty = false;
}

void polygon_renderer_gpu::update_fps_texture(double fps) {
    if (!m_settings_font || !m_fps_texture) return;
    std::string label = m_cabinet_status;
    if (m_display_settings.show_renderer) {
        std::string renderer = renderer_backend_name(m_active_backend);
        std::transform(renderer.begin(), renderer.end(), renderer.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::toupper(c));
                       });
        if (!label.empty()) label += "  |  ";
        label += renderer;
    }
    if (m_display_settings.show_fps) {
        char fps_label[24]{};
        std::snprintf(fps_label, sizeof(fps_label), "FPS %5.1f", fps);
        if (!label.empty()) label += "  |  ";
        label += fps_label;
    }
    if (label.empty()) label = " ";
    const SDL_Color color{238, 245, 250, 255};
    SDL_Surface* text = TTF_RenderText_Blended(
        m_settings_font, label.c_str(), label.size(), color);
    if (!text) return;
    constexpr int padding = 8;
    SDL_Surface* panel = SDL_CreateSurface(
        text->w + padding * 2, text->h + padding, SDL_PIXELFORMAT_RGBA32);
    if (!panel) {
        SDL_DestroySurface(text);
        return;
    }
    const SDL_PixelFormatDetails* panel_fmt =
        SDL_GetPixelFormatDetails(panel->format);
    SDL_FillSurfaceRect(panel, nullptr,
                        SDL_MapRGBA(panel_fmt, nullptr, 8, 13, 20, 220));
    SDL_Rect destination{padding, padding / 2, text->w, text->h};
    SDL_BlitSurface(text, nullptr, panel, &destination);
    SDL_DestroySurface(text);
    m_fps_texture_width = panel->w;
    m_fps_texture_height = panel->h;
    m_fps_pixels.resize(static_cast<std::size_t>(panel->w) * panel->h * 4);
    for (int y = 0; y < panel->h; ++y) {
        std::memcpy(m_fps_pixels.data() +
                        static_cast<std::size_t>(y) * panel->w * 4,
                    static_cast<const uint8_t*>(panel->pixels) +
                        static_cast<std::size_t>(y) * panel->pitch,
                    static_cast<std::size_t>(panel->w) * 4);
    }
    glBindTexture(GL_TEXTURE_2D, m_fps_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, panel->w, panel->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, panel->pixels);
    SDL_DestroySurface(panel);
}

void polygon_renderer_gpu::ensure_player_labels() {
    if (m_player_labels_ready || !m_settings_font) return;
    m_player_labels_ready = true;
    const std::array<const char*, 2> texts{"PLAYER 1", "PLAYER 2"};
    // Match the light-gun sight colours: cyan for P1, pink for P2.
    const std::array<SDL_Color, 2> colours{{{56, 214, 255, 255},
                                            {255, 92, 140, 255}}};
    for (int i = 0; i < 2; ++i) {
        SDL_Surface* text = TTF_RenderText_Blended(
            m_settings_font, texts[i], std::strlen(texts[i]), colours[i]);
        if (!text) continue;
        constexpr int padding = 8;
        SDL_Surface* panel = SDL_CreateSurface(
            text->w + padding * 2, text->h + padding, SDL_PIXELFORMAT_RGBA32);
        if (!panel) {
            SDL_DestroySurface(text);
            continue;
        }
        const SDL_PixelFormatDetails* fmt =
            SDL_GetPixelFormatDetails(panel->format);
        SDL_FillSurfaceRect(panel, nullptr,
                            SDL_MapRGBA(fmt, nullptr, 8, 13, 20, 200));
        SDL_Rect destination{padding, padding / 2, text->w, text->h};
        SDL_BlitSurface(text, nullptr, panel, &destination);
        SDL_DestroySurface(text);
        if (!m_player_label_texture[i]) glGenTextures(1, &m_player_label_texture[i]);
        glBindTexture(GL_TEXTURE_2D, m_player_label_texture[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, panel->w, panel->h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, panel->pixels);
        m_player_label_w[i] = panel->w;
        m_player_label_h[i] = panel->h;
        // Host copy for the Vulkan overlay pipeline, packed to width*4.
        m_player_label_pixels[static_cast<std::size_t>(i)].resize(
            static_cast<std::size_t>(panel->w) * panel->h * 4);
        for (int row = 0; row < panel->h; ++row)
            std::memcpy(m_player_label_pixels[static_cast<std::size_t>(i)]
                                .data() +
                            static_cast<std::size_t>(row) * panel->w * 4,
                        static_cast<const uint8_t*>(panel->pixels) +
                            static_cast<std::size_t>(row) * panel->pitch,
                        static_cast<std::size_t>(panel->w) * 4);
        SDL_DestroySurface(panel);
    }
}

void polygon_renderer_gpu::draw_fps_overlay() {
    if (!m_fps_texture || m_fps_texture_width <= 0 ||
        m_fps_texture_height <= 0)
        return;
    glViewport(12, m_ctx->height - m_fps_texture_height - 12,
               m_fps_texture_width, m_fps_texture_height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(m_settings_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_fps_texture);
    glUniform1i(glGetUniformLocation(m_settings_program, "settings_texture"), 0);
    glBindVertexArray(m_settings_vertex_array);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void polygon_renderer_gpu::draw_settings_overlay() {
    if (m_settings_texture_dirty) update_settings_texture();
    // This is a docked host UI, not part of the emulated framebuffer. Keep
    // its physical size stable as the game window is resized and pin its top
    // edge to the window instead of floating it over the centre of the game.
    const float scale = std::min(
        1.0f, std::min(static_cast<float>(m_ctx->width) / settings_width,
                       static_cast<float>(m_ctx->height) / settings_height));
    const int width = std::max(1, static_cast<int>(settings_width * scale));
    const int height = std::max(1, static_cast<int>(settings_height * scale));
    const int x = (m_ctx->width - width) / 2;
    const int y = m_ctx->height - height;

    glViewport(x, y, width, height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(m_settings_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_settings_texture);
    glUniform1i(glGetUniformLocation(m_settings_program, "settings_texture"), 0);
    glBindVertexArray(m_settings_vertex_array);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void polygon_renderer_gpu::destroy_graphics_pipeline() {
    if (m_vertex_buffer) glDeleteBuffers(1, &m_vertex_buffer);
    if (m_vertex_array) glDeleteVertexArrays(1, &m_vertex_array);
    if (m_graphics_program) glDeleteProgram(m_graphics_program);
    m_vertex_buffer = 0;
    m_vertex_array = 0;
    m_graphics_program = 0;
}

void polygon_renderer_gpu::destroy_model2_pipeline() {
    if (m_model2_vertex_buffer)
        glDeleteBuffers(1, &m_model2_vertex_buffer);
    if (m_model2_vertex_array)
        glDeleteVertexArrays(1, &m_model2_vertex_array);
    if (m_model2_overlay_array)
        glDeleteVertexArrays(1, &m_model2_overlay_array);
    if (m_model2_program) glDeleteProgram(m_model2_program);
    if (m_model2_overlay_program) glDeleteProgram(m_model2_overlay_program);
    m_model2_vertex_buffer = 0;
    m_model2_vertex_array = 0;
    m_model2_overlay_array = 0;
    m_model2_program = 0;
    m_model2_overlay_program = 0;
}

void polygon_renderer_gpu::destroy_text_pipeline() {
    if (m_text_vertex_array) glDeleteVertexArrays(1, &m_text_vertex_array);
    if (m_text_program) glDeleteProgram(m_text_program);
    m_text_vertex_array = 0;
    m_text_program = 0;
}

void polygon_renderer_gpu::destroy_post_pipeline() {
    if (m_post_vertex_array) glDeleteVertexArrays(1, &m_post_vertex_array);
    if (m_post_program) glDeleteProgram(m_post_program);
    m_post_vertex_array = 0;
    m_post_program = 0;
}

void polygon_renderer_gpu::destroy_settings_overlay() {
    if (m_settings_texture) glDeleteTextures(1, &m_settings_texture);
    if (m_fps_texture) glDeleteTextures(1, &m_fps_texture);
    for (int i = 0; i < 2; ++i) {
        if (m_player_label_texture[i])
            glDeleteTextures(1, &m_player_label_texture[i]);
        m_player_label_texture[i] = 0;
    }
    m_player_labels_ready = false;
    if (m_settings_vertex_array)
        glDeleteVertexArrays(1, &m_settings_vertex_array);
    if (m_settings_program) glDeleteProgram(m_settings_program);
    m_settings_texture = 0;
    m_fps_texture = 0;
    m_settings_vertex_array = 0;
    m_settings_program = 0;
    if (m_settings_font) {
        TTF_CloseFont(m_settings_font);
        m_settings_font = nullptr;
    }
    if (m_ttf_initialized) {
        TTF_Quit();
        m_ttf_initialized = false;
    }
}

// opengl_context implementation
bool opengl_context::create(void* window, int w, int h) {
    this->width = w;
    this->height = h;
    this->window = window;
    return true;
}

void opengl_context::destroy() {
    if (window2) SDL_DestroyWindow(static_cast<SDL_Window*>(window2));
    if (gl_context) SDL_GL_DestroyContext(static_cast<SDL_GLContext>(gl_context));
    if (window) SDL_DestroyWindow(static_cast<SDL_Window*>(window));
    window2 = nullptr;
    twin_layout_attempts = 0;
    gl_context = nullptr;
    window = nullptr;
}

bool opengl_context::make_current() {
    return window && gl_context &&
           SDL_GL_MakeCurrent(static_cast<SDL_Window*>(window),
                              static_cast<SDL_GLContext>(gl_context));
}
