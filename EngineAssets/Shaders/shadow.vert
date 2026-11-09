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


// Sway in the scene's wind (MeshRenderer Wind > 0: trees, bushes; Scene::Wind). The higher a vertex sits in its
// mesh (Emissive.w = the mesh's lowest local y, Surface.w its height) the more it moves, so the base stays put.
// The gusts are the grass's (foliage.vert): waves rolling along the wind, so a tree and the grass under it move together.
uniform float u_SceneWindTime;
uniform vec2  u_SceneWindDir;
uniform float u_SceneWindSpeed;
uniform float u_SceneWindStrength;

vec4 ApplyWind(vec4 world, vec3 local, Instance inst) {
    float amount = inst.Surface.z * u_SceneWindStrength;
    if (amount == 0.0) return world;
    float h = clamp((local.y - inst.Emissive.w) / max(inst.Surface.w, 1e-3), 0.0, 1.0);
    vec3 root = inst.Model[3].xyz;
    float height = inst.Surface.w * length(inst.Model[1].xyz);           // metres, with the entity's scale
    float t = u_SceneWindTime * u_SceneWindSpeed;
    float along = dot(root.xz, u_SceneWindDir);
    float gust = sin(along * 0.23 - t * 1.7) * 0.5 + 0.5;
    gust *= 0.55 + 0.45 * sin(along * 0.061 + dot(root.xz, vec2(-u_SceneWindDir.y, u_SceneWindDir.x)) * 0.093 - t * 0.43);
    float phase = fract(sin(dot(root.xz, vec2(12.9898, 78.233))) * 43758.5453) * 6.2832;
    float sway = 0.3 + 0.7 * gust * gust + sin(t * 1.1 + phase) * 0.12;
    world.xz += u_SceneWindDir * (sway * amount * 0.035 * height * h * h);
    // leaves flutter: small, quick and different at every point (more in the gusts)
    float f = sin(t * 5.3 + dot(world.xyz, vec3(2.1, 1.7, 2.9)) + phase);
    world.xyz += vec3(f, 0.6 * f, -f) * (0.03 * amount * h * (0.4 + gust));
    return world;
}

void main() {
    Instance inst = u_Instances[u_Indices[u_InstanceBase + gl_InstanceID]];
    vec4 pos = vec4(a_Position, 1.0);
    if (u_Skinned == 1) {
        mat4 skin = a_Weights.x * u_Bones[a_Bones.x] + a_Weights.y * u_Bones[a_Bones.y]
                  + a_Weights.z * u_Bones[a_Bones.z] + a_Weights.w * u_Bones[a_Bones.w];
        pos = skin * pos;
    }
    vec4 world = inst.Model * pos;
    world = ApplyWind(world, pos.xyz, inst);
    v_WorldPos = world.xyz;
    v_TexCoord = a_TexCoord;
    v_Alpha = inst.Color.a;
    gl_Position = u_LightVP * world;
}
