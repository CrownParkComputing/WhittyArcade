// Vulkan port of vertex_shader_source in src/arcade_renderer.cpp: the System
// 22 polygon, direct-polygon and sprite transform.
//
// Two conventions differ from OpenGL and are handled here rather than being
// pushed onto the caller:
//
//   Y direction. Board coordinates run downwards, and the OpenGL shader relies
//   on GL clip space pointing +Y up to land row zero at the top. Vulkan clip
//   space points +Y down, so every clip-space Y below is negated. That also
//   makes gl_FragCoord.y in the fragment stage read straight across as a
//   board scanline, with no 479-minus flip.
//
//   Depth range. GL maps NDC z in [-1,1] onto the depth buffer; Vulkan maps
//   [0,1]. The z values written here are already non-negative and the mapping
//   stays monotonic, so depth ordering between polygons is unchanged - only
//   the absolute buffer values differ, and nothing reads those back.
#version 450

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

// Grouped into vec4s so std140 padding cannot silently shift a member. The
// C++ mirror is system22_uniform_block in src/arcade_presenter.cpp.
layout(std140, set = 0, binding = 0) uniform system22_uniforms {
    mat4 view_matrix;
    uvec4 super_fade_color;        // xyz = colour
    uvec4 super_poly_fade_color;   // xyz = colour
    uvec4 texture_control;         // x address base, y high-bit-from-attr,
                                   // z super_system22, w super_fade_factor
    uvec4 mixer_control;           // x mixer flags, y poly fade enabled,
                                   // z alpha colour, w alpha pen
    uvec4 alpha_control;           // x = poly alpha factor
} uniforms;

layout(location = 0) noperspective out float u_over_z;
layout(location = 1) noperspective out float v_over_z;
layout(location = 2) noperspective out float brightness_over_z;
layout(location = 3) noperspective out float one_over_z;
layout(location = 4) noperspective out float fog_factor;
layout(location = 5) flat out uint palette_base;
layout(location = 6) flat out uint alpha_palette;
layout(location = 7) flat out uint color_mode;
layout(location = 8) flat out uint texture_bank;
layout(location = 9) flat out vec3 polygon_fog_color;
layout(location = 10) flat out vec4 polygon_clip_rect;
layout(location = 11) flat out uint sprite_mode;
layout(location = 12) flat out uint sprite_alpha;
layout(location = 13) flat out uint object_flags;

void main() {
    const vec4 position = uniforms.view_matrix * vec4(in_position, 1.0);
    if (in_render_flags.x > 0.5) {
        // Sprite coordinates are absolute native-screen pixels with a
        // top-left origin, unlike direct polygon coordinates which are
        // centred around the System 22 viewport.
        gl_Position = vec4(position.x / 320.0 - 1.0,
                           -(1.0 - position.y / 240.0), 0.0, 1.0);
    } else if (in_direct > 0.5) {
        gl_Position = vec4(position.x / 320.0, -(position.y / 240.0),
                           clamp(1.0 / max(position.z, 1.0), 0.0, 1.0),
                           1.0);
    } else {
        // Preserve camera-space Z as homogeneous W so the hardware performs
        // the perspective divide while clipping edges that cross the near
        // plane, avoiding the huge coordinates a manual divide produces.
        const float z = position.z;
        gl_Position = vec4((position.x + in_viewport.x * z) / 320.0,
                           -((position.y - in_viewport.y * z) / 240.0),
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
