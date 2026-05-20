// Built-in `circle` vertex program (GLSL 330 core).
//
// Functionally equivalent to ml-regl-js/src/circle/vert.glsl, rewritten
// natively for desktop GL.
//
// This vert is used by both the `circle` program and the `roundedRect`
// program (which pairs it with rounded_rect.frag.glsl). The geometry is
// expected to be a fullscreen NDC quad ([-1,-1],[1,-1],[1,1],[-1,1]).
// The vertex stage projects each NDC corner to its corresponding world-
// space position so the fragment stage can do per-pixel SDF math against
// world-space inputs (`cr` for circle, `cs+radius` for rounded_rect).
//
// Public interface (kept stable, walker binds by name):
//   in       vec2 position   (NDC corner: -1..1)
//   out      vec2 v_position (world-space position)
//   uniform  vec2 view
//   uniform  vec4 camera     (cx, cy, zoom, rot)

#version 330 core

in  vec2 position;
out vec2 v_position;
uniform vec2 view;
uniform vec4 camera;

void main() {
    vec2 tp = position * view / camera.z;

    if (camera.w != 0.0) {
        float c = cos(camera.w);
        float s = sin(camera.w);
        tp = vec2(c * tp.x - s * tp.y,
                  s * tp.x + c * tp.y);
    }

    v_position  = tp + camera.xy;
    gl_Position = vec4(position, 0.0, 1.0);
}
