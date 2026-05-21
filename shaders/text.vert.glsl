// Built-in `textbox` vertex program (GLSL 330 core).
//
// Functionally equivalent to ml-regl-js/src/text/vert.glsl. Per-vertex
// `position` is in textbox-local space (origin at the textbox `offset`
// point, +x right, +y up). The walker computes positions from the JS
// layout algorithm in [renderable_walker.cc] and uploads them as a
// dynamic VBO each frame.
//
// Uniforms:
//   view    = (virtWidth/2, -virtHeight/2)  — same convention as every
//             other 2D builtin
//   camera  = (cx, cy, zoom, rot)
//   offset  = textbox top-left in world space (x, y)
//
// Attributes:
//   position = per-vertex local-space corner
//   uv       = MSDF atlas UV for that corner

#version 330 core

in  vec2 position;
in  vec2 uv;
uniform vec2 view;
uniform vec2 offset;
uniform vec4 camera;

out vec2 vUv;

void main() {
    vUv = uv;
    vec2 wpos = position + offset;
    if (camera.w == 0.0) {
        vec2 p = (wpos - camera.xy) * camera.z / view;
        gl_Position = vec4(p, 0.0, 1.0);
    } else {
        vec2 diff = wpos - camera.xy;
        float c = cos(camera.w);
        float s = sin(camera.w);
        vec2 rotated = vec2(c * diff.x + s * diff.y,
                           -s * diff.x + c * diff.y);
        vec2 p = rotated * camera.z / view;
        gl_Position = vec4(p, 0.0, 1.0);
    }
}
