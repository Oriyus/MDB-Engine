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

// Colour grading (PostSettings): neutral values leave the picture as it was
uniform vec3  u_WhiteBalance;
uniform float u_Contrast;
uniform float u_Saturation;
uniform vec3  u_ShadowTint, u_HighlightTint;
uniform float u_Vignette;
uniform float u_Grain, u_GrainSeed;

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

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

    // ---- colour grading, part 1: on the scene's light, before the tone map (so it rolls off, never clips)
    hdr *= u_WhiteBalance;                                              // temperature / tint
    hdr = 0.18 * pow(max(hdr, vec3(0.0)) / 0.18, vec3(u_Contrast));     // contrast about mid grey (a power curve)
    hdr = max(mix(vec3(dot(hdr, kLuma)), hdr, u_Saturation), vec3(0.0));

    vec3 mapped;
    if      (u_ToneMap == 1) mapped = hdr / (1.0 + hdr);
    else if (u_ToneMap == 2) mapped = ACES(hdr);
    else                     mapped = clamp(hdr, 0.0, 1.0);

    // ---- part 2: on the picture. Shadows take one tint and highlights another; the corners darken
    float luma = dot(mapped, kLuma);
    mapped *= mix(u_ShadowTint, u_HighlightTint, smoothstep(0.0, 0.6, luma));
    vec2 fromCentre = (v_UV - 0.5) * vec2(1.0, 0.8);
    mapped *= 1.0 - u_Vignette * smoothstep(0.25, 0.85, length(fromCentre) * 1.6);

    if (u_Gamma == 1) mapped = pow(max(mapped, vec3(0.0)), vec3(1.0 / 2.2));

    // Film grain: fine noise, new every frame, strongest in the mid tones
    if (u_Grain > 0.0) {
        float n = fract(sin(dot(gl_FragCoord.xy + u_GrainSeed * 17.0, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
        mapped += n * u_Grain * (1.0 - abs(luma * 2.0 - 1.0) * 0.6);
    }

    FragColor = vec4(clamp(mapped, 0.0, 1.0), 1.0);
}