#version 450 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;
layout(location = 3) in ivec4 a_Bones;     // skinned meshes only
layout(location = 4) in vec4 a_Weights;

// Per-object data (DrawList): one entry per instance, so one draw call can place many objects
struct Instance {
    mat4  Model;
    mat4  Normal;      // transpose(inverse(mat3(Model)))
    vec4  Color;       // rgb sRGB-style, a = alpha
    vec4  Surface;     // x metallic, y roughness
    vec4  Emissive;    // rgb linear
    ivec4 Info;        // x entity ID, y unlit
};
layout(std430, binding = 0) readonly buffer Instances { Instance u_Instances[]; };
uniform int u_InstanceBase;          // the batch's first instance

uniform mat4 u_ViewProjection;

// Skeletal animation: when u_Skinned is 1, each vertex is moved by up to four bone matrices
const int MAX_BONES = 128;
uniform int u_Skinned;
uniform mat4 u_Bones[MAX_BONES];

out vec3 v_WorldPos;
out vec3 v_Normal;
out vec2 v_TexCoord;
flat out vec4  v_Color;
flat out vec2  v_Surface;
flat out vec3  v_Emissive;
flat out int   v_EntityID;
flat out int   v_Unlit;

void main() {
    Instance inst = u_Instances[u_InstanceBase + gl_InstanceID];

    vec4 pos = vec4(a_Position, 1.0);
    vec3 nrm = a_Normal;
    if (u_Skinned == 1) {
        mat4 skin = a_Weights.x * u_Bones[a_Bones.x] + a_Weights.y * u_Bones[a_Bones.y]
                  + a_Weights.z * u_Bones[a_Bones.z] + a_Weights.w * u_Bones[a_Bones.w];
        pos = skin * pos;
        nrm = mat3(skin) * nrm;           // bones carry no non-uniform scale in practice
    }
    vec4 world = inst.Model * pos;
    v_WorldPos = world.xyz;
    v_Normal = mat3(inst.Normal) * nrm;   // correct under non-uniform scaling
    v_TexCoord = a_TexCoord;
    v_Color = inst.Color;
    v_Surface = inst.Surface.xy;
    v_Emissive = inst.Emissive.rgb;
    v_EntityID = inst.Info.x;
    v_Unlit = inst.Info.y;
    gl_Position = u_ViewProjection * world;
}
