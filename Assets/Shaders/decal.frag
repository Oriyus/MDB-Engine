#version 450 core
// Projects a decal onto the scene: each pixel's world position comes from a copy of the depth buffer;
// pixels inside the decal's box take its shape (circle, square or texture, filled or outlined).
// Fades out on steep surfaces (walls, step risers) and towards the top and bottom of the box.

struct DecalData {
    mat4 Model;
    mat4 InvModel;
    vec4 Color;
    vec4 Params;       // shape, thickness, fill, softness
    vec4 Extra;        // emission
};
layout(std430, binding = 3) readonly buffer Decals { DecalData u_Decals[]; };

flat in int v_Index;

uniform sampler2D u_Depth;         // unit 0: the scene depth (a copy: the real one is still attached)
uniform sampler2D u_Texture;       // unit 1
uniform int u_UseTexture;
uniform mat4 u_InvViewProjection;
uniform vec2 u_ViewSize;

layout(location = 0) out vec4 o_Color;

void main() {
    vec2 uv = gl_FragCoord.xy / u_ViewSize;
    float depth = texture(u_Depth, uv).r;
    vec4 p = u_InvViewProjection * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec3 world = p.xyz / p.w;
    // the surface's normal from neighbouring pixels (before any discard: derivatives need all of them)
    vec3 n = normalize(cross(dFdx(world), dFdy(world)));
    if (depth >= 1.0) discard;                     // the sky

    DecalData d = u_Decals[v_Index];
    vec3 local = (d.InvModel * vec4(world, 1.0)).xyz;
    if (any(greaterThan(abs(local), vec3(0.5)))) discard;

    vec3 up = normalize(vec3(d.Model[1]));
    float fade = smoothstep(0.3, 0.55, abs(dot(n, up))) * (1.0 - smoothstep(0.35, 0.5, abs(local.y)));

    int shape = int(d.Params.x + 0.5);
    float thick = d.Params.y, fill = d.Params.z, soft = max(d.Params.w, 0.001);
    vec3 color = pow(clamp(d.Color.rgb, 0.0, 1.0), vec3(2.2)) * d.Extra.x;   // sRGB picker colour -> linear
    vec2 st = local.xz * 2.0;                      // -1..1 across the decal
    float a;
    if (shape == 2) {
        vec4 t = texture(u_Texture, vec2(local.x + 0.5, 0.5 - local.z));
        a = t.a;
        color *= t.rgb;
    }
    else {
        float r = shape == 0 ? length(st) : max(abs(st.x), abs(st.y));
        float outer = 1.0 - smoothstep(1.0 - soft, 1.0, r);
        if (thick > 0.0) {
            float ring = smoothstep(1.0 - thick - soft, 1.0 - thick, r);
            a = outer * max(ring, fill);
        }
        else a = outer;
    }
    a *= d.Color.a * fade;
    if (a <= 0.002) discard;
    o_Color = vec4(color, a);
}
