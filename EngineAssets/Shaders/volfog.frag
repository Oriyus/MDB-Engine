#version 450 core
// Volumetric fog, marched at half resolution (Renderer/VolumetricFog). Output per 2x2 block of pixels, at its
// nearest and farthest depth: rgb = light scattered towards the camera, a = transmittance
// (how much of the scene behind still shows). The lighting uniforms are lit.frag's
// (Scene::SetLightingUniforms): the same sun, cascades, sky and point lights.

in vec2 v_UV;
layout(location = 0) out vec4 NearFog;   // rgb light scattered towards the camera, a transmittance: to the block's nearest depth
layout(location = 1) out vec4 FarFog;    // the same to its farthest depth
layout(location = 2) out vec2 Range;     // those two distances (metres)

#include "lights.glsl"
const int MAX_CASCADES = 4;
const float PI = 3.14159265359;

uniform sampler2D u_Depth;                 // unit 9: the scene depth, full resolution
uniform mat4  u_InvViewProjection;
uniform vec3  u_ViewPos;

uniform float u_Density, u_HeightFalloff, u_BaseHeight;
uniform vec3  u_FogColor;                  // linear
uniform float u_Anisotropy, u_SunScale, u_AmbientScale, u_PointScale;
uniform int   u_FogPointShadows;      // 1: shadowed point lights cut shafts too (VolumetricFogSettings::PointShadows; spots always)
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

// How far the fog goes along the ray to a full-resolution pixel: to the surface, or MaxDistance (also for the sky)
float FogDistance(ivec2 px, ivec2 full) {
    float depth = texelFetch(u_Depth, px, 0).r;
    if (depth >= 1.0) return u_MaxDistance;
    vec2 uv = (vec2(px) + 0.5) / vec2(full);
    vec4 w = u_InvViewProjection * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    return min(length(w.xyz / w.w - u_ViewPos), u_MaxDistance);
}

vec3 g_SunL, g_Sky;
int g_Tile = 0, g_Slice = -1;     // this ray's light tile, and the slice whose cell g_Cell holds (it changes a few times a ray)
uvec2 g_Cell = uvec2(0);

// Marches [from, to] in n steps, adding to light and trans
void March(vec3 dir, float from, float to, int n, float jitter, inout vec3 light, inout float trans) {
    if (n <= 0 || to <= from) return;
    float stepLen = (to - from) / float(n);
    for (int i = 0; i < n; i++) {
        if (trans < 0.01) return;
        vec3 p = u_ViewPos + dir * (from + stepLen * (float(i) + jitter));
        float d = Density(p);
        if (d > 1e-5) {
            vec3 L = g_Sky;
            if (u_HasSun == 1) L += g_SunL * SunLight(p);
            for (int k = 0; k < u_PointCount; k++) {      // the shadowed lights, directly (lights.glsl)
                vec3 toLight = u_PointPosRange[k].xyz - p;
                if (u_PointExtra[k].z > 0.5) toLight = AreaClosest(u_PointPosRange[k].xyz, u_PointSpot[k].xyz, u_PointArea[k], u_PointExtra[k].z, p) - p;   // an area light
                float d2 = dot(toLight, toLight);
                float r = u_PointPosRange[k].w;
                if (d2 >= r * r) continue;
                float dl = sqrt(d2);
                vec3 Lk = toLight / max(dl, 1e-4);
                float w = u_PointScale * LightWindow(dl, r) * SpotCone(Lk, u_PointSpot[k], u_PointColor[k].w) * AreaEmission(u_PointSpot[k], u_PointExtra[k], Lk) / (1.0 + d2 * 1.5);
                vec3 lc = u_PointColor[k].rgb * w;
                if (dot(lc, vec3(0.333)) < 1e-3) continue;          // too faint here for its shadow or cookie to matter
                if (u_PointCookie[k].x >= 0.0 || u_PointCookie[k].z >= 0.0) lc *= LightCookie(u_PointSpot[k], u_PointCookie[k].xyz, u_PointFrames[k].xyz, Lk, 0.05);
                if (u_PointShadow[k].x >= 0) lc *= AtlasSpotLit(u_PointShadow[k].x, p, 1.0, dl, 1, 0.0);   // shafts
                else if (u_PointShadow[k].y >= 0 && u_FogPointShadows == 1) lc *= AtlasCubeLit(u_PointShadow[k].y, u_PointPosRange[k].xyz, p, 1.0, 1, 0.0);
                L += lc;
            }
            if (u_ClusterLightCount > 0) {          // the others: those of this sample's cluster
                int z = LightSlice(p);
                if (z != g_Slice) { g_Slice = z; g_Cell = LightCell(g_Tile, z); }
                uvec2 cell = g_Cell;
                for (uint k = 0u; k < cell.y; k++) {
                    PointLightGpu pl = u_Lights[u_ClusterIndices[cell.x + k]];
                    vec3 toLight = pl.PosRange.xyz - p;
                    if (pl.Extra.z > 0.5) toLight = AreaClosest(pl.PosRange.xyz, pl.Spot.xyz, pl.Area, pl.Extra.z, p) - p;
                    float d2 = dot(toLight, toLight);
                    if (d2 >= pl.PosRange.w * pl.PosRange.w) continue;
                    float dl = sqrt(d2);
                    float lw = LightWindow(dl, pl.PosRange.w) * SpotCone(toLight / max(dl, 1e-4), pl.Spot, pl.Color.w) * AreaEmission(pl.Spot, pl.Extra, toLight / max(dl, 1e-4));
                    if (lw <= 0.0 || dot(pl.Color.rgb, vec3(0.333)) * u_PointScale * lw / (1.0 + d2 * 1.5) < 1e-3) continue;   // too faint here
                    if (pl.Shadow.x >= 0.0) lw *= AtlasSpotLit(int(pl.Shadow.x), p, 1.0, dl, 1, 0.0);   // shafts
                    else if (pl.Shadow.y >= 0.0 && u_FogPointShadows == 1) lw *= AtlasCubeLit(int(pl.Shadow.y), pl.PosRange.xyz, p, 1.0, 1, 0.0);
                    vec3 lc = pl.Color.rgb;
                    if (pl.Shadow.z >= 0.0 || pl.Cookie.x >= 0.0) lc *= LightCookie(pl, toLight / max(dl, 1e-4), 0.05);   // coloured / patterned shafts
                    L += lc * u_PointScale * lw / (1.0 + d2 * 1.5);
                }
            }
            float stepTrans = exp(-d * stepLen);
            light += trans * L * u_FogColor * (1.0 - stepTrans);    // what this slice scatters, dimmed by what is in front
            trans *= stepTrans;
        }
    }
}

