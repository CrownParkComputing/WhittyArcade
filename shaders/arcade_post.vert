// Fullscreen quad for the Vulkan post-process pass, drawn as a four-vertex
// triangle strip with no vertex buffer.
#version 450

layout(location = 0) out vec2 pane_uv;

void main() {
    const vec2 positions[4] = vec2[4](
        vec2(-1.0, -1.0), vec2(1.0, -1.0),
        vec2(-1.0,  1.0), vec2(1.0,  1.0));
    const vec2 position = positions[gl_VertexIndex];
    gl_Position = vec4(position, 0.0, 1.0);
    // The OpenGL post shader derives its sampling coordinate from
    // gl_FragCoord, whose origin is the bottom-left of the framebuffer, and
    // its callers set flip_y against that convention. Vulkan puts +Y down in
    // clip space and gl_FragCoord's origin at the top-left, so hand the
    // fragment stage an explicit bottom-left-origin coordinate instead. Every
    // flip_y decision the boards already make then carries over unchanged.
    pane_uv = vec2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
}
