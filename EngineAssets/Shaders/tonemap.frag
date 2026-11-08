#version 450 core

in vec2 v_UV;
out vec4 FragColor;

uniform sampler2D u_Scene;
uniform sampler2D u_AO;
uniform sampler2D u_Bloom;
uniform float u_Exposure;
uniform int   u_ToneMap;     // 0 = clamp, 1 = Reinhard, 2 = ACES
uniform int   u_Gamma;

uniform int   u_UseAO;
uniform float u_AOStrength;
uniform int   u_AODebug;     // show the AO buffer on its own
uniform int   u_UseBloom;
uniform float u_BloomIntensity;

vec3 ACES(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    if (u_AODebug == 1 && u_UseAO == 1) {
        FragColor = vec4(vec3(texture(u_AO, v_UV).r), 1.0);
        return;
    }

    vec3 hdr = texture(u_Scene, v_UV).rgb;
    if (u_UseAO == 1)    hdr *= mix(1.0, texture(u_AO, v_UV).r, u_AOStrength);
    if (u_UseBloom == 1) hdr += texture(u_Bloom, v_UV).rgb * u_BloomIntensity;
    hdr *= u_Exposure;

    vec3 mapped;
    if      (u_ToneMap == 1) mapped = hdr / (1.0 + hdr);
    else if (u_ToneMap == 2) mapped = ACES(hdr);
    else                     mapped = clamp(hdr, 0.0, 1.0);

    if (u_Gamma == 1) mapped = pow(mapped, vec3(1.0 / 2.2));

    FragColor = vec4(mapped, 1.0);
}