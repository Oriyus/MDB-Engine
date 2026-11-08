#version 450 core

uniform mat4 u_InvViewProj;
uniform vec3 u_CamPos;
out vec3 v_Dir;

void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vec2 ndc = p * 2.0 - 1.0;
    vec4 w = u_InvViewProj * vec4(ndc, 1.0, 1.0);
    v_Dir = w.xyz / w.w - u_CamPos;
    gl_Position = vec4(ndc, 1.0, 1.0);          // z = w: depth exactly 1.0, behind everything
}