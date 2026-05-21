// Built-in `texture` fragment program (GLSL 330 core).
//
// Functionally equivalent to ml-regl-js/src/texture/frag.glsl —
// samples the bound texture at `uv`, multiplies by the per-call
// `alpha`, and emits premultiplied RGBA so it composites correctly
// with the engine's GL_ONE / GL_ONE_MINUS_SRC_ALPHA blending.
//
// Public interface:
//   uniform sampler2D tex         bound on TEXUNIT0 by the walker
//                                  (the OCaml field key "texture" is
//                                   remapped here because GLSL 330
//                                   shadows the [texture()] sampling
//                                   function with a uniform named
//                                   "texture")
//   uniform float     alpha       defaults to 1.0
//   in      vec2      uv          from vertex shader

#version 330 core

uniform sampler2D tex;
uniform float     alpha;
in  vec2  uv;
out vec4  fragColor;

void main() {
    vec4 sampled = texture(tex, uv) * alpha;
    fragColor = vec4(sampled.rgb * sampled.a, sampled.a);
}
