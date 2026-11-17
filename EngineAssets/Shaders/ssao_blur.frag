#version 450 core
// SSAO's blur (half resolution): 5 x 5, each neighbour weighted by how close its depth is to this pixel's, so the
// darkening of a crevice doesn't bleed onto a surface in front of it or behind it (halos round silhouettes).
// In and out: (AO, view depth); the upsample (ao_apply.frag) weighs by the depth too.

in vec2 v_UV;
out vec2 FragAO;

uniform sampler2D u_AO;         // ssao.frag's raw AO and view depth

void main() {
    vec2 centre = texture(u_AO, v_UV).rg;
    float z0 = centre.y;
    if (z0 > 1e5) { FragAO = vec2(1.0, z0); return; }   // sky
    vec2 texel = 1.0 / vec2(textureSize(u_AO, 0));
    float tolerance = 0.04 * z0 + 0.02;                 // metres: a few percent of the distance
    float sum = 0.0, weights = 0.0;
    for (int x = -2; x <= 2; x++)
        for (int y = -2; y <= 2; y++) {
            vec2 s = texture(u_AO, v_UV + vec2(x, y) * texel).rg;
            float w = max(1.0 - abs(s.y - z0) / tolerance, 0.0);
            sum += s.x * w;
            weights += w;
        }
    FragAO = vec2(weights > 1e-4 ? sum / weights : centre.x, z0);
}
