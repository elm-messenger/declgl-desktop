// Built-in `triangle` fragment program (GLSL 330 core).
//
// Public interface (kept stable, walker binds by name):
//   uniform vec4 color   // straight RGBA; we premultiply alpha at output

#version 330 core

uniform vec4 color;
out vec4 fragColor;

void main() {
    fragColor = vec4(color.rgb * color.a, color.a);
}
