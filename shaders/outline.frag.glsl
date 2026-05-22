// Built-in `outline` effect fragment program (GLSL 330 core).
// Functionally equivalent to ml-regl-js/src/outline/frag.glsl.

#version 330 core

uniform sampler2D tex;
uniform float outline;
uniform vec2 view;
uniform vec4 color;
in vec2 uv;
out vec4 fragColor;

void main() {
    float alpha = texture(tex, uv).a;
    float maxAlpha = alpha;

    if (alpha != 0.0) {
        fragColor = texture(tex, uv);
        return;
    }

    for (float i = 1.0; i <= 10.0; i++) {
        if (i > outline) {
            break;
        }
        for (float xo = -1.0; xo < 1.5; xo += 1.0) {
            for (float yo = -1.0; yo < 1.5; yo += 1.0) {
                vec2 pos = vec2(uv.x + xo * i * (0.5 / view.x),
                                uv.y + yo * i * (-0.5 / view.y));
                maxAlpha = max(maxAlpha, texture(tex, pos).a);
            }
        }
    }
    if (alpha == 0.0 && maxAlpha > 0.0) {
        fragColor = color;
    } else {
        fragColor = texture(tex, uv);
    }
}

