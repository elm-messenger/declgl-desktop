// Built-in `gblurh` / `gblurv` effect fragment program (GLSL 330 core).
// Functionally equivalent to ml-regl-js/src/gblur/frag.glsl.

#version 330 core

uniform sampler2D tex;
uniform vec2 view;
uniform vec2 dir;
uniform float radius;
in vec2 uv;
out vec4 fragColor;

void main() {
    vec4 sum = vec4(0.0);

    float blurx = radius / (2.0 * view.x);
    float blury = radius / (2.0 * -view.y);
    float hstep = dir.x;
    float vstep = dir.y;

    sum += texture(tex, vec2(uv.x - 4.0 * blurx * hstep, uv.y - 4.0 * blury * vstep)) * 0.0162162162;
    sum += texture(tex, vec2(uv.x - 3.0 * blurx * hstep, uv.y - 3.0 * blury * vstep)) * 0.0540540541;
    sum += texture(tex, vec2(uv.x - 2.0 * blurx * hstep, uv.y - 2.0 * blury * vstep)) * 0.1216216216;
    sum += texture(tex, vec2(uv.x - 1.0 * blurx * hstep, uv.y - 1.0 * blury * vstep)) * 0.1945945946;
    sum += texture(tex, vec2(uv.x, uv.y)) * 0.2270270270;
    sum += texture(tex, vec2(uv.x + 1.0 * blurx * hstep, uv.y + 1.0 * blury * vstep)) * 0.1945945946;
    sum += texture(tex, vec2(uv.x + 2.0 * blurx * hstep, uv.y + 2.0 * blury * vstep)) * 0.1216216216;
    sum += texture(tex, vec2(uv.x + 3.0 * blurx * hstep, uv.y + 3.0 * blury * vstep)) * 0.0540540541;
    sum += texture(tex, vec2(uv.x + 4.0 * blurx * hstep, uv.y + 4.0 * blury * vstep)) * 0.0162162162;

    if (sum.a < 0.01) discard;
    fragColor = sum;
}

