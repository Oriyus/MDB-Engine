#version 450 core

// Camera-facing particle quads: 6 vertices per particle from gl_VertexID, the particle from a
// storage buffer (ParticleRenderer)
struct Particle {
    vec4 PositionSize;   // xyz world position, w size
    vec4 Color;          // linear rgb (emission included), alpha
    vec4 Extra;          // x rotation (radians)
};
layout(std430, binding = 2) readonly buffer Particles { Particle u_Particles[]; };

uniform mat4 u_ViewProjection;
uniform vec3 u_CamRight;
uniform vec3 u_CamUp;

out vec2 v_UV;
out vec4 v_Color;

const vec2 kCorners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));

void main() {
    Particle p = u_Particles[gl_InstanceID];
    vec2 c = kCorners[gl_VertexID];
    float s = sin(p.Extra.x), co = cos(p.Extra.x);
    vec2 r = vec2(c.x * co - c.y * s, c.x * s + c.y * co) * (p.PositionSize.w * 0.5);
    vec3 world = p.PositionSize.xyz + u_CamRight * r.x + u_CamUp * r.y;
    v_UV = c * 0.5 + 0.5;
    v_Color = p.Color;
    gl_Position = u_ViewProjection * vec4(world, 1.0);
}
