#version 450 core

in vec3 v_WorldPos;

uniform vec3  u_LightPos;
uniform float u_FarPlane;

void main() {
    gl_FragDepth = length(v_WorldPos - u_LightPos) / u_FarPlane;
}