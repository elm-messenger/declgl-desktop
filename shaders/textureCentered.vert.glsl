// Built-in `textureCentered` vertex program (GLSL 330 core).
//
// Functionally equivalent to ml-regl-js/src/texture-centered/vert.glsl.
// Used by [centered_texture] / [rect_texture]: a single quad at
// (posize.xy) of size (posize.zw), rotated by `angle`, sampling the
// hardcoded UVs flipped on Y to match WebGL's top-left UV convention.
//
// Public interface (walker binds by name):
//   in       vec2  texc       hardcoded [0,1]² UV from walker
//   uniform  vec4  posize     (cx, cy, w, h)
//   uniform  float angle      rotation in radians
//   uniform  vec2  view       virtual canvas size halved + Y-negated
//   uniform  vec4  camera     (cx, cy, zoom, rot)
// Outputs:
//   out      vec2  vuv        UV passed through (Y-flipped)

#version 330 core

in  vec2  texc;
uniform vec4  posize;
uniform float angle;
uniform vec2  view;
uniform vec4  camera;
out vec2  vuv;

void main() {
    vuv = vec2(texc.x, 1.0 - texc.y);
    vec2 scaled = (texc - 0.5) * posize.zw;
    vec2 rot    = scaled;
    if (angle != 0.0) {
        float c = cos(angle);
        float s = sin(angle);
        rot = vec2(c * scaled.x + s * scaled.y,
                  -s * scaled.x + c * scaled.y);
    }
    vec2 wpos = posize.xy + rot;
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
