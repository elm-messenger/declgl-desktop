// Built-in `rect` vertex program (GLSL 330 core).
//
// Functionally equivalent to ml-regl-js/src/rect/vert.glsl, rewritten
// natively for desktop GL. Used by the OCaml [rect] / [rect_centered]
// builtin draws.
//
// Public interface (kept stable, walker binds by name):
//   in       vec2 position    (NDC-ish unit-quad corner: 0..1)
//   uniform  vec4 posize      (cx, cy, w, h)   centre + size
//   uniform  float angle      rotation in radians
//   uniform  vec2 view        virtual canvas size
//   uniform  vec4 camera      (cx, cy, zoom, rot)

#version 330 core

in  vec2  position;
uniform vec4  posize;
uniform float angle;
uniform vec2  view;
uniform vec4  camera;

void main() {
    vec2 scaled = (position - 0.5) * posize.zw;

    if (angle != 0.0) {
        float c = cos(angle);
        float s = sin(angle);
        scaled = vec2(c * scaled.x + s * scaled.y,
                     -s * scaled.x + c * scaled.y);
    }

    vec2 wpos = posize.xy + scaled;
    vec2 diff = wpos - camera.xy;

    if (camera.w != 0.0) {
        float c = cos(camera.w);
        float s = sin(camera.w);
        diff = vec2(c * diff.x + s * diff.y,
                   -s * diff.x + c * diff.y);
    }

    vec2 pos = diff * camera.z / view;
    gl_Position = vec4(pos, 0.0, 1.0);
}
