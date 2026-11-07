#version 450 core

in vec3 v_WorldPos;
in vec3 v_Normal;
in vec2 v_TexCoord;

layout(location = 0) out vec4 FragColor;
layout(location = 1) out int  EntityID;

#define MAX_POINT_LIGHTS 4
const float PI = 3.14159265359;
const float kUnlitGlow = 4.0;   // unlit objects (light markers) are drawn as hot, glowing emitters

uniform sampler2D        u_Texture;          // unit 0: albedo
uniform samplerCubeArray u_PointShadowMaps;  // unit 1
uniform sampler2D        u_SunShadowMap;     // unit 2
uniform sampler2D        u_NormalMap;        // unit 3
uniform sampler2D        u_MetalRoughMap;    // unit 4
uniform sampler2D        u_EmissiveMap;      // unit 5

uniform int  u_UseTexture;
uniform int  u_UseNormalMap;
uniform int  u_UseMetalRough;
uniform int  u_UseEmissive;
uniform int  u_Unlit;
uniform int  u_EntityID;

uniform vec3  u_Color;
uniform float u_Metallic;
uniform float u_Roughness;
uniform vec3  u_Emissive;

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
uniform mat4  u_SunVP;
uniform float u_SunBias;
uniform float u_SunTexel;

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

    float shadow = 0.0;
    for (int k = 0; k < 20; k++) {
        float closest = textureLod(u_PointShadowMaps, vec4(fragToLight + kOffsets[k] * radius, float(i)), 0.0).r * u_PointFar;
        if (current - bias > closest) shadow += 1.0;
    }
    return shadow / 20.0;
}

float SunShadow(vec3 N, vec3 L) {
    vec4 lp = u_SunVP * vec4(v_WorldPos + N * u_SunTexel * 2.0, 1.0);
    vec3 p = lp.xyz / lp.w * 0.5 + 0.5;
    if (p.z > 1.0 || p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) return 0.0;

    float bias = max(u_SunBias * (1.0 - dot(N, L)), u_SunBias * 0.1);
    vec2 texel = 1.0 / vec2(textureSize(u_SunShadowMap, 0));

    float shadow = 0.0;
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            float closest = textureLod(u_SunShadowMap, p.xy + vec2(x, y) * texel, 0.0).r;
            shadow += (p.z - bias > closest) ? 1.0 : 0.0;
        }
    }
    return shadow / 9.0;
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

vec3 FresnelSchlick(float cosTheta, vec3 F0) {          // surfaces reflect more at grazing angles
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Cook-Torrance for one light. "radiance" is multiplied by PI so a light of intensity 1
// lights a white surface facing it to about 1.0, like the old shader did.
vec3 Shade(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float rough, vec3 F0,
           vec3 radiance, float shadow) {
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0) return vec3(0.0);

    vec3  H = normalize(V + L);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);

    float D = DistributionGGX(NdotH, rough * rough);
    float G = GeometrySmith(NdotV, NdotL, rough);
    vec3  F = FresnelSchlick(max(dot(H, V), 0.0), F0);

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
vec3 EnvBRDFApprox(vec3 F0, float rough, float NdotV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.040, -0.040);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    return F0 * AB.x + AB.y;
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
    EntityID = u_EntityID;

    // Picker colors are sRGB, albedo textures are decoded to linear by the GPU
    vec3 albedo = pow(u_Color, vec3(2.2));
    if (u_UseTexture == 1) albedo *= texture(u_Texture, v_TexCoord).rgb;

    if (u_Unlit == 1) {
        FragColor = vec4(albedo * kUnlitGlow, 1.0);
        return;
    }

    float metallic = u_Metallic;
    float roughness = u_Roughness;
    if (u_UseMetalRough == 1) {
        vec3 mr = texture(u_MetalRoughMap, v_TexCoord).rgb;   // glTF: G = roughness, B = metallic
        roughness *= mr.g;
        metallic  *= mr.b;
    }
    metallic  = clamp(metallic, 0.0, 1.0);
    roughness = clamp(roughness, 0.04, 1.0);                 // 0 would make an infinitely sharp highlight

    vec3 Ng = normalize(v_Normal);                           // geometric normal, used for shadows
    vec3 N = Ng;
    if (u_UseNormalMap == 1) {
        vec3 m = texture(u_NormalMap, v_TexCoord).xyz * 2.0 - 1.0;
        N = PerturbNormal(Ng, v_WorldPos, v_TexCoord, m);
    }

    vec3 V = normalize(u_ViewPos - v_WorldPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);             // 4% reflectance for non-metals

    vec3 color = vec3(0.0);

    for (int i = 0; i < u_PointCount; i++) {
        vec3 Lv = u_PointPos[i] - v_WorldPos;
        float dist = length(Lv);
        vec3 L = Lv / dist;
        float att = 1.0 / (1.0 + 0.02 * dist * dist);

        float shadow = (u_PointShadow[i] == 1 && dot(Ng, L) > 0.0) ? PointShadow(i, Ng, L) : 0.0;
        color += Shade(N, V, L, albedo, metallic, roughness, F0, u_PointColor[i] * att, shadow);
    }

    if (u_HasSun == 1) {
        vec3 L = normalize(-u_SunDir);
        float shadow = (u_SunShadow == 1 && dot(Ng, L) > 0.0) ? SunShadow(Ng, L) : 0.0;
        color += Shade(N, V, L, albedo, metallic, roughness, F0, u_SunColor, shadow);
    }

        // Ambient: diffuse from the irradiance map, reflections from the prefiltered map
    float NdotV = max(dot(N, V), 1e-4);
    vec3 specAmb = EnvBRDFApprox(F0, roughness, NdotV);
    vec3 R = reflect(-V, N);

    vec3 diffuseLight, specularLight;
    if (u_UseIBL == 1) {
        diffuseLight  = texture(u_IrradianceMap, EnvDir(N)).rgb;
        specularLight = textureLod(u_PrefilterMap, EnvDir(R), roughness * u_PrefilterMaxLod).rgb;
    } else {
        diffuseLight  = Env(N);
        specularLight = Env(normalize(mix(R, N, roughness * roughness)));
    }
    color += ((1.0 - metallic) * albedo * (1.0 - specAmb) * diffuseLight + specAmb * specularLight) * u_Ambient;

    vec3 emissive = u_Emissive;
    if (u_UseEmissive == 1) emissive *= texture(u_EmissiveMap, v_TexCoord).rgb;
    color += emissive;

    FragColor = vec4(color, 1.0);
}