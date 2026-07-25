// Vulkan port of text_fragment_shader_source in src/arcade_renderer.cpp: the
// System 22 character/tilemap layer drawn over the polygon scene.
//
// As in system22_polygon.frag, screen_y reads gl_FragCoord.y directly because
// Vulkan's fragment origin is already the top-left. The OpenGL version's
// "479 -" flip exists only to undo GL's bottom-left origin.
#version 450

layout(location = 0) out vec4 output_color;

layout(std140, set = 0, binding = 0) uniform system22_text_uniforms {
    ivec4 text_scroll;      // xy = scroll, z = palette base
    uvec4 super_fade_color; // xyz = colour
    uvec4 super_control;    // x super_system22, y fade factor, z mixer flags
} uniforms;

layout(set = 0, binding = 1) uniform usampler2D character_data;
layout(set = 0, binding = 2) uniform usampler2D text_map;
layout(set = 0, binding = 3) uniform usampler2DArray palette_data;

void main() {
    const int screen_x = int(gl_FragCoord.x);
    const int screen_y = int(gl_FragCoord.y);
    const int source_x = (screen_x + uniforms.text_scroll.x) & 0x3ff;
    const int source_y = (screen_y + uniforms.text_scroll.y) & 0x3ff;
    const uint entry = texelFetch(text_map,
        ivec2((source_x >> 4) & 0x3f, (source_y >> 4) & 0x3f), 0).r;

    uint pixel_x = uint(source_x & 0x0f);
    uint pixel_y = uint(source_y & 0x0f);
    if ((entry & 0x0400u) != 0u) pixel_x = 15u - pixel_x;
    if ((entry & 0x0800u) != 0u) pixel_y = 15u - pixel_y;
    const uint character = entry & 0x03ffu;
    const uint byte_address = character * 128u + pixel_y * 8u + (pixel_x >> 1u);
    const uint packed_byte = texelFetch(character_data,
        ivec2(int(byte_address & 0x1ffu), int(byte_address >> 9u)), 0).r;
    // Character RAM is addressed big-endian by the 68020. MAME's
    // WORD2_XOR_BE layout therefore decodes the high nibble first.
    const uint pen = ((pixel_x & 1u) == 0u) ? (packed_byte >> 4u) :
                                              (packed_byte & 0x0fu);
    if (pen == 0x0fu) discard;

    const uint palette_index =
        uint(uniforms.text_scroll.z) + ((entry >> 12u) * 16u) + pen;
    const ivec2 palette_coord = ivec2(int(palette_index & 0xffu),
                                      int((palette_index >> 8u) & 0x7fu));
    vec3 rgb = vec3(
        texelFetch(palette_data, ivec3(palette_coord, 0), 0).r,
        texelFetch(palette_data, ivec3(palette_coord, 1), 0).r,
        texelFetch(palette_data, ivec3(palette_coord, 2), 0).r) / 255.0;
    if (uniforms.super_control.x != 0u && uniforms.super_control.y != 0u &&
        (uniforms.super_control.z & 0x02u) != 0u)
        rgb = mix(rgb, vec3(uniforms.super_fade_color.xyz) / 255.0,
                  float(uniforms.super_control.y) / 255.0);
    output_color = vec4(rgb, 1.0);
}
