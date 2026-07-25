// Vulkan port of fragment_shader_source in src/arcade_renderer.cpp: System 22
// texture-sheet decoding, palette lookup, fog, shading and the dual-source
// blend that carries per-pixel opacity.
//
// The arithmetic is unchanged from the OpenGL original so both backends
// produce the same pixels. Two things are deliberately different:
//
//   The loose uniforms became one std140 block. They do not fit the 128-byte
//   push-constant size Vulkan guarantees, and silently exceeding that would
//   only fail on the drivers with the smallest limit.
//
//   screen_y reads gl_FragCoord.y directly. Vulkan's fragment origin is the
//   top-left, and system22_polygon.vert negates clip-space Y to match, so the
//   OpenGL version's "479 -" flip would double-invert the clip rectangle.
#version 450

layout(location = 0) noperspective in float u_over_z;
layout(location = 1) noperspective in float v_over_z;
layout(location = 2) noperspective in float brightness_over_z;
layout(location = 3) noperspective in float one_over_z;
layout(location = 4) noperspective in float fog_factor;
layout(location = 5) flat in uint palette_base;
layout(location = 6) flat in uint alpha_palette;
layout(location = 7) flat in uint color_mode;
layout(location = 8) flat in uint texture_bank;
layout(location = 9) flat in vec3 polygon_fog_color;
layout(location = 10) flat in vec4 polygon_clip_rect;
layout(location = 11) flat in uint sprite_mode;
layout(location = 12) flat in uint sprite_alpha;
layout(location = 13) flat in uint object_flags;

layout(std140, set = 0, binding = 0) uniform system22_uniforms {
    mat4 view_matrix;
    uvec4 super_fade_color;
    uvec4 super_poly_fade_color;
    uvec4 texture_control;
    uvec4 mixer_control;
    uvec4 alpha_control;
} uniforms;

layout(set = 0, binding = 1) uniform usampler2DArray texture_tiles;
layout(set = 0, binding = 2) uniform usampler2D texture_map;
layout(set = 0, binding = 3) uniform usampler2D texture_attr;
layout(set = 0, binding = 4) uniform usampler2DArray palette_data;
layout(set = 0, binding = 5) uniform usampler2DArray sprite_tiles;

layout(location = 0, index = 0) out vec4 output_color;
layout(location = 0, index = 1) out vec4 output_blend;

bool super_system22() { return uniforms.texture_control.z != 0u; }

vec3 apply_super_fade(vec3 rgb, uint target_flag) {
    const uint fade_factor = uniforms.texture_control.w;
    if (!super_system22() || fade_factor == 0u ||
        (uniforms.mixer_control.x & target_flag) == 0u)
        return rgb;
    return mix(rgb, vec3(uniforms.super_fade_color.xyz) / 255.0,
               float(fade_factor) / 255.0);
}

