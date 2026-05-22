// Built-in `blurv` effect fragment program (GLSL 330 core).
// Functionally equivalent to ml-regl-js/src/blur/frag2.glsl.

#version 330 core

#define BLUR_RADIUS 3
#define KERNEL_SIZE (2 * BLUR_RADIUS + 1)

uniform sampler2D tex;
uniform float radius;
uniform vec2 view;
in vec2 uv;
out vec4 fragColor;

void main() {
    if (radius < 0.1) {
        fragColor = texture(tex, uv);
        return;
    }

    vec4 avg = vec4(0.0);
    for (int i = -BLUR_RADIUS; i <= BLUR_RADIUS; i++) {
        vec2 offset = vec2(0.0, float(i) * radius / (2.0 * -view.y));
        avg += texture(tex, uv + offset) / float(KERNEL_SIZE);
    }
    if (avg.a < 0.01) {
        discard;
    }
    fragColor = avg;
}

