// Built-in `crt` effect fragment program (GLSL 330 core).
// Functionally equivalent to ml-regl-js/src/crt/frag.glsl.

#version 330 core

uniform float scanline_count;
uniform sampler2D tex;
uniform vec2 view;
in vec2 uv;
out vec4 fragColor;

vec2 uv_curve(vec2 p) {
    p = (p - 0.5) * 2.0;
    p.x *= 1.0 + pow(abs(p.y) / 3.0, 2.0);
    p.y *= 1.0 + pow(abs(p.x) / 3.0, 2.0);
    p /= 1.2;
    p = (p / 2.0) + 0.5;
    return p;
}

void main() {
    const float PI = 3.14159;
    float r = texture(tex, uv_curve(uv) + vec2(0.0 / view.x), 0.0).r;
    float g = texture(tex, uv_curve(uv) + vec2(3.0 / view.x), 0.0).g;
    float b = texture(tex, uv_curve(uv) + vec2(-3.0 / view.x), 0.0).b;

    float s = sin(uv_curve(uv).y * scanline_count * PI * 2.0);
    s = (s * 0.5 + 0.5) * 0.9 + 0.1;
    vec4 scan_line = vec4(vec3(pow(s, 0.25)), 1.0);

    fragColor = vec4(r, g, b, 1.0) * scan_line;
}

