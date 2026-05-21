// Built-in `effect` vertex program (GLSL 330 core).
//
// Functionally equivalent to ml-regl-js/src/effect/vert.glsl. Used by
// every effect and compositor program: takes a `texc` attribute that
// already maps the unit-quad corner to its UV (caller-supplied,
// hardcoded by the walker as [1,1, 1,0, 0,0, 0,1]) and projects it
// directly to NDC via `texc * 2 - 1`. No camera, no view, no posize —
// these always cover the full viewport.

#version 330 core

in  vec2 texc;
out vec2 uv;

void main() {
    uv = texc;
    gl_Position = vec4(texc * 2.0 - 1.0, 0.0, 1.0);
}
