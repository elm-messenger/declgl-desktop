// Built-in `fxaa` effect fragment program (GLSL 330 core).
// Functionally equivalent to ml-regl-js/src/fxaa/frag.glsl.

#version 330 core

in vec2 v_rgbNW;
in vec2 v_rgbNE;
in vec2 v_rgbSW;
in vec2 v_rgbSE;
in vec2 v_rgbM;
in vec2 vUv;

uniform vec2 view;
uniform sampler2D tex;
out vec4 fragColor;

#define FXAA_REDUCE_MIN   (1.0 / 128.0)
#define FXAA_REDUCE_MUL   (1.0 / 8.0)
#define FXAA_SPAN_MAX     8.0

vec4 fxaa(
    sampler2D tex0,
    vec2 fragCoord,
    vec2 resolution,
    vec2 rgbNWCoord,
    vec2 rgbNECoord,
    vec2 rgbSWCoord,
    vec2 rgbSECoord,
    vec2 rgbMCoord
) {
    vec2 inverseVP = vec2(1.0 / resolution.x, 1.0 / resolution.y);
    vec3 rgbNW = texture(tex0, rgbNWCoord).xyz;
    vec3 rgbNE = texture(tex0, rgbNECoord).xyz;
    vec3 rgbSW = texture(tex0, rgbSWCoord).xyz;
    vec3 rgbSE = texture(tex0, rgbSECoord).xyz;
    vec4 texColor = texture(tex0, rgbMCoord);
    vec3 rgbM = texColor.xyz;
    vec3 luma = vec3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, luma);
    float lumaNE = dot(rgbNE, luma);
    float lumaSW = dot(rgbSW, luma);
    float lumaSE = dot(rgbSE, luma);
    float lumaM = dot(rgbM, luma);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y = ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) *
        (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);

    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = min(vec2(FXAA_SPAN_MAX, FXAA_SPAN_MAX),
              max(vec2(-FXAA_SPAN_MAX, -FXAA_SPAN_MAX), dir * rcpDirMin)) * inverseVP;

    vec3 rgbA = 0.5 * (texture(tex0, fragCoord * inverseVP + dir * (1.0 / 3.0 - 0.5)).xyz +
        texture(tex0, fragCoord * inverseVP + dir * (2.0 / 3.0 - 0.5)).xyz);
    vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(tex0, fragCoord * inverseVP + dir * -0.5).xyz +
        texture(tex0, fragCoord * inverseVP + dir * 0.5).xyz);

    float lumaB = dot(rgbB, luma);
    if ((lumaB < lumaMin) || (lumaB > lumaMax))
        return vec4(rgbA, texColor.a);
    return vec4(rgbB, texColor.a);
}

void main() {
    vec2 fragCoord = vUv * view;
    fragColor = fxaa(tex, fragCoord, view, v_rgbNW, v_rgbNE, v_rgbSW, v_rgbSE, v_rgbM);
}

