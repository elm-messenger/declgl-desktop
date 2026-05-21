// Built-in `colormult` effect fragment program (GLSL 330 core).
//
// Multiplies the source FBO's RGBA by a uniform `color` (vec4).
// Functionally equivalent to ml-regl-js/src/colormult/frag.glsl.

#version 330 core

uniform sampler2D tex;
uniform vec4      color;
in  vec2  uv;
out vec4  fragColor;

void main() {
    fragColor = texture(tex, uv) * color;
}
