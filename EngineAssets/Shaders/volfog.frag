#version 450 core
// Volumetric fog, marched at half resolution (Renderer/VolumetricFog). Output: rgb = light scattered towards the
// camera, a = transmittance (how much of the scene behind still shows). The lighting uniforms are lit.frag's
// (Scene::SetLightingUniforms): the same sun, cascades, sky and point lights.

in vec2 v_UV;
out vec4 FragColor;

#define MAX_POINT_LIGHTS 4
const int MAX_CASCADES = 4;
const float PI = 3.14159265359;

uniform sampler2D u_Depth;                 // unit 9: the scene depth, full resolution
uniform mat4  u_InvViewProjection;
uniform vec3  u_ViewPos;

uniform float u_Density, u_HeightFalloff, u_BaseHeight;
uniform vec3  u_FogColor;                  // linear
uniform float u_Anisotropy, u_SunScale, u_AmbientScale, u_PointScale;
uniform float u_NoiseScale, u_NoiseAmount;
uniform vec3  u_Wind;                      // how far the wisps have drifted (metres)
uniform float u_MaxDistance;
uniform int   u_Steps;

uniform int   u_HasSun;
uniform vec3  u_SunDir;
uniform vec3  u_SunColor;
uniform int   u_SunShadow;
uniform int   u_CascadeCount;
uniform float u_CascadeSplits[MAX_CASCADES];
uniform vec4  u_ViewRow;
uniform mat4  u_SunVP[MAX_CASCADES];
uniform float u_SunBias[MAX_CASCADES];
uniform sampler2DArray u_SunShadowMap;     // unit 2

uniform int   u_PointCount;
uniform vec3  u_PointPos[MAX_POINT_LIGHTS];
uniform vec3  u_PointColor[MAX_POINT_LIGHTS];

uniform samplerCube u_IrradianceMap;       // unit 6
uniform int   u_UseIBL;
uniform float u_Ambient;

// ------------------------------------------------------------ noise (value noise, 3D, two octaves)
float Hash(vec3 p) {
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float Noise(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(Hash(i + vec3(0, 0, 0)), Hash(i + vec3(1, 0, 0)), f.x),
                   mix(Hash(i + vec3(0, 1, 0)), Hash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(Hash(i + vec3(0, 0, 1)), Hash(i + vec3(1, 0, 1)), f.x),
                   mix(Hash(i + vec3(0, 1, 1)), Hash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float Wisps(vec3 p) {
    vec3 q = p * u_NoiseScale + u_Wind * u_NoiseScale;
    return Noise(q) * 0.65 + Noise(q * 2.7 + vec3(4.1, 1.7, 8.3)) * 0.35;
}

float Density(vec3 p) {
    float d = u_Density * exp(-max(p.y - u_BaseHeight, 0.0) / u_HeightFalloff);
    return d * max(0.0, mix(1.0, Wisps(p) * 2.2 - 0.25, u_NoiseAmount));
}

// ------------------------------------------------------------ lighting
// Is the sun seen from p? One tap of the cascade covering p (the march averages many)
float SunLight(vec3 p) {
    if (u_SunShadow != 1) return 1.0;
    float depth = -dot(u_ViewRow, vec4(p, 1.0));
    int c = 0;
    while (c < u_CascadeCount && depth > u_CascadeSplits[c]) c++;
    if (c >= u_CascadeCount) return 1.0;
    vec4 lp = u_SunVP[c] * vec4(p, 1.0);
    vec3 q = lp.xyz / lp.w * 0.5 + 0.5;
    if (q.z > 1.0 || q.x < 0.0 || q.x > 1.0 || q.y < 0.0 || q.y > 1.0) return 1.0;
    float closest = textureLod(u_SunShadowMap, vec3(q.xy, float(c)), 0.0).r;
    return q.z - u_SunBias[c] * 2.0 > closest ? 0.0 : 1.0;
}

// Henyey-Greenstein, scaled so that even scattering (g = 0) is 1
float Phase(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
}

vec3 SkyLight() {            // the light the sky sends into the fog from all around (as lit.frag's ambient)
    if (u_UseIBL == 1)
        return (texture(u_IrradianceMap, vec3(0, 1, 0)).rgb * 0.5 + texture(u_IrradianceMap, vec3(1, 0.2, 0)).rgb * 0.25
              + texture(u_IrradianceMap, vec3(-1, 0.2, 0)).rgb * 0.25) * u_Ambient;
    return vec3(0.05, 0.075, 0.12) * u_Ambient;
}

// Interleaved gradient noise: a different start along the ray per pixel, so the steps don't show as bands
float IGN(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

void main() {
    ivec2 full = textureSize(u_Depth, 0);
    ivec2 px = min(ivec2(gl_FragCoord.xy) * 2, full - 1);       // the same full-resolution pixel the composite checks
    float depth = texelFetch(u_Depth, px, 0).r;
    vec2 uv = (vec2(px) + 0.5) / vec2(full);
    vec4 w = u_InvViewProjection * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec3 end = w.xyz / w.w;
    vec3 ray = end - u_ViewPos;
    float dist = length(ray);
    vec3 dir = ray / max(dist, 1e-4);
    float range = depth >= 1.0 ? u_MaxDistance : min(dist, u_MaxDistance);

    int steps = u_Steps;
    float stepLen = range / float(steps);
    float t = stepLen * IGN(gl_FragCoord.xy);

    vec3 sunL = vec3(0.0);
    if (u_HasSun == 1) sunL = u_SunColor * u_SunScale * Phase(dot(dir, normalize(-u_SunDir)), u_Anisotropy) * 0.25;
    vec3 sky = SkyLight() * u_AmbientScale;

    float trans = 1.0;
    vec3 light = vec3(0.0);
    for (int i = 0; i < steps; i++) {
        vec3 p = u_ViewPos + dir * t;
        float d = Density(p);
        if (d > 1e-5) {
            vec3 L = sky;
            if (u_HasSun == 1) L += sunL * SunLight(p);
            for (int k = 0; k < u_PointCount; k++) {
                vec3 to = u_PointPos[k] - p;
                L += u_PointColor[k] * u_PointScale / (1.0 + dot(to, to) * 1.5);
            }
            float stepTrans = exp(-d * stepLen);
            light += trans * L * u_FogColor * (1.0 - stepTrans);    // what this slice scatters, dimmed by what is in front
            trans *= stepTrans;
            if (trans < 0.01) break;
        }
        t += stepLen;
    }
    FragColor = vec4(light, trans);
}
