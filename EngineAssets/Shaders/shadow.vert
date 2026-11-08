#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 2) in vec2 a_TexCoord;    // for alpha-tested shadows
layout(location = 3) in ivec4 a_Bones;      // skinned meshes only
layout(location = 4) in vec4 a_Weights;

// The same per-object data as lit.vert (DrawList)
struct Instance {
    mat4  Model;
    mat4  Normal;
    vec4  Color;
    vec4  Surface;
    vec4  Emissive;
    ivec4 Info;
};
layout(std430, binding = 0) readonly buffer Instances { Instance u_Instances[]; };
layout(std430, binding = 1) readonly buffer InstanceIndices { uint u_Indices[]; };   // per view: which instances
uniform int u_InstanceBase;

uniform mat4 u_LightVP;

const int MAX_BONES = 128;
uniform int u_Skinned;
uniform mat4 u_Bones[MAX_BONES];

out vec3 v_WorldPos;
out vec2 v_TexCoord;
flat out float v_Alpha;

void main() {
    Instance inst = u_Instances[u_Indices[u_InstanceBase + gl_InstanceID]];
    vec4 pos = vec4(a_Position, 1.0);
    if (u_Skinned == 1) {
        mat4 skin = a_Weights.x * u_Bones[a_Bones.x] + a_Weights.y * u_Bones[a_Bones.y]
                  + a_Weights.z * u_Bones[a_Bones.z] + a_Weights.w * u_Bones[a_Bones.w];
        pos = skin * pos;
    }
    vec4 world = inst.Model * pos;
    v_WorldPos = world.xyz;
    v_TexCoord = a_TexCoord;
    v_Alpha = inst.Color.a;
    gl_Position = u_LightVP * world;
}
