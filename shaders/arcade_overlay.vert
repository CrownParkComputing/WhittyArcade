// Vulkan port of settings_vertex_shader_source: a viewport-filling quad for
// blitting an RGBA overlay (FPS badge, cabinet status, player label, settings
// and ROM menus) over the presented frame.
#version 450

layout(location = 0) out vec2 texture_coordinate;

void main() {
    const vec2 positions[4] = vec2[4](
        vec2(-1.0, -1.0), vec2(1.0, -1.0),
        vec2(-1.0,  1.0), vec2(1.0,  1.0));
    const vec2 position = positions[gl_VertexIndex];
    gl_Position = vec4(position, 0.0, 1.0);
    // The GL shader pairs position -1 (bottom of the viewport under GL's
    // +Y-up clip space) with V = 1. Vulkan's clip space is +Y down, so
    // position -1 is the top of the viewport and the same texel belongs at
    // V = 0. Deriving V from the position keeps one source of truth.
    texture_coordinate = vec2(position.x * 0.5 + 0.5, position.y * 0.5 + 0.5);
}
