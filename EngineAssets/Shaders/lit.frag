#version 450 core

in vec3 v_WorldPos;
in vec3 v_Normal;
in vec2 v_TexCoord;
in vec4  v_Color;            // per instance (DrawList), or per vertex (foliage.vert): rgb sRGB-style, a alpha
flat in vec3  v_Surface;     // metallic, roughness, specular (KHR_materials_specular factor x Mesh Renderer Specular)
flat in vec3  v_Emissive;
flat in int   v_EntityID;
flat in int   v_Unlit;

layout(location = 0) out vec4 FragColor;
layout(location = 1) out int  EntityID;
layout(location = 2) out vec2 Velocity;    // motion vector (UV this frame - last frame); written only while enabled
in vec4 v_CurClip;
in vec4 v_PrevClip;

#define MAX_POINT_LIGHTS 4
const float PI = 3.14159265359;
const float kUnlitGlow = 4.0;   // unlit objects (light markers) are drawn as hot, glowing emitters

uniform sampler2D        u_Texture;          // unit 0: albedo
uniform samplerCubeArrayShadow u_PointShadowMaps;  // unit 1: hardware compare (PointShadowArray), stored = distance / u_PointFar
uniform sampler2DArray   u_SunShadowMap;     // unit 2: one layer per cascade
uniform sampler2D        u_NormalMap;        // unit 3
uniform sampler2D        u_MetalRoughMap;    // unit 4
uniform sampler2D        u_EmissiveMap;      // unit 5

uniform int  u_UseTexture;
uniform int  u_UseNormalMap;
uniform int  u_UseMetalRough;
uniform int  u_UseEmissive;
uniform int  u_DoubleSided;      // 1: back faces are drawn too (Mesh Renderer / material Double-sided)
uniform int  u_ViewMode;        // editor view mode: 0 rendered (never set by the game), 1 solid, 2 textured

// Terrain (Scene/Terrain.h): four layers blended by a weight map over the terrain (v_TexCoord 0..1 across it)
uniform int       u_Terrain;            // 1: this batch is a terrain chunk
uniform sampler2D u_TerrainSplat;       // unit 9: RGBA = weights of layers 0..3
uniform sampler2D u_TerrainLayer0;      // units 10..13: each layer's colour texture (optional, repeating)
uniform sampler2D u_TerrainLayer1;
uniform sampler2D u_TerrainLayer2;
uniform sampler2D u_TerrainLayer3;
uniform sampler2D u_TerrainSurface0;    // units 14..17: each layer's packed maps (optional, linear, same repeat):
uniform sampler2D u_TerrainSurface1;    //   RG = normal X / Y (0..1, Z rebuilt), B = roughness, A = height
uniform sampler2D u_TerrainSurface2;
uniform sampler2D u_TerrainSurface3;
uniform vec4  u_TerrainUseTex;          // per layer: 1 = has a colour texture
uniform ivec4 u_TerrainMaps;            // per layer: which packed maps it has (1 normal, 2 roughness, 4 height)
uniform vec4  u_TerrainNormalStrength;  // per layer: scales the normal map's tilt
uniform vec4  u_TerrainColor[4];        // rgb sRGB-style tint, a roughness (times the roughness map)
uniform vec4  u_TerrainTiling;          // per layer: metres per repeat
uniform vec2  u_TerrainSize;            // the terrain's metres in X / Z
uniform float u_TerrainBlend;           // 0 = plain weights, 1 = height blending
uniform vec4  u_TerrainVariation;       // per layer: 0 = plain repeat, 1 = full stochastic (hex) tiling

// One layer at one point: its colour and its surface = tangent normal XY (in the terrain's UV axes, -1..1),
// roughness factor (1 without a map) and height (the height map, else the colour's brightness; 0.5 without either)
struct TerrainTap { vec4 color; vec4 surf; };

