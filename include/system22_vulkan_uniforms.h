// system22_vulkan_uniforms.h - host mirror of the System 22 shader uniform
// blocks.
//
// These do not fit the 128 bytes of push-constant space Vulkan guarantees, so
// they travel in a std140 uniform buffer. Members are grouped into vec4s
// because std140 pads a uvec3 to sixteen bytes: writing them as loose scalars
// would put the host and the shader on different offsets on some compilers
// while still building cleanly on others.
//
// The offsets asserted below were read out of the compiled SPIR-V with
// spirv-dis; if a shader block changes, the build breaks here rather than
// producing quietly wrong colours on screen.
#pragma once

#include <cstddef>
#include <cstdint>

// Mirrors system22_uniforms in shaders/system22_polygon.{vert,frag}.
struct system22_uniform_block {
    float view_matrix[16]{};
    // xyz = Super System 22 screen fade colour.
    uint32_t super_fade_color[4]{};
    // xyz = Super System 22 polygon fade colour.
    uint32_t super_poly_fade_color[4]{};
    // x texture address base, y tile high bit from attribute,
    // z Super System 22 flag, w screen fade factor.
    uint32_t texture_control[4]{};
    // x mixer flags, y polygon fade enabled, z alpha colour, w alpha pen.
    uint32_t mixer_control[4]{};
    // x polygon alpha factor.
    uint32_t alpha_control[4]{};
};

static_assert(offsetof(system22_uniform_block, view_matrix) == 0);
static_assert(offsetof(system22_uniform_block, super_fade_color) == 64);
static_assert(offsetof(system22_uniform_block, super_poly_fade_color) == 80);
static_assert(offsetof(system22_uniform_block, texture_control) == 96);
static_assert(offsetof(system22_uniform_block, mixer_control) == 112);
static_assert(offsetof(system22_uniform_block, alpha_control) == 128);
static_assert(sizeof(system22_uniform_block) == 144);

// Byte offsets of each System 22 vertex attribute, shared by the Vulkan
// pipeline that declares them and by arcade_renderer.cpp, which asserts its
// draw_vertex still matches. Reordering that struct without updating these
// would feed the shader plausible-looking nonsense, so the build breaks
// instead.
namespace system22_vertex_layout {
inline constexpr std::size_t stride = 25 * sizeof(float);
inline constexpr std::size_t position = 0;
inline constexpr std::size_t u_over_z = 12;
inline constexpr std::size_t v_over_z = 16;
inline constexpr std::size_t brightness_over_z = 20;
inline constexpr std::size_t one_over_z = 24;
inline constexpr std::size_t palette = 28;
inline constexpr std::size_t color_mode = 36;
inline constexpr std::size_t texture_bank = 40;
inline constexpr std::size_t direct = 44;
inline constexpr std::size_t fog = 48;
inline constexpr std::size_t fog_color = 52;
inline constexpr std::size_t viewport = 64;
inline constexpr std::size_t clip_rect = 72;
inline constexpr std::size_t render_flags = 88;
} // namespace system22_vertex_layout

// Byte offsets of each Model 2 vertex attribute, shared by the Vulkan
// pipeline and asserted against model2_draw_vertex in arcade_renderer.cpp.
namespace model2_vertex_layout {
inline constexpr std::size_t stride = 19 * sizeof(float);
inline constexpr std::size_t position = 0;
inline constexpr std::size_t uv = 12;
inline constexpr std::size_t center = 20;
inline constexpr std::size_t clip = 28;
inline constexpr std::size_t header0 = 44;
inline constexpr std::size_t header1 = 48;
inline constexpr std::size_t header2 = 52;
inline constexpr std::size_t color_base = 56;
inline constexpr std::size_t luma = 60;
inline constexpr std::size_t renderer = 64;
inline constexpr std::size_t texture_lod = 68;
inline constexpr std::size_t draw_priority = 72;
} // namespace model2_vertex_layout

// Mirrors system22_text_uniforms in shaders/system22_text.frag.
struct system22_text_uniform_block {
    // xy = tilemap scroll, z = palette base.
    int32_t text_scroll[4]{};
    // xyz = Super System 22 screen fade colour.
    uint32_t super_fade_color[4]{};
    // x Super System 22 flag, y fade factor, z mixer flags.
    uint32_t super_control[4]{};
};

static_assert(offsetof(system22_text_uniform_block, text_scroll) == 0);
static_assert(offsetof(system22_text_uniform_block, super_fade_color) == 16);
static_assert(offsetof(system22_text_uniform_block, super_control) == 32);
static_assert(sizeof(system22_text_uniform_block) == 48);
