#version 450 core
// Fog of war, blended over the HDR scene: every pixel's world position comes from the depth buffer and
// its X/Z picks a texel of the fog map (R = visible now, G = explored; bilinear, so edges are soft).
// Never explored: covered by the hidden colour. Explored but not visible: covered partly by the explored
// colour. Visible: untouched. The sky is left alone.

in vec2 v_UV;

uniform sampler2D u_Depth;         // unit 0: a copy of the scene depth
uniform sampler2D u_Fog;           // unit 1
uniform mat4 u_InvViewProjection;
uniform vec2 u_FogOrigin;          // world X/Z of the map's corner
uniform vec2 u_FogSize;            // world size of the map
uniform vec3 u_HiddenColor;        // linear
uniform vec3 u_ExploredColor;      // linear
uniform float u_ExploredAlpha;

layout(location = 0) out vec4 o_Color;

void main() {
    float depth = texture(u_Depth, v_UV).r;
    if (depth >= 1.0) discard;
    vec4 p = u_InvViewProjection * vec4(v_UV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec3 world = p.xyz / p.w;
    vec2 f = texture(u_Fog, (world.xz - u_FogOrigin) / u_FogSize).rg;     // outside the map: the border (hidden)
    float visible = f.r, explored = f.g;
    float a = mix(1.0, u_ExploredAlpha, explored);
    a = mix(a, 0.0, visible);
    vec3 c = mix(u_HiddenColor, u_ExploredColor, explored);
    if (a <= 0.002) discard;
    o_Color = vec4(c, a);
}
