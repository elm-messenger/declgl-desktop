// Built-in `triangle` vertex program (GLSL 330 core).
//
// Functionally equivalent to ml-regl-js/src/triangle/vert.glsl, but written
// natively for desktop GL instead of through a WebGL1 compat shim.
//
// Public interface (kept stable, walker binds by name):
//   attribute  vec2 position
//   uniform    vec2 view     // virtual canvas size in pixels
//   uniform    vec4 camera   // (cx, cy, zoom, rot)

#version 330 core

in  vec2 position;
uniform vec2 view;
uniform vec4 camera;

void main() {
    vec2 diff = position - camera.xy;

    // Skip the rotation when rot == 0 (matches the JS branch).
    if (camera.w != 0.0) {
        float c = cos(camera.w);
        float s = sin(camera.w);
        diff = vec2(c * diff.x + s * diff.y,
                   -s * diff.x + c * diff.y);
    }

    vec2 pos = diff * camera.z / view;
    gl_Position = vec4(pos, 0.0, 1.0);
}
