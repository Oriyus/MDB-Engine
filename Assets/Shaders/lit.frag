#version 450 core

in vec3 v_WorldPos;
in vec3 v_Normal;
in vec2 v_TexCoord;

layout(location = 0) out vec4 FragColor;
layout(location = 1) out int  EntityID;

#define MAX_POINT_LIGHTS 4

uniform sampler2D        u_Texture;          // unit 0
uniform samplerCubeArray u_PointShadowMaps;  // unit 1
uniform sampler2D        u_SunShadowMap;     // unit 2

uniform int  u_UseTexture;
uniform int  u_Unlit;
uniform int  u_EntityID;
uniform vec3 u_Color;

uniform vec3  u_ViewPos;
uniform float u_Shininess;
uniform float u_SpecularStrength;

// Point lights (color already includes intensity)
uniform int   u_PointCount;
uniform vec3  u_PointPos[MAX_POINT_LIGHTS];
uniform vec3  u_PointColor[MAX_POINT_LIGHTS];
uniform int   u_PointShadow[MAX_POINT_LIGHTS];   // 1 = this light has a shadow layer
uniform float u_PointFar;
uniform float u_PointBias;

// Sun
uniform int   u_HasSun;
uniform vec3  u_SunDir;       // direction the light travels
uniform vec3  u_SunColor;
uniform int   u_SunShadow;
uniform mat4  u_SunVP;
uniform float u_SunBias;
uniform float u_SunTexel;     // size of one shadow texel in world units

const vec3 kOffsets[20] = vec3[](
    vec3( 1,  1,  1), vec3( 1, -1,  1), vec3(-1, -1,  1), vec3(-1,  1,  1),
    vec3( 1,  1, -1), vec3( 1, -1, -1), vec3(-1, -1, -1), vec3(-1,  1, -1),
    vec3( 1,  1,  0), vec3( 1, -1,  0), vec3(-1, -1,  0), vec3(-1,  1,  0),
    vec3( 1,  0,  1), vec3(-1,  0,  1), vec3( 1,  0, -1), vec3(-1,  0, -1),
    vec3( 0,  1,  1), vec3( 0, -1,  1), vec3( 0, -1, -1), vec3( 0,  1, -1)
);

// 0 = lit, 1 = in shadow
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
    // Push the point slightly along the normal to avoid self-shadowing on slopes
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

// Blinn-Phong for one light direction
vec3 Shade(vec3 N, vec3 V, vec3 L, vec3 base, vec3 radiance, float shadow) {
    float diff = max(dot(N, L), 0.0);
    vec3  H = normalize(L + V);
    float spec = diff > 0.0 ? pow(max(dot(N, H), 0.0), u_Shininess) * u_SpecularStrength : 0.0;
    return (1.0 - shadow) * (diff * base + spec) * radiance;
}

void main() {
    EntityID = u_EntityID;

    vec3 base = pow(u_Color, vec3(2.2));   // picker colors are sRGB; lighting needs linear
    if (u_UseTexture == 1) base *= texture(u_Texture, v_TexCoord).rgb;

    if (u_Unlit == 1) {
        FragColor = vec4(base, 1.0);
        return;
    }

    vec3 N = normalize(v_Normal);
    vec3 V = normalize(u_ViewPos - v_WorldPos);

    vec3 color = 0.03 * base;   // ambient, never shadowed

    for (int i = 0; i < u_PointCount; i++) {
        vec3 Lv = u_PointPos[i] - v_WorldPos;
        float dist = length(Lv);
        vec3 L = Lv / dist;
        float att = 1.0 / (1.0 + 0.02 * dist * dist);

        float shadow = (u_PointShadow[i] == 1 && dot(N, L) > 0.0) ? PointShadow(i, N, L) : 0.0;
        color += Shade(N, V, L, base, u_PointColor[i] * att, shadow);
    }

    if (u_HasSun == 1) {
        vec3 L = normalize(-u_SunDir);
        float shadow = (u_SunShadow == 1 && dot(N, L) > 0.0) ? SunShadow(N, L) : 0.0;
        color += Shade(N, V, L, base, u_SunColor, shadow);
    }

    FragColor = vec4(color, 1.0);
}