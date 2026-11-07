#version 450 core

in vec2 v_UV;
out float FragAO;

uniform sampler2D u_Depth;
uniform mat4  u_Proj;
uniform mat4  u_InvProj;
uniform float u_Radius;     // view-space units
uniform float u_Bias;
uniform int   u_Samples;

const float PI = 3.14159265359;

// View-space position of the surface at a screen position
vec3 ViewPos(vec2 uv) {
    float d = texture(u_Depth, uv).r;
    vec4 p = u_InvProj * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    return p.xyz / p.w;
}

vec2 Hammersley(int i, int n) {
    return vec2(float(i) / float(n), float(bitfieldReverse(uint(i))) * 2.3283064365386963e-10);
}

void main() {
    if (texture(u_Depth, v_UV).r >= 0.99999) { FragAO = 1.0; return; }   // sky

    vec2 texel = 1.0 / vec2(textureSize(u_Depth, 0));
    vec3 P  = ViewPos(v_UV);
    vec3 Pl = ViewPos(v_UV - vec2(texel.x, 0.0));
    vec3 Pr = ViewPos(v_UV + vec2(texel.x, 0.0));
    vec3 Pd = ViewPos(v_UV - vec2(0.0, texel.y));
    vec3 Pu = ViewPos(v_UV + vec2(0.0, texel.y));

    // Use the closer neighbour on each axis, which avoids bad normals at silhouette edges
    vec3 dx = abs(Pr.z - P.z) < abs(P.z - Pl.z) ? Pr - P : P - Pl;
    vec3 dy = abs(Pu.z - P.z) < abs(P.z - Pd.z) ? Pu - P : P - Pd;
    vec3 N = normalize(cross(dx, dy));          // faces the camera (+Z in view space)

    // Random rotation per pixel, so the blur pass can average the pattern away
    float rnd = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
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

        float sceneZ = ViewPos(uv).z;
        // Ignore occluders far in front of or behind the sample (stops dark halos around objects)
        float range = smoothstep(0.0, 1.0, u_Radius / max(abs(P.z - sceneZ), 1e-4));
        occlusion += (sceneZ >= samplePos.z + u_Bias ? 1.0 : 0.0) * range;
    }
    FragAO = 1.0 - occlusion / float(u_Samples);
}