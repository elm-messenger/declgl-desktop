// Built-in `fxaa` vertex program (GLSL 330 core).
// Functionally equivalent to ml-regl-js/src/fxaa/vert.glsl.

#version 330 core

out vec2 v_rgbNW;
out vec2 v_rgbNE;
out vec2 v_rgbSW;
out vec2 v_rgbSE;
out vec2 v_rgbM;
out vec2 vUv;

uniform vec2 view;
in vec2 position;

void texcoords(
    vec2 fragCoord,
    vec2 resolution,
    out vec2 rgbNW,
    out vec2 rgbNE,
    out vec2 rgbSW,
    out vec2 rgbSE,
    out vec2 rgbM
) {
    vec2 inverseVP = 1.0 / resolution.xy;
    rgbNW = (fragCoord + vec2(-1.0, -1.0)) * inverseVP;
    rgbNE = (fragCoord + vec2(1.0, -1.0)) * inverseVP;
    rgbSW = (fragCoord + vec2(-1.0, 1.0)) * inverseVP;
    rgbSE = (fragCoord + vec2(1.0, 1.0)) * inverseVP;
    rgbM = vec2(fragCoord * inverseVP);
}

void main() {
    gl_Position = vec4(position, 1.0, 1.0);
    vUv = (position + 1.0) * 0.5;
    vec2 fragCoord = vUv * view;
    texcoords(fragCoord, view, v_rgbNW, v_rgbNE, v_rgbSW, v_rgbSE, v_rgbM);
}