// Each half-resolution texel covers a 2x2 block of full-resolution pixels, which can lie at very different depths
// (a thin distant branch in front of the sky). One march along the block's centre ray records the fog at the
// block's nearest and farthest depth, and the composite interpolates between the two by each pixel's own depth,
// so every pixel finds a texel that matches it. (Marching to a single pixel of the block left the others to borrow
// fog marched to the wrong depth: leaves lit up with the sky's fog, on and off as they swayed.)
void main() {
    ivec2 full = textureSize(u_Depth, 0);
    ivec2 px = ivec2(gl_FragCoord.xy) * 2;
    float d0 = FogDistance(min(px, full - 1), full);
    float d1 = FogDistance(min(px + ivec2(1, 0), full - 1), full);
    float d2 = FogDistance(min(px + ivec2(0, 1), full - 1), full);
    float d3 = FogDistance(min(px + ivec2(1, 1), full - 1), full);
    float nearD = min(min(d0, d1), min(d2, d3));
    float farD = max(max(d0, d1), max(d2, d3));

    vec2 uv = min(vec2(px) + 1.0, vec2(full) - 0.5) / vec2(full);      // the block's centre
    vec4 w = u_InvViewProjection * vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    vec3 dir = normalize(w.xyz / w.w - u_ViewPos);

    g_Tile = LightTile(v_UV);
    g_SunL = vec3(0.0);
    if (u_HasSun == 1) g_SunL = u_SunColor * u_SunScale * Phase(dot(dir, normalize(-u_SunDir)), u_Anisotropy) * 0.25;
    g_Sky = SkyLight() * u_AmbientScale;

    // the steps are shared in proportion to the two lengths (about the same step length in both)
    int nNear = clamp(int(round(float(u_Steps) * nearD / max(farD, 1e-3))), 1, u_Steps);
    int nFar = farD > nearD + 1e-3 ? max(u_Steps - nNear, 1) : 0;
    float jitter = IGN(gl_FragCoord.xy);

    vec3 light = vec3(0.0);
    float trans = 1.0;
    March(dir, 0.0, nearD, nNear, jitter, light, trans);
    NearFog = vec4(light, trans);
    March(dir, nearD, farD, nFar, jitter, light, trans);
    FarFog = vec4(light, trans);
    Range = vec2(nearD, farD);
}
