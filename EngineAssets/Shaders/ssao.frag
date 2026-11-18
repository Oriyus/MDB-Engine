#version 450 core

in vec2 v_UV;
out vec2 FragAO;            // AO (1 = open), view depth (for the depth-aware blur)

uniform sampler2D u_Depth;
uniform mat4  u_Proj;
uniform mat4  u_InvProj;
uniform vec2  u_TargetSize; // this pass's size in pixels (half the depth's)
uniform float u_Radius;     // view-space units
uniform float u_Bias;
uniform int   u_Samples;
uniform float u_NoiseOffset;   // 0..1, a new value every frame while TAA is on (0 without TAA: a steady pattern)

const float PI = 3.14159265359;
// Occluders less than about 8.6 degrees above the surface's plane don't count (HBAO's angle bias): the shallow
// creases between the faces of a terrain or any faceted mesh (normals here come from depth, so per face) would
// otherwise each draw a dark line, stripes along the triangles.
const float kAngleBias = 0.15;

// Depth texels are read with texelFetch: this pass is half resolution, so its pixel centres fall exactly on the
// borders between depth texels, and a filtered or NEAREST read there picks either side depending on rounding,
// in bands across the screen (wrong normals, stripes).
ivec2 g_Size;

vec3 ViewPosAt(ivec2 p) {
    p = clamp(p, ivec2(0), g_Size - 1);
    float d = texelFetch(u_Depth, p, 0).r;
    vec2 uv = (vec2(p) + 0.5) / vec2(g_Size);
    vec4 v = u_InvProj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return v.xyz / v.w;
}

vec2 Hammersley(int i, int n) {
    return vec2(float(i) / float(n), float(bitfieldReverse(uint(i))) * 2.3283064365386963e-10);
}

void main() {
    g_Size = textureSize(u_Depth, 0);
    // the depth texel under this pixel's centre (the one whose depth the blur and the upsample compare against)
    ivec2 px = clamp(ivec2((floor(gl_FragCoord.xy) + 0.5) * vec2(g_Size) / u_TargetSize), ivec2(0), g_Size - 1);
    if (texelFetch(u_Depth, px, 0).r >= 0.99999) { FragAO = vec2(1.0, 1e6); return; }   // sky

    vec3 P  = ViewPosAt(px);
    vec3 Pl = ViewPosAt(px - ivec2(1, 0));
    vec3 Pr = ViewPosAt(px + ivec2(1, 0));
    vec3 Pd = ViewPosAt(px - ivec2(0, 1));
    vec3 Pu = ViewPosAt(px + ivec2(0, 1));

    // Use the closer neighbour on each axis, which avoids bad normals at silhouette edges
    vec3 dx = abs(Pr.z - P.z) < abs(P.z - Pl.z) ? Pr - P : P - Pl;
    vec3 dy = abs(Pu.z - P.z) < abs(P.z - Pd.z) ? Pu - P : P - Pd;
    vec3 N = normalize(cross(dx, dy));          // faces the camera (+Z in view space)

    // Random rotation per pixel, so the blur pass can average the pattern away
    // (with TAA the rotations change every frame, u_NoiseOffset, and the resolve averages them over frames)
    float rnd = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) + u_NoiseOffset);
    float ang = rnd * 2.0 * PI;
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);
    T = T * cos(ang) + B * sin(ang);
    B = cross(N, T);

    float occlusion = 0.0;
    for (int i = 0; i < u_Samples; i++) {
        vec2 h = Hammersley(i, u_Samples);
        float phi = 2.0 * PI * h.x;
        float cosT = sqrt(1.0 - h.y);
        float sinT = sqrt(h.y);
        vec3 s = vec3(cos(phi) * sinT, sin(phi) * sinT, cosT);          // hemisphere around N

        float t = (float(i) + 0.5) / float(u_Samples);
        float scale = mix(0.1, 1.0, t * t);                              // more samples close to the surface
        vec3 samplePos = P + (T * s.x + B * s.y + N * s.z) * (u_Radius * scale);

        vec4 o = u_Proj * vec4(samplePos, 1.0);
        vec2 uv = o.xy / o.w * 0.5 + 0.5;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) continue;

        vec3 Q = ViewPosAt(ivec2(uv * vec2(g_Size)));                     // the surface seen there
        if (Q.z < samplePos.z + u_Bias) continue;                        // the sample isn't behind it
        vec3 toQ = Q - P;
        if (dot(N, toQ) < kAngleBias * length(toQ)) continue;           // barely above this surface's plane
        // Ignore occluders far in front of or behind the sample (stops dark halos around objects)
        occlusion += smoothstep(0.0, 1.0, u_Radius / max(abs(P.z - Q.z), 1e-4));
    }
    FragAO = vec2(1.0 - occlusion / float(u_Samples), -P.z);
}
