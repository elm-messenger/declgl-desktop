// Built-in `pixilation` effect fragment program (GLSL 330 core).
// Functionally equivalent to ml-regl-js/src/pixilation/frag.glsl.

#version 330 core

uniform sampler2D tex;
uniform vec2 view;
uniform float pixelSize;
in vec2 uv;
out vec4 fragColor;

void main() {
    vec2 res = vec2(view.x * 2.0, -view.y * 2.0);
    vec2 puv = floor(uv * res / pixelSize) * pixelSize / res;
    fragColor = texture(tex, puv);
}