TerrainTap TerrainFetch(sampler2D col, sampler2D surf, bool useCol, int maps, vec2 uv, vec2 dx, vec2 dy, mat2 rot) {
    TerrainTap t;
    vec2 gx = rot * dx, gy = rot * dy;
    t.color = useCol ? textureGrad(col, uv, gx, gy) : vec4(1.0);
    t.surf = vec4(0.0, 0.0, 1.0, useCol ? dot(t.color.rgb, vec3(0.2126, 0.7152, 0.0722)) : 0.5);
    if (maps != 0) {
        vec4 s = textureGrad(surf, uv, gx, gy);
        if ((maps & 1) != 0) t.surf.xy = transpose(rot) * (s.rg * 2.0 - 1.0);   // back from the copy's turn
        if ((maps & 2) != 0) t.surf.z = s.b;
        if ((maps & 4) != 0) t.surf.w = s.a;
    }
    return t;
}

// ------------------------------------------------------------ stochastic tiling
// Hex-tiling (Mikkelsen 2022, "Practical Real-Time Hex-Tiling"): the plane is covered by a triangle grid, and every
// grid vertex owns a copy of the texture at a random offset and turn. A point takes the three copies of its
// triangle's corners, weighted by its barycentrics sharpened (power 7) and by the copies' heights, so the seams
// fall along the texture's own low parts instead of cross-fading into mush. Colour and maps use the same copies;
// a copy's normal is turned back by its turn. The derivatives are the plain ones turned with each copy, so the
// mip level is unchanged (no seams, no blur).
uint TileHash(ivec2 v, uint seed) {
    uint h = uint(v.x) * 0x8da6b343u ^ uint(v.y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
vec3 TileRandom(ivec2 v, uint seed) {         // offset x, offset y, turn: 0..1
    uint h = TileHash(v, seed);
    return vec3(h & 0x3FFu, (h >> 10) & 0x3FFu, (h >> 20) & 0x3FFu) / 1023.0;
}
TerrainTap HexCopy(sampler2D col, sampler2D surf, bool useCol, int maps, vec2 st, vec2 dx, vec2 dy, ivec2 v,
                   float amount, uint seed) {
    vec3 r = TileRandom(v, seed);
    float a = (r.z * 2.0 - 1.0) * 3.14159265 * amount;
    mat2 rot = mat2(cos(a), sin(a), -sin(a), cos(a));
    vec2 cen = vec2(float(v.x) + 0.5 * float(v.y), 0.8660254 * float(v.y)) / 3.4641016;   // the vertex in st
    return TerrainFetch(col, surf, useCol, maps, rot * (st - cen) + cen + r.xy * amount, dx, dy, rot);
}
TerrainTap TerrainSample(sampler2D col, sampler2D surf, bool useCol, int maps, vec2 st, vec2 dx, vec2 dy,
                         float amount, uint seed) {
    if (amount <= 0.0) return TerrainFetch(col, surf, useCol, maps, st, dx, dy, mat2(1.0));
    vec2 g = st * 3.4641016;                                  // 2 sqrt(3): about three hexes per repeat
    vec2 skew = vec2(g.x - 0.57735027 * g.y, 1.15470054 * g.y);   // lattice (1, 0), (0.5, sqrt(3)/2) -> unit square
    ivec2 base = ivec2(floor(skew));
    vec2 f = fract(skew);
    float z = 1.0 - f.x - f.y;
    float s = step(0.0, -z), s2 = 2.0 * s - 1.0;
    vec3 bw = vec3(-z * s2, s - f.y * s2, s - f.x * s2);
    ivec2 v1 = base + ivec2(s, s), v2 = base + ivec2(s, 1.0 - s), v3 = base + ivec2(1.0 - s, s);
    TerrainTap c1 = HexCopy(col, surf, useCol, maps, st, dx, dy, v1, amount, seed);
    TerrainTap c2 = HexCopy(col, surf, useCol, maps, st, dx, dy, v2, amount, seed);
    TerrainTap c3 = HexCopy(col, surf, useCol, maps, st, dx, dy, v3, amount, seed);
    vec3 d = mix(vec3(1.0), vec3(c1.surf.w, c2.surf.w, c3.surf.w), 0.6);
    vec3 w = d * pow(bw, vec3(7.0));
    w /= max(w.x + w.y + w.z, 1e-6);
    TerrainTap t;
    t.color = w.x * c1.color + w.y * c2.color + w.z * c3.color;
    t.surf  = w.x * c1.surf  + w.y * c2.surf  + w.z * c3.surf;
    return t;
}

// The terrain's surface here: albedo (linear), its sRGB colour without textures (solid view), roughness and the
// normal in tangent space (UV axes; (0, 0, 1) without normal maps). Height blending takes each layer's height
// (its height map, else its colour's brightness), so where layers meet, the higher one (stones, thick grass) shows
// through first instead of a soft cross-fade. A layer that isn't painted here is not sampled.
void TerrainSurface(vec2 uv, out vec3 albedo, out vec3 flatColor, out float rough, out vec3 tanNormal) {
    vec4 w = texture(u_TerrainSplat, uv);
    float total = w.x + w.y + w.z + w.w;
    w = total > 1e-4 ? w / total : vec4(1.0, 0.0, 0.0, 0.0);
    vec2 p = uv * u_TerrainSize;                       // metres across the terrain
    vec2 px = dFdx(p), py = dFdy(p);                   // taken here, before the per-layer branches
    vec4 inv = 1.0 / u_TerrainTiling;
    vec4 painted = step(1e-3, w);
    bvec4 useTex = greaterThan(u_TerrainUseTex, vec4(0.5));
    TerrainTap none = TerrainTap(vec4(1.0), vec4(0.0, 0.0, 1.0, 0.5));
    TerrainTap t0 = none, t1 = none, t2 = none, t3 = none;
    if (painted.x > 0.5 && (useTex.x || u_TerrainMaps.x != 0))
        t0 = TerrainSample(u_TerrainLayer0, u_TerrainSurface0, useTex.x, u_TerrainMaps.x, p * inv.x, px * inv.x, py * inv.x, u_TerrainVariation.x, 1u);
    if (painted.y > 0.5 && (useTex.y || u_TerrainMaps.y != 0))
        t1 = TerrainSample(u_TerrainLayer1, u_TerrainSurface1, useTex.y, u_TerrainMaps.y, p * inv.y, px * inv.y, py * inv.y, u_TerrainVariation.y, 2u);
    if (painted.z > 0.5 && (useTex.z || u_TerrainMaps.z != 0))
        t2 = TerrainSample(u_TerrainLayer2, u_TerrainSurface2, useTex.z, u_TerrainMaps.z, p * inv.z, px * inv.z, py * inv.z, u_TerrainVariation.z, 3u);
    if (painted.w > 0.5 && (useTex.w || u_TerrainMaps.w != 0))
        t3 = TerrainSample(u_TerrainLayer3, u_TerrainSurface3, useTex.w, u_TerrainMaps.w, p * inv.w, px * inv.w, py * inv.w, u_TerrainVariation.w, 4u);
    if (u_TerrainBlend > 0.0) {
        vec4 h = vec4(t0.surf.w, t1.surf.w, t2.surf.w, t3.surf.w);
        vec4 c = w + h * 0.6 * painted;                    // a layer that isn't painted here never shows
        float top = max(max(c.x, c.y), max(c.z, c.w)) - 0.15;
        vec4 b = max(c - top, 0.0);
        b /= max(b.x + b.y + b.z + b.w, 1e-4);
        w = mix(w, b, u_TerrainBlend);
    }
    vec3 c0 = pow(u_TerrainColor[0].rgb, vec3(2.2)), c1 = pow(u_TerrainColor[1].rgb, vec3(2.2));
    vec3 c2 = pow(u_TerrainColor[2].rgb, vec3(2.2)), c3 = pow(u_TerrainColor[3].rgb, vec3(2.2));
    albedo = c0 * t0.color.rgb * w.x + c1 * t1.color.rgb * w.y + c2 * t2.color.rgb * w.z + c3 * t3.color.rgb * w.w;
    flatColor = u_TerrainColor[0].rgb * w.x + u_TerrainColor[1].rgb * w.y + u_TerrainColor[2].rgb * w.z + u_TerrainColor[3].rgb * w.w;
    rough = dot(w, vec4(u_TerrainColor[0].a * t0.surf.z, u_TerrainColor[1].a * t1.surf.z,
                        u_TerrainColor[2].a * t2.surf.z, u_TerrainColor[3].a * t3.surf.z));
    // Normals: each layer's tilt scaled by its strength, Z rebuilt, blended by the final weights
    vec4 k = u_TerrainNormalStrength;
    vec2 n0 = t0.surf.xy * k.x, n1 = t1.surf.xy * k.y, n2 = t2.surf.xy * k.z, n3 = t3.surf.xy * k.w;
    tanNormal = w.x * vec3(n0, sqrt(max(1.0 - dot(n0, n0), 0.0))) + w.y * vec3(n1, sqrt(max(1.0 - dot(n1, n1), 0.0)))
              + w.z * vec3(n2, sqrt(max(1.0 - dot(n2, n2), 0.0))) + w.w * vec3(n3, sqrt(max(1.0 - dot(n3, n3), 0.0)));
    tanNormal = normalize(tanNormal);
}


// Transparency: 0 opaque, 1 cutout (discard below u_AlphaCutoff), 2 blend (premultiplied output)
uniform int   u_AlphaMode;
uniform float u_AlphaCutoff;

uniform vec3  u_ViewPos;
uniform float u_Ambient;        // strength of the procedural sky light

uniform int   u_PointCount;
uniform vec3  u_PointPos[MAX_POINT_LIGHTS];
uniform vec3  u_PointColor[MAX_POINT_LIGHTS];
uniform int   u_PointShadow[MAX_POINT_LIGHTS];
uniform float u_PointFar;
uniform float u_PointBias;

uniform int   u_HasSun;
uniform vec3  u_SunDir;
uniform vec3  u_SunColor;
uniform int   u_SunShadow;
// Cascaded sun shadows: cascade c covers view depth up to u_CascadeSplits[c]
const int MAX_CASCADES = 4;
uniform int   u_CascadeCount;
uniform float u_CascadeSplits[MAX_CASCADES];   // a float array: indexing a vec4 by a loop variable misbehaves on some drivers
uniform vec4  u_ViewRow;      // row 2 of the camera view matrix: view depth = -dot(u_ViewRow, (p, 1))
uniform mat4  u_SunVP[MAX_CASCADES];
uniform float u_SunBias[MAX_CASCADES];      // depth units of that cascade
uniform float u_SunTexel[MAX_CASCADES];     // world size of one texel, for the normal offset
uniform int   u_CascadeDebug;

const vec3 kOffsets[20] = vec3[](
    vec3( 1,  1,  1), vec3( 1, -1,  1), vec3(-1, -1,  1), vec3(-1,  1,  1),
    vec3( 1,  1, -1), vec3( 1, -1, -1), vec3(-1, -1, -1), vec3(-1,  1, -1),
    vec3( 1,  1,  0), vec3( 1, -1,  0), vec3(-1, -1,  0), vec3(-1,  1,  0),
    vec3( 1,  0,  1), vec3(-1,  0,  1), vec3( 1,  0, -1), vec3(-1,  0, -1),
    vec3( 0,  1,  1), vec3( 0, -1,  1), vec3( 0, -1, -1), vec3( 0,  1, -1)
);

// ------------------------------------------------------------ shadows (unchanged)
float PointShadow(int i, vec3 N, vec3 L) {
    vec3  fragToLight = v_WorldPos - u_PointPos[i];
    float current = length(fragToLight);
    if (current > u_PointFar) return 0.0;

    float bias = max(u_PointBias * 2.0 * (1.0 - dot(N, L)), u_PointBias * 0.5);
    float viewDist = length(u_ViewPos - v_WorldPos);
    float radius = (1.0 + viewDist / u_PointFar) * 0.03;

    // 8 fetches at the cube's corners (kOffsets[0..7]), each a hardware 2x2 comparison with bilinear weights:
    // as soft as the old 20 single comparisons. Measured on the village (4 shadowed lamps, 1080p): 15.2 -> 14.2 ms a frame
    float ref = (current - bias) / u_PointFar;
    float lit = 0.0;
    for (int k = 0; k < 8; k++)
        lit += texture(u_PointShadowMaps, vec4(fragToLight + kOffsets[k] * radius, float(i)), ref);
    return 1.0 - lit / 8.0;
}

float ViewDepth() { return -dot(u_ViewRow, vec4(v_WorldPos, 1.0)); }

// The cascade covering this pixel, or u_CascadeCount when it is beyond the shadow distance
int SunCascade(float depth) {
    int c = 0;
    while (c < u_CascadeCount && depth > u_CascadeSplits[c]) c++;
    return c;
}

float SampleCascade(int c, vec3 N, vec3 L) {
    vec4 lp = u_SunVP[c] * vec4(v_WorldPos + N * u_SunTexel[c] * 2.0, 1.0);
    vec3 p = lp.xyz / lp.w * 0.5 + 0.5;
    if (p.z > 1.0 || p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) return 0.0;

    float bias = max(u_SunBias[c] * (1.0 - dot(N, L)), u_SunBias[c] * 0.1);
    vec2 texel = 1.0 / vec2(textureSize(u_SunShadowMap, 0).xy);

    float shadow = 0.0;
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            float closest = textureLod(u_SunShadowMap, vec3(p.xy + vec2(x, y) * texel, float(c)), 0.0).r;
            shadow += (p.z - bias > closest) ? 1.0 : 0.0;
        }
    }
    return shadow / 9.0;
}

