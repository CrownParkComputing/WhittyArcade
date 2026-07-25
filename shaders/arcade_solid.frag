#version 450

layout(location = 0) out vec4 output_color;

layout(push_constant) uniform solid_constants {
    vec4 color;
} constants;

void main() {
    output_color = constants.color;
}
