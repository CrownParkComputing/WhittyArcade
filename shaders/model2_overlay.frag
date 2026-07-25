// Vulkan port of model2_overlay_fragment_shader_source: the Model 2 2D layer
// composited over the polygon scene.
#version 450
layout(set = 0, binding = 0) uniform sampler2D layer_texture;
layout(location = 0) out vec4 output_color;
void main() {
    // Vulkan's fragment origin is already the top-left, so the OpenGL
    // version's row flip is not wanted here.
    ivec2 coordinate = ivec2(int(gl_FragCoord.x), int(gl_FragCoord.y));
    output_color = texelFetch(layer_texture, coordinate, 0);
}
