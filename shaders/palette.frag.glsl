// Built-in `palette` fragment program (GLSL 330 core).
//
// Functionally equivalent to ml-regl-js/src/palette/frag.glsl: a
// passthrough of the bound FBO color texture. Used to flush an
// offscreen palette into a parent palette (or the system framebuffer).
//
// The OCaml field key for the source FBO sampler is "tex" (the walker
// substitutes when binding palette draws); the uniform here is
// renamed away from `texture` for the same reason as the texture/
// frag shaders — GLSL 330 unifies sampling under [texture()] and a
// uniform with that name shadows the function.

#version 330 core

uniform sampler2D tex;
in  vec2 uv;
out vec4 fragColor;

void main() {
    fragColor = texture(tex, uv);
}
