// Built-in `compFade` fragment program (GLSL 330 core).
//
// Linear cross-fade between two FBOs by `t` ∈ [0,1].
// Functionally equivalent to ml-regl-js/src/compFade/frag.glsl.

#version 330 core

uniform sampler2D t1;
uniform sampler2D t2;
uniform float     t;
uniform int       mode;
in  vec2  uv;
out vec4  fragColor;

void main() {
    if (mode == 0) {
        fragColor = mix(texture(t1, uv), texture(t2, uv), t);
    } else {
        fragColor = texture(t1, uv);
    }
}
