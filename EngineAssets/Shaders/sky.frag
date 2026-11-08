#version 450 core

in vec3 v_Dir;
layout(location = 0) out vec4 FragColor;
layout(location = 1) out int  EntityID;

uniform samplerCube u_Env;
uniform float u_Lod;
uniform float u_Intensity;
uniform float u_EnvRot;

vec3 EnvDir(vec3 d) {
    float c = cos(u_EnvRot), s = sin(u_EnvRot);
    return vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

void main() {
    EntityID = -1;    // the picking buffer has a second output, so write "no entity" for the sky
    FragColor = vec4(textureLod(u_Env, EnvDir(normalize(v_Dir)), u_Lod).rgb * u_Intensity, 1.0);
}