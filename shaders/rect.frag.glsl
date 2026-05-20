// Built-in `rect` fragment program (GLSL 330 core).
//
// Public interface:
//   uniform vec4 color    straight RGBA, premultiplied at output

#version 330 core

uniform vec4 color;
out vec4 fragColor;

void main() {
    fragColor = vec4(color.rgb * color.a, color.a);
}