float SunShadow(vec3 N, vec3 L) {
    float depth = ViewDepth();
    int c = SunCascade(depth);
    if (c >= u_CascadeCount) return 0.0;
    float shadow = SampleCascade(c, N, L);

    // The last 10% of a cascade blends into the next one; the last cascade fades out instead
    float end = u_CascadeSplits[c];
    float start = c == 0 ? 0.0 : u_CascadeSplits[c - 1];
    float band = 0.1 * (end - start);
    float t = clamp((depth - (end - band)) / max(band, 1e-4), 0.0, 1.0);
    if (t > 0.0) {
        if (c + 1 < u_CascadeCount) shadow = mix(shadow, SampleCascade(c + 1, N, L), t);
        else shadow *= 1.0 - t;
    }
    return shadow;
}

// ------------------------------------------------------------ PBR building blocks
float DistributionGGX(float NdotH, float a) {          // how many microfacets face the half vector
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float GeometrySchlick(float NdotX, float k) {
    return NdotX / (NdotX * (1.0 - k) + k);
}

float GeometrySmith(float NdotV, float NdotL, float rough) {   // microfacet self-shadowing
    float r = rough + 1.0;
    float k = r * r / 8.0;
    return GeometrySchlick(NdotV, k) * GeometrySchlick(NdotL, k);
}

// Surfaces reflect more at grazing angles, up to F90 (1, or the specular factor for non-metals: KHR_materials_specular)
vec3 FresnelSchlick(float cosTheta, vec3 F0, vec3 F90) {
    return F0 + (F90 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Cook-Torrance for one light. "radiance" is multiplied by PI so a light of intensity 1
// lights a white surface facing it to about 1.0, like the old shader did.
vec3 Shade(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float rough, vec3 F0, vec3 F90,
           vec3 radiance, float shadow) {
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);

    vec3  H = normalize(V + L);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);

    float D = DistributionGGX(NdotH, rough * rough);
    float G = GeometrySmith(NdotV, NdotL, rough);
    vec3  F = FresnelSchlick(max(dot(H, V), 0.0), F0, F90);

    vec3 specular = D * G * F / (4.0 * NdotV * NdotL + 1e-4);
    vec3 kD = (1.0 - F) * (1.0 - metallic);             // metals have no diffuse

    return (1.0 - shadow) * (kD * albedo / PI + specular) * radiance * PI * NdotL;
}

// Normal mapping without a tangent attribute: the tangent frame is rebuilt per pixel
// from screen-space derivatives of position and UV.
vec3 PerturbNormal(vec3 N, vec3 p, vec2 uv, vec3 mapN) {
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float m = max(dot(T, T), dot(B, B));
    if (m < 1e-20) return N;
    float invmax = inversesqrt(m);
    return normalize(mat3(T * invmax, B * invmax, N) * mapN);
}

// ------------------------------------------------------------ stand-in environment
// A simple sky gradient (linear values). Replaced by a real HDR skybox later.
vec3 Env(vec3 d) {
    vec3 ground  = vec3(0.010, 0.010, 0.012);
    vec3 horizon = vec3(0.045, 0.050, 0.060);
    vec3 sky     = vec3(0.050, 0.090, 0.180);
    return d.y > 0.0 ? mix(horizon, sky, sqrt(d.y)) : mix(horizon, ground, sqrt(-d.y));
}

// Analytic fit of the "split-sum" environment BRDF (Lazarov), saves a lookup texture
vec3 EnvBRDFApprox(vec3 F0, vec3 F90, float rough, float NdotV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.040, -0.040);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    return F0 * AB.x + F90 * AB.y;       // B is the grazing part: it scales with F90 (F90 = 1: as before)
}

