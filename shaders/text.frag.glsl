// Built-in `textbox` fragment program (GLSL 330 core).
//
// Functionally equivalent to ml-regl-js/src/text/frag.glsl. Samples
// the bound MSDF atlas, computes a smooth signed-distance derivative
// using `fwidth` (always available in GL 3.3 core; no #extension
// dance like in GLES2), and emits premultiplied RGBA matching the
// engine-wide GL_ONE / GL_ONE_MINUS_SRC_ALPHA blending mode.
//
// Uniforms:
//   tMap       = MSDF atlas, bound on TEXUNIT0 by the walker
//   thickness  = boldness offset (positive = bolder, negative = thinner)
//   color      = RGBA tint, straight-alpha; we premultiply at end
//   unitRange  = (range/scaleW, range/scaleH) from the BMFont JSON's
//                distanceField.distanceRange ÷ atlas dimensions

#version 330 core

uniform sampler2D tMap;
uniform float     thickness;
uniform vec4      color;
uniform vec2      unitRange;

in  vec2 vUv;
out vec4 fragColor;

float screenPxRange() {
    vec2 screenTexSize = vec2(1.0) / fwidth(vUv);
    return max(0.5 * dot(unitRange, screenTexSize), 1.0);
}

void main() {
    vec4 nc = vec4(color.rgb * color.a, color.a);
    vec3 tex = texture(tMap, vUv).rgb;
    float d = max(min(tex.r, tex.g), min(max(tex.r, tex.g), tex.b)) - 0.5;
    float bodyDist = screenPxRange() * d;
    float alpha = clamp(bodyDist + 0.5 + thickness, 0.0, 1.0);
    if (alpha < 0.01) discard;
    fragColor = nc * alpha;
}
