#version 450 core
// Blends the half-resolution fog (volfog.frag) into the full-resolution image. Each fog texel holds the fog of its
// 2x2 block of pixels at the block's nearest and farthest depth; a pixel interpolates between the two by its own
// depth. Of the four nearest texels, those whose depth range holds this pixel's depth count most (its own block
// always does), so fog in front of a far wall doesn't bleed onto a near post and a thin branch against the sky
// gets the fog of its own depth (a joint bilateral upsample). Blending: ONE, SRC_ALPHA.

in vec2 v_UV;
layout(location = 0) out vec4 FragColor;

uniform sampler2D u_NearFog;      // unit 10: rgb light, a transmittance, to the block's nearest depth (half resolution)
uniform sampler2D u_FarFog;       // unit 11: the same to its farthest depth
uniform sampler2D u_Range;        // unit 12: those two distances
uniform sampler2D u_Depth;        // unit 9: scene depth (full resolution)
uniform mat4 u_InvProjection;
uniform float u_MaxDistance;

void main() {
    ivec2 full = textureSize(u_Depth, 0);
    ivec2 low = textureSize(u_NearFog, 0);
    ivec2 px = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(u_Depth, px, 0).r;
    float here = u_MaxDistance;                                   // as volfog.frag's FogDistance
    if (depth < 1.0) {
        vec2 uv = (vec2(px) + 0.5) / vec2(full);
        vec4 v = u_InvProjection * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
        here = min(length(v.xyz / v.w), u_MaxDistance);
    }

    vec2 lowPos = (vec2(px) + 0.5) * 0.5 - 0.5;
    ivec2 base = ivec2(floor(lowPos));
    vec2 f = lowPos - vec2(base);
    vec4 sum = vec4(0.0);
    float weights = 0.0;
    for (int y = 0; y <= 1; y++) {
        for (int x = 0; x <= 1; x++) {
            ivec2 t = clamp(base + ivec2(x, y), ivec2(0), low - 1);
            vec2 range = texelFetch(u_Range, t, 0).rg;
            float s = range.y - range.x > 1e-3 ? clamp((here - range.x) / (range.y - range.x), 0.0, 1.0) : 0.0;
            vec4 fog = mix(texelFetch(u_NearFog, t, 0), texelFetch(u_FarFog, t, 0), s);
            float miss = max(max(range.x - here, here - range.y), 0.0) / max(here, 0.1);
            float bilinear = (x == 1 ? f.x : 1.0 - f.x) * (y == 1 ? f.y : 1.0 - f.y);
            float w = bilinear / (1e-3 + miss * 40.0);
            sum += fog * w;
            weights += w;
        }
    }
    FragColor = sum / max(weights, 1e-8);
}
