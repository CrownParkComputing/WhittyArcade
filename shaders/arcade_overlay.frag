// Vulkan port of settings_fragment_shader_source. The overlay textures carry
// their own alpha and the pipeline blends with it, matching the GL path's
// GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA state.
#version 450

layout(location = 0) in vec2 texture_coordinate;
layout(location = 0) out vec4 output_color;

layout(set = 0, binding = 0) uniform sampler2D overlay_texture;

void main() {
    output_color = texture(overlay_texture, texture_coordinate);
}
