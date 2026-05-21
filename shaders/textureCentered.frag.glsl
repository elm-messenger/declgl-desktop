// Built-in `textureCentered` fragment program (GLSL 330 core).
//
// Functionally equivalent to ml-regl-js/src/texture-centered/frag.glsl.
// Same sampling as `texture`, but reads the Y-flipped `vuv` produced by
// the centered vertex shader.
//
// Public interface:
//   uniform sampler2D tex         bound on TEXUNIT0 by the walker
//                                  (key "texture" is remapped — see
//                                   texture.frag.glsl note)
//   uniform float     alpha       defaults to 1.0
//   in      vec2      vuv

#version 330 core

uniform sampler2D tex;
uniform float     alpha;
in  vec2  vuv;
out vec4  fragColor;

void main() {
    vec4 sampled = texture(tex, vuv) * alpha;
    fragColor = vec4(sampled.rgb * sampled.a, sampled.a);
}
