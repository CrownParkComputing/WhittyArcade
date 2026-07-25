// Screen-filling quad shared by every pass whose fragment stage works from
// gl_FragCoord alone: the solid fills, the System 22 text layer and the
// Model 2 2D layers. Passes that need a UV have their own vertex stage.
#version 450

void main() {
    const vec2 positions[4] = vec2[4](
        vec2(-1.0, -1.0), vec2(1.0, -1.0),
        vec2(-1.0,  1.0), vec2(1.0,  1.0));
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
