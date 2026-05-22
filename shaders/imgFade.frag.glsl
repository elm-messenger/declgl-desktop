// Built-in `imgFade` compositor fragment program (GLSL 330 core).
// Functionally equivalent to ml-regl-js/src/imgFade/frag.glsl.

#version 330 core

uniform sampler2D t1;
uniform sampler2D t2;
uniform sampler2D mask;
uniform float t;
uniform int invert_mask;
in vec2 uv;
out vec4 fragColor;

void main() {
    float t0 = texture(mask, uv).x;
    if (invert_mask == 1) {
        t0 = 1.0 - t0;
    }
    t0 = t0 * 0.5 + 0.5;
    float a = smoothstep(-0.5, 0.0, (t - t0));
    fragColor = mix(texture(t1, uv), texture(t2, uv), a);
}

