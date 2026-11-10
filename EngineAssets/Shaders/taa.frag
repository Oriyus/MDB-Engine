#version 450 core

// Temporal anti-aliasing resolve (SceneRenderer, after the scene passes, before bloom and the tone map).
// Each frame is drawn with the projection shifted by a different sub-pixel offset (Halton 2,3). This pass blends
// the new frame into the history of earlier ones, reprojected through the motion vectors, so edges, thin leaves and
// noisy effects average out over a few frames. The history is clipped to the colour range of the current pixel's
// neighbourhood, which stops ghosting when something moves or appears.

in vec2 v_UV;
out vec4 FragColor;

uniform sampler2D u_Current;    // unit 0: this frame's HDR colour (jittered)
uniform sampler2D u_History;    // unit 1: last resolve (linear filtering)
uniform sampler2D u_Velocity;   // unit 2: motion vectors in UV (current - previous); kNoVelocity = none written
uniform sampler2D u_Depth;      // unit 3: this frame's depth
uniform mat4  u_InvViewProj;    // inverse of this frame's jittered view-projection (the depth was drawn with it)
uniform mat4  u_CurViewProj;    // this frame's, without jitter
uniform mat4  u_PrevViewProj;   // last frame's, without jitter
uniform int   u_HistoryValid;   // 0: first frame after a reset (size change, TAA just switched on)
uniform float u_Feedback;       // weight of the new frame (0.1 = 10%)
uniform int   u_ShowVelocity;   // debug view: motion vectors as colour

const float kNoVelocity = 5000.0;   // Framebuffer::kNoVelocity is 10000; anything above this means "not written"

float Luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
// Blend in a compressed space (Karis): a single very bright pixel can't dominate its neighbourhood or flicker
vec3 Compress(vec3 c)   { return c / (1.0 + Luma(c)); }
vec3 Uncompress(vec3 c) { return c / max(1.0 - Luma(c), 1e-4); }
vec3 ToYCoCg(vec3 c)   { return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25))); }
vec3 FromYCoCg(vec3 c) { return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// Catmull-Rom filtered history from 5 bilinear taps (sharper than one bilinear tap, which blurs over time)
vec3 SampleHistory(vec2 uv, vec2 size) {
    vec2 pos = uv * size;
    vec2 tc = floor(pos - 0.5) + 0.5;
    vec2 f = pos - tc;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 tc0 = (tc - 1.0) / size, tc3 = (tc + 2.0) / size, tc12 = (tc + w2 / w12) / size;
    vec3 c = texture(u_History, vec2(tc12.x, tc0.y)).rgb * (w12.x * w0.y)
           + texture(u_History, vec2(tc0.x, tc12.y)).rgb * (w0.x * w12.y)
           + texture(u_History, tc12).rgb * (w12.x * w12.y)
           + texture(u_History, vec2(tc3.x, tc12.y)).rgb * (w3.x * w12.y)
           + texture(u_History, vec2(tc12.x, tc3.y)).rgb * (w12.x * w3.y);
    float w = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(c / w, vec3(0.0));
}

void main() {
    ivec2 size = textureSize(u_Current, 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec2 texel = 1.0 / vec2(size);
    vec2 uv = (vec2(p) + 0.5) * texel;

    // The closest surface in the 3x3 neighbourhood gives the motion (edges move with the object in front)
    float bestDepth = 1.0;
    ivec2 bp = p;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 q = clamp(p + ivec2(x, y), ivec2(0), size - 1);
            float d = texelFetch(u_Depth, q, 0).r;
            if (d < bestDepth) { bestDepth = d; bp = q; }
        }
    vec2 velocity = texelFetch(u_Velocity, bp, 0).rg;
    if (velocity.x > kNoVelocity) {
        // Nothing wrote a motion vector (the sky, see-through parts): the camera's motion, from the depth
        vec2 buv = (vec2(bp) + 0.5) * texel;
        vec4 world = u_InvViewProj * vec4(buv * 2.0 - 1.0, bestDepth * 2.0 - 1.0, 1.0);
        world /= world.w;
        vec4 cur = u_CurViewProj * world, prev = u_PrevViewProj * world;
        velocity = (cur.xy / cur.w - prev.xy / prev.w) * 0.5;
    }
    if (any(isnan(velocity)) || any(isinf(velocity))) velocity = vec2(0.0);

    vec3 current = texelFetch(u_Current, p, 0).rgb;
    if (u_ShowVelocity == 1) {
        vec2 px = velocity * vec2(size);              // pixels moved since last frame: 16 px = full colour
        FragColor = vec4(min(abs(px) / 16.0, vec2(1.0)), 0.0, 1.0);
        return;
    }

    // The colour range of the neighbourhood (mean and spread, in compressed YCoCg)
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            vec3 c = ToYCoCg(Compress(texelFetch(u_Current, clamp(p + ivec2(x, y), ivec2(0), size - 1), 0).rgb));
            m1 += c;
            m2 += c * c;
        }
    m1 /= 9.0;
    vec3 sigma = sqrt(max(m2 / 9.0 - m1 * m1, vec3(0.0)));
    vec3 boxMin = m1 - 1.1 * sigma, boxMax = m1 + 1.1 * sigma;

    vec2 prevUV = uv - velocity;
    vec3 cur = ToYCoCg(Compress(current));
    if (u_HistoryValid == 0 || any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0)))) {
        FragColor = vec4(current, 1.0);          // nothing to blend with (first frame, or it came on screen)
        return;
    }
    vec3 hist = ToYCoCg(Compress(SampleHistory(prevUV, vec2(size))));

    // Clip the history towards the neighbourhood's mean until it fits the box (keeps its hue, unlike clamping)
    vec3 centre = 0.5 * (boxMax + boxMin), extent = 0.5 * (boxMax - boxMin) + 1e-5;
    vec3 offset = hist - centre;
    vec3 units = abs(offset / extent);
    float m = max(units.x, max(units.y, units.z));
    if (m > 1.0) hist = centre + offset / m;

    // A little more of the new frame where things move fast: less smearing, the clip does the rest
    float speed = length(velocity * vec2(size));
    float alpha = mix(u_Feedback, 0.25, clamp(speed / 30.0, 0.0, 1.0));
    vec3 result = Uncompress(FromYCoCg(mix(hist, cur, alpha)));
    if (any(isnan(result)) || any(isinf(result))) result = current;
    FragColor = vec4(max(result, vec3(0.0)), 1.0);
}
