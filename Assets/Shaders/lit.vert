#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;
layout(location = 3) in ivec4 a_Bones;     // skinned meshes only
layout(location = 4) in vec4 a_Weights;

uniform mat4 u_ViewProjection;
uniform mat4 u_Model;

// Skeletal animation: when u_Skinned is 1, each vertex is moved by up to four bone matrices
const int MAX_BONES = 128;
uniform int u_Skinned;
uniform mat4 u_Bones[MAX_BONES];

out vec3 v_WorldPos;
out vec3 v_Normal;
out vec2 v_TexCoord;

void main() {
    mat4 skin = mat4(1.0);
    if (u_Skinned == 1) {
        skin = a_Weights.x * u_Bones[a_Bones.x] + a_Weights.y * u_Bones[a_Bones.y]
             + a_Weights.z * u_Bones[a_Bones.z] + a_Weights.w * u_Bones[a_Bones.w];
    }
    mat4 model = u_Model * skin;
    vec4 world = model * vec4(a_Position, 1.0);
    v_WorldPos = world.xyz;
    // The normal matrix keeps normals correct under non-uniform scaling.
    // (inverse() per vertex is wasteful; we'll move this to the CPU later.)
    v_Normal = mat3(transpose(inverse(model))) * a_Normal;
    v_TexCoord = a_TexCoord;
    gl_Position = u_ViewProjection * world;
}