float source_opacity(uint pen) {
    if (!super_system22()) return 1.0;
    const uint alpha = sprite_mode != 0u ?
        255u - sprite_alpha : 255u - uniforms.alpha_control.x;
    if (alpha == 255u ||
        (alpha_palette == uniforms.mixer_control.z &&
         pen != uniforms.mixer_control.w))
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
    const int screen_x = int(gl_FragCoord.x);
    const int screen_y = int(gl_FragCoord.y);
    if (screen_x < int(polygon_clip_rect.x) ||
        screen_x > int(polygon_clip_rect.y) ||
        screen_y < int(polygon_clip_rect.z) ||
        screen_y > int(polygon_clip_rect.w))
        discard;

    const float q = max(one_over_z, 0.0000001);
    if (sprite_mode != 0u) {
        const uint ix = uint(clamp(int(u_over_z / q), 0, 31));
        const uint iy = uint(clamp(int(v_over_z / q), 0, 31));
        const uint address = texture_bank * 1024u + iy * 32u + ix;
        const uint layer = address >> 20u;
        const uint layer_offset = address & 0xfffffu;
        const uint pen = texelFetch(sprite_tiles,
            ivec3(int(layer_offset & 0x3ffu),
                  int(layer_offset >> 10u), int(layer)), 0).r;
        if (pen == 0xffu) discard;

        const uint palette_index = palette_base + pen;
        const ivec2 palette_coord = ivec2(int(palette_index & 0xffu),
                                          int((palette_index >> 8u) & 0x7fu));
        vec3 rgb = vec3(
            texelFetch(palette_data, ivec3(palette_coord, 0), 0).r,
            texelFetch(palette_data, ivec3(palette_coord, 1), 0).r,
            texelFetch(palette_data, ivec3(palette_coord, 2), 0).r) / 255.0;
        rgb = mix(rgb, polygon_fog_color, clamp(fog_factor, 0.0, 1.0));
        rgb = apply_super_fade(rgb, 0x02u);
        write_scene_color(rgb, pen, (color_mode & 1u) != 0u ? 1.0 : 0.0);
        return;
    }
    const uint tx = uint(int(u_over_z / q)) & 0xfffu;
    const uint ty = (uint(int(v_over_z / q)) & 0xfffu) | (texture_bank << 12u);
    // System 22 tilemap addressing: the 12-bit Y coordinate occupies the
    // upper 12 bits after its tile-row conversion, not bits 8..19.
    const uint tile_offset = ((ty << 4u) & 0xfff00u) | (tx >> 4u);

    uint tile = texelFetch(texture_map,
        ivec2(int(tile_offset & 0xffu), int((tile_offset >> 8u) & 0xfffu)),
        0).r;
    const uint packed_index = tile_offset >> 1u;
    const uint packed_attr = texelFetch(texture_attr,
        ivec2(int(packed_index & 0x3ffu), int(packed_index >> 10u)), 0).r;
    const uint attr = ((tile_offset & 1u) == 0u) ? (packed_attr >> 4u) :
                                                   (packed_attr & 0x0fu);
    if (uniforms.texture_control.y != 0u && (attr & 1u) == 0u)
        tile = (tile & 0x3fffu) | 0x8000u;

    uint ix = tx & 0x0fu;
    uint iy = ty & 0x0fu;
    if ((attr & 4u) != 0u) ix = 15u - ix;
    if ((attr & 2u) != 0u) iy = 15u - iy;
    if ((attr & 8u) != 0u) {
        const uint swap_value = ix;
        ix = iy;
        iy = swap_value;
    }

    uint pen = 0u;
    if (object_flags == 0u) {
        const uint tile_address = (tile << 8u) | (iy << 4u) | ix;
        if (tile_address >= uniforms.texture_control.x) {
            const uint relative = tile_address - uniforms.texture_control.x;
            const uint layer = relative >> 21u;
            const uint layer_offset = relative & 0x1fffffu;
            pen = texelFetch(texture_tiles,
                ivec3(int(layer_offset & 0x7ffu),
                      int(layer_offset >> 11u), int(layer)), 0).r;
        }
    }

    const uint effective_color_mode = object_flags != 0u ? 0u : color_mode;
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

    const uint palette_index =
        palette_base + pen_base + ((pen >> pen_shift) & pen_mask);
    const ivec2 palette_coord = ivec2(int(palette_index & 0xffu),
                                      int((palette_index >> 8u) & 0x7fu));
    vec3 rgb = vec3(
        texelFetch(palette_data, ivec3(palette_coord, 0), 0).r,
        texelFetch(palette_data, ivec3(palette_coord, 1), 0).r,
        texelFetch(palette_data, ivec3(palette_coord, 2), 0).r) / 255.0;
    const float shade = max(brightness_over_z / q, 0.0) / 64.0;
    const bool shade_enabled = (object_flags & 6u) == 0u;
    // Super System 22 shades before fog; the original board shades after it.
    // The shade is 8.6 fixed point, with 0x40 representing unity.
    if (super_system22() && shade_enabled)
        rgb = clamp(rgb * shade, 0.0, 1.0);
    rgb = mix(rgb, polygon_fog_color, clamp(fog_factor, 0.0, 1.0));
    if (!super_system22() && shade_enabled)
        rgb = clamp(rgb * shade, 0.0, 1.0);
    if (super_system22() && uniforms.mixer_control.y != 0u)
        rgb = clamp(rgb * (vec3(uniforms.super_poly_fade_color.xyz) / 256.0),
                    0.0, 1.0);
    rgb = apply_super_fade(rgb, 0x01u);
    // Non-Super System 22 keeps a one-bit per-pixel polygon-over-text flag.
    // Store the flag in the otherwise unused framebuffer alpha component;
    // the following text pass uses destination-alpha blending to honour it.
    const float over_text = ((color_mode & 7u) == 1u) ? 1.0 : 0.0;
    write_scene_color(rgb, pen, over_text);
}
