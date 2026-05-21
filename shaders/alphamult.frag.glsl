// Built-in `alphamult` effect fragment program (GLSL 330 core).
//
// Multiplies the source FBO's RGBA by a uniform `alpha`. The OCaml
// `Regl_effects.alpha_mult` builder ships [num "alpha" a] only — the
// walker also auto-binds the input FBO's color texture to `tex`
// (the JS-side uniform was named `texture`, renamed here to avoid
// the GLSL 330 sampler-name shadowing).

#version 330 core

uniform sampler2D tex;
uniform float     alpha;
in  vec2  uv;
out vec4  fragColor;

void main() {
    fragColor = texture(tex, uv) * alpha;
}
