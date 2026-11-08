#version 450 core

in vec4 v_Color;
out vec4 FragColor;

uniform float u_Time;

void main() {
    // Subtle brightness pulse so you can see uniforms are working
    float pulse = 0.85 + 0.15 * sin(u_Time * 2.0);
    FragColor = vec4(1.0 - v_Color.rgb, v_Color.a);
}