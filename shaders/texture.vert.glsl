// Built-in `texture` vertex program (GLSL 330 core).
//
// Functionally equivalent to ml-regl-js/src/texture/vert.glsl,
// rewritten natively for desktop GL. Used by the OCaml [texture] /
// [texture_cropped] builtin draws.
//
// The OCaml builders ship a 4-vertex `pos` (one corner per row) plus
// either a hardcoded `texc` (for the `texture` atomic) or a per-call
// `texc` (for `textureCropped`). Both flow into the `texc` attribute
// here unchanged.
//
// Public interface (walker binds by name):
//   in       vec2 position    world-space corner
//   in       vec2 texc        UV coord matching that corner
//   uniform  vec2 view        virtual canvas size halved + Y-negated
//   uniform  vec4 camera      (cx, cy, zoom, rot)
// Outputs:
//   out      vec2 uv          passed to the fragment shader

#version 330 core

in  vec2 position;
in  vec2 texc;
uniform vec2 view;
uniform vec4 camera;
out vec2 uv;

void main() {
    uv = texc;
    vec2 diff = position - camera.xy;
    if (camera.w != 0.0) {
        float c = cos(camera.w);
        float s = sin(camera.w);
        diff = vec2(c * diff.x + s * diff.y,
                   -s * diff.x + c * diff.y);
    }
    vec2 pos = diff * camera.z / view;
    gl_Position = vec4(pos, 0.0, 1.0);
}
