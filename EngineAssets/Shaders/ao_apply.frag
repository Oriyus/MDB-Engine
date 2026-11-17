#version 450 core
// Ambient occlusion onto the opaque scene (SceneRenderer, right after the opaque passes): multiplied into the HDR
// colour with blending (DST_COLOR, ZERO). Done before the sky, glass, volumetric fog and particles, so those are not
// darkened, and before the TAA resolve, which averages away AO's noise.
// The AO is half resolution: each pixel takes the 4 nearest AO texels weighted by distance (bilinear) and by how close
// their depth is to its own, so an edge doesn't borrow the AO of the surface behind or in front of it.

in vec2 v_UV;
layout(location = 0) out vec4 FragColor;

uniform sampler2D u_AO;          // (AO: 1 = open, 0 = fully occluded; view depth), half resolution, blurred
uniform sampler2D u_Depth;       // a copy of the full-resolution scene depth
uniform mat4  u_InvProj;
uniform float u_AOStrength;

void main() {
    float d = textureLod(u_Depth, v_UV, 0.0).r;
    vec4 p = u_InvProj * vec4(0.0, 0.0, d * 2.0 - 1.0, 1.0);
    float z0 = -p.z / p.w;
    vec2 size = vec2(textureSize(u_AO, 0));
    vec2 f = v_UV * size - 0.5;
    vec2 base = floor(f), t = f - base;
    float tolerance = 0.02 * z0 + 0.01;
    float sum = 0.0, weights = 0.0;
    for (int k = 0; k < 4; k++) {
        vec2 c = vec2(k & 1, k >> 1);
        vec2 s = texelFetch(u_AO, clamp(ivec2(base + c), ivec2(0), ivec2(size) - 1), 0).rg;
        float bilinear = (c.x > 0.5 ? t.x : 1.0 - t.x) * (c.y > 0.5 ? t.y : 1.0 - t.y);
        float w = (bilinear + 1e-3) / (1.0 + abs(s.y - z0) / tolerance);
        sum += s.x * w;
        weights += w;
    }
    const float ao = mix(1.0, sum / weights, u_AOStrength);
    FragColor = vec4(vec3(ao), 1.0);
}
