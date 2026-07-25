// Vulkan port of post_fragment_shader_source in src/arcade_renderer.cpp.
// The arithmetic is kept identical so both backends produce the same pixels;
// only the resource declarations change. The GL version's output_size and
// output_origin uniforms are gone because the vertex stage now supplies the
// pane coordinate directly.
#version 450

layout(location = 0) in vec2 pane_uv;
layout(location = 0) out vec4 output_color;

layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform usampler2DArray gamma_data;

layout(push_constant) uniform post_constants {
    uvec3 screen_fade;
    uint super_system22;
    uint apply_board_color;
    uint flip_y;
    uint linear_filtering;
    // Size of this pane in output pixels, needed to work out how many of them
    // one source texel covers.
    float pane_width;
    float pane_height;
} constants;

// Sharp bilinear. Plain bilinear blends across a whole source texel, which is
// what makes an arcade raster look soft once it is scaled by a non-integer
// factor. This keeps each texel's interior flat - as nearest does - and
// confines the blend to a single output pixel at the texel boundary, so the
// picture stays crisp without the uneven pixel widths nearest produces.
vec2 sharp_texel_coordinate(vec2 uv, vec2 source_size) {
    const vec2 scale = max(vec2(constants.pane_width, constants.pane_height) /
                               source_size,
                           vec2(1.0));
    const vec2 texel = uv * source_size;
    const vec2 base = floor(texel);
    const vec2 offset = texel - base;
    // Half-width of the flat region either side of the texel centre.
    const vec2 flat_range = 0.5 - 0.5 / scale;
    const vec2 centre_distance = offset - 0.5;
    const vec2 ramp =
        (centre_distance - clamp(centre_distance, -flat_range, flat_range)) *
            scale + 0.5;
    return (base + ramp) / source_size;
}

uint apply_fade(uint component, uint factor) {
    // MAME's non-Super System 22 mixer seeds a zero component before a
    // greater-than-unity fade so black can brighten, then performs 8.8 fixed
    // point multiplication and clamps to one byte.
    if (factor > 0x100u && component == 0u) component = 1u;
    return min((component * factor) >> 8u, 255u);
}

void main() {
    const ivec2 source_size = textureSize(scene_color, 0);
    vec3 sampled;
    if (constants.linear_filtering != 0u) {
        // Sample at destination pixel centres. An integer-only texelFetch
        // path makes non-integer PS2 scaling repeat rows unevenly, visibly
        // squeezing fine HUD text even when LINEAR is selected.
        vec2 uv = pane_uv;
        if (constants.flip_y != 0u) uv.y = 1.0 - uv.y;
        sampled = texture(scene_color,
                          sharp_texel_coordinate(uv, vec2(source_size))).rgb;
    } else {
        ivec2 coordinate = ivec2(pane_uv * vec2(source_size));
        coordinate = clamp(coordinate, ivec2(0), source_size - ivec2(1));
        if (constants.flip_y != 0u)
            coordinate.y = source_size.y - 1 - coordinate.y;
        sampled = texelFetch(scene_color, coordinate, 0).rgb;
    }
    uvec3 pixel = uvec3(round(clamp(sampled, 0.0, 1.0) * 255.0));
    if (constants.apply_board_color == 0u) {
        output_color = vec4(vec3(pixel) / 255.0, 1.0);
        return;
    }

    if (constants.super_system22 == 0u) {
        pixel = uvec3(apply_fade(pixel.r, constants.screen_fade.r),
                      apply_fade(pixel.g, constants.screen_fade.g),
                      apply_fade(pixel.b, constants.screen_fade.b));
    }

    const uvec3 corrected = uvec3(
        texelFetch(gamma_data, ivec3(int(pixel.r), 0, 0), 0).r,
        texelFetch(gamma_data, ivec3(int(pixel.g), 0, 1), 0).r,
        texelFetch(gamma_data, ivec3(int(pixel.b), 0, 2), 0).r);
    output_color = vec4(vec3(corrected) / 255.0, 1.0);
}
