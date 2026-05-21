// Built-in `textureCroppedCentered` vertex program (GLSL 330 core).
//
// Functionally equivalent to ml-regl-js/src/texture-cropped-centered/vert.glsl.
// Used by [centered_texture_cropped]: rotated quad at posize.xy of
// posize.zw size, with caller-supplied `texc` defining the source UV
// rectangle (4 corners, expanded from the OCaml's 4-float (cx,cy,cw,ch)
// before being shipped — done by the walker since we don't have the JS
// preprocessor closure).
//
// Public interface (walker binds by name):
//   in       vec2  texc       caller-supplied per-corner UVs
//   in       vec2  texc2      hardcoded [-0.5,0.5]² unit-quad corner
//   uniform  vec4  posize     (cx, cy, w, h)
//   uniform  float angle
//   uniform  vec2  view
//   uniform  vec4  camera

#version 330 core

in  vec2  texc;
in  vec2  texc2;
uniform vec4  posize;
uniform float angle;
uniform vec2  view;
uniform vec4  camera;
out vec2  vuv;

void main() {
    vuv = texc;
    vec2 scaled = texc2 * posize.zw;
    float c = cos(angle);
    float s = sin(angle);
    vec2 rot = vec2(c * scaled.x + s * scaled.y,
                   -s * scaled.x + c * scaled.y);
    vec2 wpos = posize.xy + rot;
    vec2 diff = wpos - camera.xy;
    if (camera.w != 0.0) {
        float cw = cos(camera.w);
        float sw = sin(camera.w);
        diff = vec2(cw * diff.x + sw * diff.y,
                   -sw * diff.x + cw * diff.y);
    }
    vec2 pos = diff * camera.z / view;
    gl_Position = vec4(pos, 0.0, 1.0);
}
