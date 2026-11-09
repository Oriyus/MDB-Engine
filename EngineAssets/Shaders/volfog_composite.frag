#version 450 core
// Blends the half-resolution fog (volfog.frag) into the full-resolution image. Each pixel takes the four nearest
// fog texels, weighted by how close the depth each of them marched to is to this pixel's depth, so fog in front of
// a far wall doesn't bleed onto a near post (a joint bilateral upsample). Blending: ONE, SRC_ALPHA.

in vec2 v_UV;
layout(location = 0) out vec4 FragColor;

uniform sampler2D u_Fog;          // unit 10: rgb light, a transmittance (half resolution)
uniform sampler2D u_Depth;        // unit 9: scene depth (full resolution)
uniform mat4 u_InvProjection;

float ViewDistance(float d, vec2 uv) {
    vec4 v = u_InvProjection * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return length(v.xyz / v.w);
}

void main() {
    ivec2 full = textureSize(u_Depth, 0);
    ivec2 low = textureSize(u_Fog, 0);
    ivec2 px = ivec2(gl_FragCoord.xy);
    float here = ViewDistance(texelFetch(u_Depth, px, 0).r, (vec2(px) + 0.5) / vec2(full));

    vec2 lowPos = (vec2(px) + 0.5) * 0.5 - 0.5;
    ivec2 base = ivec2(floor(lowPos));
    vec2 f = lowPos - vec2(base);
    vec4 sum = vec4(0.0);
    float weights = 0.0;
    for (int y = 0; y <= 1; y++) {
        for (int x = 0; x <= 1; x++) {
            ivec2 t = clamp(base + ivec2(x, y), ivec2(0), low - 1);
            ivec2 tp = min(t * 2, full - 1);                       // the pixel that fog texel marched to
            float there = ViewDistance(texelFetch(u_Depth, tp, 0).r, (vec2(tp) + 0.5) / vec2(full));
            float bilinear = (x == 1 ? f.x : 1.0 - f.x) * (y == 1 ? f.y : 1.0 - f.y);
            float w = bilinear / (1e-3 + abs(there - here) / max(here, 0.1) * 40.0);
            sum += texelFetch(u_Fog, t, 0) * w;
            weights += w;
        }
    }
    FragColor = weights > 0.0 ? sum / weights : texelFetch(u_Fog, clamp(base, ivec2(0), low - 1), 0);
}
