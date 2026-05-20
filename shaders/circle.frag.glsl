// Built-in `circle` fragment program (GLSL 330 core).
//
// Public interface:
//   in      vec2 v_position  world-space pixel position
//   uniform vec3 cr          (cx, cy, radius)
//   uniform vec4 color       straight RGBA

#version 330 core

in  vec2 v_position;
uniform vec4 color;
uniform vec3 cr;
out vec4 fragColor;

void main() {
    float d = distance(v_position, cr.xy);
    if (d > cr.z + 1.0) discard;

    float alpha = 1.0 - smoothstep(cr.z - 1.0, cr.z + 1.0, d);
    fragColor = vec4(color.rgb * color.a * alpha, alpha * color.a);
}
