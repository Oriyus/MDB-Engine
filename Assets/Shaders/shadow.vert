#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 2) in vec2 a_TexCoord;    // for alpha-tested shadows
layout(location = 3) in ivec4 a_Bones;     // skinned meshes only
layout(location = 4) in vec4 a_Weights;

uniform mat4 u_LightVP;
uniform mat4 u_Model;

const int MAX_BONES = 128;
uniform int u_Skinned;
uniform mat4 u_Bones[MAX_BONES];

out vec3 v_WorldPos;
out vec2 v_TexCoord;

void main() {
    mat4 skin = mat4(1.0);
    if (u_Skinned == 1) {
        skin = a_Weights.x * u_Bones[a_Bones.x] + a_Weights.y * u_Bones[a_Bones.y]
             + a_Weights.z * u_Bones[a_Bones.z] + a_Weights.w * u_Bones[a_Bones.w];
    }
    vec4 world = u_Model * skin * vec4(a_Position, 1.0);
    v_WorldPos = world.xyz;
    v_TexCoord = a_TexCoord;
    gl_Position = u_LightVP * world;
}