uniform samplerCube u_IrradianceMap;   // unit 6
uniform samplerCube u_PrefilterMap;    // unit 7
uniform int   u_UseIBL;
uniform float u_PrefilterMaxLod;
uniform float u_EnvRot;

// Spin the environment lookup around Y (same function as sky.frag)
vec3 EnvDir(vec3 d) {
    float c = cos(u_EnvRot), s = sin(u_EnvRot);
    return vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);
}

void main() {
    EntityID = v_EntityID;
    Velocity = (v_CurClip.xy / v_CurClip.w - v_PrevClip.xy / v_PrevClip.w) * 0.5;   // NDC -> UV

    // Picker colors are sRGB, albedo textures are decoded to linear by the GPU
    vec4 base = (u_UseTexture == 1) ? texture(u_Texture, v_TexCoord) : vec4(1.0);
    vec3 albedo = pow(v_Color.rgb, vec3(2.2)) * base.rgb;
    float alpha = clamp(v_Color.a * base.a, 0.0, 1.0);
    if (u_AlphaMode == 1 && alpha < u_AlphaCutoff) discard;
    bool blend = u_AlphaMode == 2;
    if (!blend) alpha = 1.0;
    vec3 terrainFlat = vec3(1.0);
    float terrainRough = -1.0;
    vec3 terrainN = vec3(0.0, 0.0, 1.0);                     // tangent space (the terrain's UV axes)
    bool terrainNormals = u_Terrain == 1 && any(notEqual(u_TerrainMaps & 1, ivec4(0)));
    if (u_Terrain == 1) TerrainSurface(v_TexCoord, albedo, terrainFlat, terrainRough, terrainN);

    // Editor view modes (SceneRenderer::ViewMode): 1 solid (colour only), 2 textured. A simple studio light
    // that follows the camera, no shadows or environment. 0 (rendered, the default) is the full shading below.
    if (u_ViewMode != 0) {
        vec3 c = pow(v_Color.rgb, vec3(2.2));
        if (u_ViewMode == 2) c *= base.rgb;
        if (u_Terrain == 1) c = u_ViewMode == 2 ? albedo : pow(terrainFlat, vec3(2.2));
        if (v_Unlit == 0) {
            vec3 N = normalize(v_Normal);
            if (terrainNormals && u_ViewMode == 2) N = PerturbNormal(N, v_WorldPos, v_TexCoord, terrainN);
            if (!gl_FrontFacing) N = -N;                         // grass is drawn without culling
            vec3 V = normalize(u_ViewPos - v_WorldPos);
            vec3 L = normalize(V + vec3(0.25, 0.9, 0.1));         // from above the camera
            float key = max(dot(N, L), 0.0);
            float sky = 0.5 + 0.5 * N.y;
            c *= 0.12 + 0.18 * sky + 0.5 * key;               // white peaks at 0.8, so shapes never clip
        }
        FragColor = vec4(c * alpha, alpha);
        return;
    }

    if (v_Unlit == 1) {
        FragColor = vec4(albedo * kUnlitGlow * alpha, alpha);
        return;
    }

    float metallic = v_Surface.x;
    float roughness = terrainRough >= 0.0 ? terrainRough : v_Surface.y;
    if (u_UseMetalRough == 1) {
        vec3 mr = texture(u_MetalRoughMap, v_TexCoord).rgb;   // glTF: G = roughness, B = metallic
        roughness *= mr.g;
        metallic  *= mr.b;
    }
    metallic  = clamp(metallic, 0.0, 1.0);
    roughness = clamp(roughness, 0.04, 1.0);                 // 0 would make an infinitely sharp highlight

    vec3 Ng = normalize(v_Normal);                           // geometric normal, used for shadows
    if (u_DoubleSided == 1 && !gl_FrontFacing) Ng = -Ng;    // the back of a double-sided part faces the other way
    vec3 N = Ng;
    if (u_UseNormalMap == 1) {
        vec3 m = texture(u_NormalMap, v_TexCoord).xyz * 2.0 - 1.0;
        N = PerturbNormal(Ng, v_WorldPos, v_TexCoord, m);
    }
    if (terrainNormals) N = PerturbNormal(Ng, v_WorldPos, v_TexCoord, terrainN);

    vec3 V = normalize(u_ViewPos - v_WorldPos);
    // 4% reflectance for non-metals, scaled by the specular factor (0 = no reflections or highlights at all:
    // dark bark, cloth; KHR_materials_specular / Mesh Renderer Specular). Metals keep their full reflection.
    float specular = clamp(v_Surface.z, 0.0, 1.0);
    vec3 F0 = mix(vec3(0.04 * specular), albedo, metallic);
    vec3 F90 = vec3(mix(specular, 1.0, metallic));
    // See-through: the diffuse part is scaled by alpha (premultiplied), reflections stay, like glass
    vec3 diffuse = albedo * alpha;

    vec3 color = vec3(0.0);

    for (int i = 0; i < u_PointCount; i++) {
        vec3 Lv = u_PointPos[i] - v_WorldPos;
        float dist = length(Lv);
        vec3 L = Lv / dist;
        float att = 1.0 / (1.0 + 0.02 * dist * dist);

        float shadow = (u_PointShadow[i] == 1 && dot(Ng, L) > 0.0) ? PointShadow(i, Ng, L) : 0.0;
        color += Shade(N, V, L, diffuse, metallic, roughness, F0, F90, u_PointColor[i] * att, shadow);
    }

    if (u_HasSun == 1) {
        vec3 L = normalize(-u_SunDir);
        float shadow = (u_SunShadow == 1 && dot(Ng, L) > 0.0) ? SunShadow(Ng, L) : 0.0;
        color += Shade(N, V, L, diffuse, metallic, roughness, F0, F90, u_SunColor, shadow);
    }

        // Ambient: diffuse from the irradiance map, reflections from the prefiltered map
    float NdotV = max(dot(N, V), 1e-4);
    vec3 specAmb = EnvBRDFApprox(F0, F90, roughness, NdotV);
    vec3 R = reflect(-V, N);

    vec3 diffuseLight, specularLight;
    if (u_UseIBL == 1) {
        diffuseLight  = texture(u_IrradianceMap, EnvDir(N)).rgb;
        specularLight = textureLod(u_PrefilterMap, EnvDir(R), roughness * u_PrefilterMaxLod).rgb;
    } else {
        diffuseLight  = Env(N);
        specularLight = Env(normalize(mix(R, N, roughness * roughness)));
    }
    color += ((1.0 - metallic) * diffuse * (1.0 - specAmb) * diffuseLight + specAmb * specularLight) * u_Ambient;

    vec3 emissive = v_Emissive;
    if (u_UseEmissive == 1) emissive *= texture(u_EmissiveMap, v_TexCoord).rgb;
    color += emissive * alpha;
    // Fade the reflections out too near alpha 0, so a fully faded object disappears
    if (blend) color *= min(1.0, alpha * 8.0);

    // Debug view: tint each sun cascade (red, green, blue, yellow)
    if (u_CascadeDebug == 1 && u_SunShadow == 1) {
        const vec3 tints[4] = vec3[](vec3(1.0, 0.45, 0.45), vec3(0.45, 1.0, 0.45), vec3(0.45, 0.6, 1.0), vec3(1.0, 1.0, 0.4));
        int c = SunCascade(ViewDepth());
        if (c < u_CascadeCount) color *= tints[c];
    }

    FragColor = vec4(color, alpha);
}