// Built-in `roundedRect` fragment program (GLSL 330 core).
//
// Pairs with circle.vert.glsl (NDC fullscreen quad → world-space
// `v_position`). The four corners are AA'd against a per-corner SDF
// circle of radius `radius`; everything else inside the box is solid.
//
// Public interface:
//   in      vec2  v_position  world-space pixel position
//   uniform vec4  color       straight RGBA
//   uniform vec4  cs          (cx, cy, w, h)  centre + size
//   uniform float radius      corner radius

#version 330 core

in  vec2 v_position;
uniform vec4  color;
uniform vec4  cs;
uniform float radius;
out vec4 fragColor;

void main() {
    vec4 nc = vec4(color.rgb * color.a, color.a);
    float hw = cs.z * 0.5;
    float hh = cs.w * 0.5;

    if (abs(v_position.x - cs.x) > hw) discard;
    if (abs(v_position.y - cs.y) > hh) discard;

    // Top-left corner.
    vec2 lt = vec2(cs.x - hw + radius, cs.y - hh + radius);
    if (v_position.x < lt.x && v_position.y < lt.y) {
        float d = distance(v_position, lt);
        if (d > radius + 1.0) discard;
        float a = 1.0 - smoothstep(radius - 1.0, radius + 1.0, d);
        fragColor = nc * a;
        return;
    }

    // Top-right corner.
    vec2 rt = vec2(cs.x + hw - radius, cs.y - hh + radius);
    if (v_position.x > rt.x && v_position.y < rt.y) {
        float d = distance(v_position, rt);
        if (d > radius + 1.0) discard;
        float a = 1.0 - smoothstep(radius - 1.0, radius + 1.0, d);
        fragColor = nc * a;
        return;
    }

    // Bottom-left corner.
    vec2 lb = vec2(cs.x - hw + radius, cs.y + hh - radius);
    if (v_position.x < lb.x && v_position.y > lb.y) {
        float d = distance(v_position, lb);
        if (d > radius + 1.0) discard;
        float a = 1.0 - smoothstep(radius - 1.0, radius + 1.0, d);
        fragColor = nc * a;
        return;
    }

    // Bottom-right corner.
    vec2 rb = vec2(cs.x + hw - radius, cs.y + hh - radius);
    if (v_position.x > rb.x && v_position.y > rb.y) {
        float d = distance(v_position, rb);
        if (d > radius + 1.0) discard;
        float a = 1.0 - smoothstep(radius - 1.0, radius + 1.0, d);
        fragColor = nc * a;
        return;
    }

    // Interior.
    fragColor = nc;
}
