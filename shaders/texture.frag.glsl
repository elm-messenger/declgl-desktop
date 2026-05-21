// Built-in `texture` fragment program (GLSL 330 core).
//
// Functionally equivalent to ml-regl-js/src/texture/frag.glsl —
// samples the bound texture at `uv`, multiplies by the per-call
// `alpha`, and emits the result. Composites correctly with the
// engine's GL_ONE / GL_ONE_MINUS_SRC_ALPHA blending because the
// underlying texture is uploaded in premultiplied form by
// [resources/texture.cc] (see its [upload_rgba8] doc comment for
// the rationale and the `no_premultiply_alpha` opt-out path).
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
    // Texels are already premultiplied at upload time; scaling by a
    // scalar [alpha] preserves the premultiplied invariant
    // ((R*A, G*A, B*A, A) * a' = (R*A*a', G*A*a', B*A*a', A*a')).
    fragColor = texture(tex, uv) * alpha;
}
