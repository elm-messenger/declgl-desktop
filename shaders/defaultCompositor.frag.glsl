// Built-in `defaultCompositor` fragment program (GLSL 330 core).
//
// Functionally equivalent to ml-regl-js/src/compositors/frag.glsl.
// Two modes selected by the `mode` int uniform:
//   mode = 0  →  destination over source  (dst + src*(1-dst.a))
//   mode = 1  →  mask by source           (dst * src.a)
// Both inputs are FBO color textures; bound by the walker on TEXUNIT0
// and TEXUNIT1 from the [composite] left/right children.

#version 330 core

uniform sampler2D t1;
uniform sampler2D t2;
uniform int       mode;
in  vec2  uv;
out vec4  fragColor;

void main() {
    vec4 src = texture(t1, uv);
    vec4 dst = texture(t2, uv);
    if (mode == 0) {
        fragColor = dst + src * (1.0 - dst.a);
    } else {
        fragColor = dst * src.a;
    }
}
