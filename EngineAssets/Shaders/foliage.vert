#version 450 core
// Grass and flower clumps (Renderer/FoliageRenderer), drawn with lit.frag: the same lighting, sun cascades,
// IBL and fog of war as every other surface. One instance per clump; the mesh is a clump of unit height.

layout(location = 0) in vec3 a_Position;   // x/z in metres around the clump centre, y 0..1 (scaled by the clump height)
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_TexCoord;   // x: height along the blade (0 root, 1 top: how much it bends), y: 1 on petals

struct Clump {
    vec4 PosRot;     // xyz root (world), w turn around Y (radians)
    vec4 Params;     // x height (m), y width scale, z sway phase, w rank 0..1 (distance thinning: high ranks go first)
    vec4 Color;      // rgb tint (multiplies the blade colours), a dryness 0..1
    vec4 Flower;     // rgb petal colour
};
layout(std430, binding = 4) readonly buffer Clumps { Clump u_Clumps[]; };
uniform int u_InstanceOffset;        // this draw's first clump (a chunk of the field)

uniform mat4  u_ViewProjection;
uniform vec3  u_ViewPos;
uniform float u_Time;
uniform vec2  u_WindDir;             // world X/Z, unit length
uniform float u_WindStrength;
uniform float u_WindSpeed;
uniform vec3  u_BaseColor, u_TipColor, u_DryColor;   // sRGB-style
uniform float u_Roughness;
uniform float u_FadeStart, u_FadeEnd;  // thinning by distance
uniform sampler2D u_Trample;         // unit 8: RG = world X/Z push (length 0..1), over the field's area
uniform mat4  u_FieldInvWorld;       // world -> the field entity's space
uniform vec2  u_FieldSize;

out vec3 v_WorldPos;
out vec3 v_Normal;
out vec2 v_TexCoord;
out vec4 v_Color;
flat out vec2  v_Surface;
flat out vec3  v_Emissive;
flat out int   v_EntityID;
flat out int   v_Unlit;

void main() {
    Clump c = u_Clumps[u_InstanceOffset + gl_InstanceID];
    vec3 root = c.PosRot.xyz;

    // Thin out with distance: of the clumps a chunk draws, the higher ranks shrink away first
    float keep = 1.0 - smoothstep(u_FadeStart, u_FadeEnd, distance(root, u_ViewPos));
    float grow = clamp((keep - c.Params.w) * 12.0, 0.0, 1.0);
    float height = c.Params.x * grow;

    float cr = cos(c.PosRot.w), sr = sin(c.PosRot.w);
    vec3 p = vec3(a_Position.x * c.Params.y, a_Position.y * height, a_Position.z * c.Params.y);
    p = vec3(cr * p.x + sr * p.z, p.y, -sr * p.x + cr * p.z);
    float h = a_TexCoord.x;
    float stiff = h * h;                     // roots stay put, tips move most

    // Wind: gusts rolling across the field (two waves along the wind, slower ones across it) and a flutter per clump
    float t = u_Time * u_WindSpeed;
    vec2 across = vec2(-u_WindDir.y, u_WindDir.x);
    float along = dot(root.xz, u_WindDir);
    float gust = sin(along * 0.23 - t * 1.7) * 0.5 + 0.5;
    gust *= 0.55 + 0.45 * sin(along * 0.061 + dot(root.xz, across) * 0.093 - t * 0.43);
    float flutter = sin(t * 3.9 + c.Params.z + h * 1.7) * 0.10 + sin(t * 7.1 + c.Params.z * 1.9) * 0.04;
    vec2 bend = u_WindDir * u_WindStrength * (0.12 + 0.6 * gust * gust + flutter);

    // Trampled: pushed away from whatever pressed it; a pressed clump stops swaying
    vec2 uv = (u_FieldInvWorld * vec4(root, 1.0)).xz / u_FieldSize + 0.5;
    vec2 push = texture(u_Trample, uv).rg;
    float pressed = min(length(push), 1.0);
    bend = bend * (1.0 - pressed) + push * 1.5;
    float b = length(bend);
    if (b > 1.4) { bend *= 1.4 / b; b = 1.4; }

    // Lean the blade over: sideways along the bend, lower so it keeps roughly its length
    vec3 world = root + p;
    world.xz += bend * stiff * height;
    world.y = root.y + p.y * max(0.15, 1.0 - 0.42 * b * b * h);

    v_WorldPos = world;
    // Lit mostly like the ground it covers (blade normals alone make a field look dark and noisy)
    vec3 n = vec3(cr * a_Normal.x + sr * a_Normal.z, a_Normal.y, -sr * a_Normal.x + cr * a_Normal.z);
    v_Normal = normalize(mix(n, vec3(0.0, 1.0, 0.0), 0.62) + vec3(bend.x, 0.0, bend.y) * 0.25);
    v_TexCoord = a_TexCoord;
    vec3 tip = mix(u_TipColor, u_DryColor, c.Color.a);
    vec3 col = mix(u_BaseColor, tip, smoothstep(0.0, 0.9, h)) * c.Color.rgb;
    if (a_TexCoord.y > 0.5) col = c.Flower.rgb;
    v_Color = vec4(col, 1.0);
    v_Surface = vec2(0.0, u_Roughness);
    v_Emissive = vec3(0.0);
    v_EntityID = -1;             // not written: the entity-ID target is masked, so clicks pick the ground under the grass
    v_Unlit = 0;
    gl_Position = u_ViewProjection * vec4(world, 1.0);
}
