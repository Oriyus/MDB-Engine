// Clustered point and spot lights (Renderer/LightClusters): every unshadowed light of the frame in one buffer, the view cut
// into 16 x 9 screen tiles x 24 depth slices, each cluster listing the lights whose bounds reach into it (binned by
// light_clusters.comp or on the CPU). Included
// by lit.frag and volfog.frag; the uniforms come from Scene::SetLightingUniforms.

struct PointLightGpu {
    vec4 PosRange;      // world position, range (metres: the light is 0 there)
    vec4 Color;         // linear colour x intensity, w = cos of the inner half angle (spot lights; points -1)
    vec4 Spot;          // the direction it shines (unit), w = cos of the outer half angle (points -2: every way)
    vec4 Shadow;        // x: its spot square in the shadow atlas, y: its first cube square (records in u_ShadowTiles; -1 = none), z: cookie layer, w: cookie roll
    vec4 Cookie;        // x: profile layer (-1 = none), y, z: the cookie's flipbook columns and rows, w: the frame shown
    vec4 Extra;         // x: the light's radius (metres; 0 = a point), y: contact shadows (1 / 0), z: shape (0 sphere, 1 tube, 2 rect), w: extent
    vec4 Area;          // an area light: its X axis x half the width (a tube's half length), w: a rect's half height
};
layout(std430, binding = 5) readonly buffer PointLights { PointLightGpu u_Lights[]; };
layout(std430, binding = 6) readonly buffer LightClusterCells { uvec2 u_ClusterCells[]; };   // first, count
layout(std430, binding = 7) readonly buffer LightClusterIndices { uint u_ClusterIndices[]; };

// The most useful shadowed lights (at most 4; their shadows in the atlas like the rest) are not in the clusters: they are shaded
// directly from these uniforms (cheaper for a handful). A light with range 0 is an unused slot.
#define MAX_POINT_LIGHTS 4
uniform int  u_PointCount;
uniform vec4 u_PointPosRange[MAX_POINT_LIGHTS];   // position, range
uniform vec4 u_PointColor[MAX_POINT_LIGHTS];      // colour, cos inner (as PointLightGpu)
uniform vec4 u_PointSpot[MAX_POINT_LIGHTS];       // direction, cos outer
uniform vec4 u_PointCookie[MAX_POINT_LIGHTS];     // cookie layer, roll, profile layer (-1 = none) (as PointLightGpu.Shadow.zw, Cookie.x)
uniform vec4 u_PointFrames[MAX_POINT_LIGHTS];     // flipbook columns, rows, frame (as PointLightGpu.Cookie.yzw)
uniform ivec4 u_PointShadow[MAX_POINT_LIGHTS];   // atlas records: x = a spot's square, y = a cube's first (-1 = none; as PointLightGpu.Shadow.xy)
uniform vec4 u_PointExtra[MAX_POINT_LIGHTS];      // as PointLightGpu.Extra
uniform vec4 u_PointArea[MAX_POINT_LIGHTS];       // as PointLightGpu.Area

const int CLUSTER_X = 16, CLUSTER_Y = 9, CLUSTER_Z = 24;
uniform int  u_ClusterLightCount;    // 0: no clustered lights at all (skip the lookups)
uniform vec2 u_ClusterZ;             // slice = log(view depth) * x + y
uniform vec4 u_ClusterViewRow;       // row 2 of the camera's view matrix: view depth = -dot(row, (p, 1))
uniform int  u_ClusterDebug;         // Viewport > Debug > Light Clusters

// The screen tile (0 .. CLUSTER_X * CLUSTER_Y - 1) of screenUV: 0..1 of the (unjittered) view, bottom left origin
int LightTile(vec2 screenUV) {
    int x = clamp(int(screenUV.x * float(CLUSTER_X)), 0, CLUSTER_X - 1);
    int y = clamp(int(screenUV.y * float(CLUSTER_Y)), 0, CLUSTER_Y - 1);
    return y * CLUSTER_X + x;
}
// The depth slice of a world position
int LightSlice(vec3 worldPos) {
    float depth = max(-dot(u_ClusterViewRow, vec4(worldPos, 1.0)), 1e-4);
    return clamp(int(floor(log(depth) * u_ClusterZ.x + u_ClusterZ.y)), 0, CLUSTER_Z - 1);
}
uvec2 LightCell(int tile, int slice) { return u_ClusterCells[slice * (CLUSTER_X * CLUSTER_Y) + tile]; }
// A pixel's cluster: (first index, light count)
uvec2 LightCluster(vec2 screenUV, vec3 worldPos) { return LightCell(LightTile(screenUV), LightSlice(worldPos)); }

// The engine's soft falloff, windowed to reach 0 at the range (the same as PointLightFalloff in LightClusters.cpp)
float LightWindow(float d, float range) {
    float q = d / range;
    q *= q; q *= q;
    float w = clamp(1.0 - q, 0.0, 1.0);
    return w * w;
}
float PointFalloff(float d, float range) { return LightWindow(d, range) / (1.0 + 0.02 * d * d); }

// Viewport > Debug > Light Clusters: how many lights a pixel weighs (blue 1, green 2, yellow 4, orange 8, red 16 and more)
vec3 ClusterHeat(uint n) {
    if (n == 0u) return vec3(0.02);
    float t = clamp(log2(float(n)) / 4.0, 0.0, 1.0);
    vec3 a = mix(vec3(0.1, 0.25, 1.0), vec3(0.1, 0.9, 0.2), clamp(t * 2.0, 0.0, 1.0));
    return mix(a, mix(vec3(1.0, 0.9, 0.1), vec3(1.0, 0.15, 0.05), clamp(t * 2.0 - 1.0, 0.0, 1.0)), step(0.5, t));
}

// Area lights (PointLightComponent::Shape): a tube (a segment along `area.xyz`, half length |area.xyz|) or a rect (half
// width |area.xyz| along it, half height area.w along Y = X x n, shining along n = the light's -Z, one way). Diffuse
// light comes from the shape's point closest to the surface; the highlight from its point nearest the reflection ray
// (Karis 2013, "representative point"), so a smooth floor mirrors a bar or a panel instead of a dot.
vec3 AreaClosest(vec3 c, vec3 n, vec4 area, float shape, vec3 p) {
    float hx = length(area.xyz);
    vec3 dx = area.xyz / max(hx, 1e-5);
    vec3 q = p - c;
    if (shape < 1.5) return c + dx * clamp(dot(q, dx), -hx, hx);
    vec3 dy = normalize(cross(dx, n));
    return c + dx * clamp(dot(q, dx), -hx, hx) + dy * clamp(dot(q, dy), -area.w, area.w);
}
vec3 AreaSpecularPoint(vec3 c, vec3 n, vec4 area, float shape, vec3 p, vec3 R, vec3 closest) {
    float hx = length(area.xyz);
    vec3 dx = area.xyz / max(hx, 1e-5);
    if (shape < 1.5) {
        // The segment's point nearest the ray p + R s
        vec3 L0 = c - dx * hx - p, Ld = dx * (2.0 * hx);
        float RoL0 = dot(R, L0), RoLd = dot(R, Ld), L0oLd = dot(L0, Ld), LdoLd = dot(Ld, Ld);
        float t = clamp((RoL0 * RoLd - L0oLd) / max(LdoLd - RoLd * RoLd, 1e-5), 0.0, 1.0);
        return p + L0 + Ld * t;
    }
    // A rect: where the ray meets its plane (from the lit side), clamped into it
    float denom = dot(R, n);
    if (denom > -1e-4) return closest;
    vec3 hit = p + R * (dot(c - p, n) / denom);
    vec3 dy = normalize(cross(dx, n));
    vec3 q = hit - c;
    return c + dx * clamp(dot(q, dx), -hx, hx) + dy * clamp(dot(q, dy), -area.w, area.w);
}
// For a light at c: Lv = to the point diffuse light comes from (its length = the distance for the falloff), Ls = unit,
// towards the highlight's point. R: the view direction reflected about the normal (unused by plain lights)
void LightVectors(vec3 c, vec4 spot, vec4 area, vec4 extra, vec3 p, vec3 R, out vec3 Lv, out vec3 Ls) {
    if (extra.z < 0.5) { Lv = c - p; Ls = normalize(Lv); return; }
    vec3 pd = AreaClosest(c, spot.xyz, area, extra.z, p);
    Lv = pd - p;
    Ls = normalize(AreaSpecularPoint(c, spot.xyz, area, extra.z, p, R, pd) - p);
}
// A rect shines one way, brightest straight ahead (a Lambertian panel); L: from the surface towards the light
float AreaEmission(vec4 spot, vec4 extra, vec3 L) { return extra.z > 1.5 ? max(dot(-L, spot.xyz), 0.0) : 1.0; }
// How soft an area light's shadows are: as a ball of half its size (or its own radius if bigger)
float ShadowRadius(vec4 extra) { return extra.z > 0.5 ? max(extra.x, extra.w * 0.5) : extra.x; }

// A spot light's cone (1 inside the inner angle, 0 outside the outer one; point lights always 1). L: towards the light
float SpotCone(vec3 L, vec4 spot, float cosInner) {
    if (spot.w < -1.5) return 1.0;
    return smoothstep(spot.w, cosInner, dot(-L, spot.xyz));
}

// The shadow atlas (Renderer/ShadowAtlas): every shadowed light has squares in one 16-bit depth texture storing
// distance / far plane, compared in hardware. Lights carry their records' index: Shadow.x = a spot's one square,
// Shadow.y = the first of a cube's six (+X, -X, +Y, -Y, +Z, -Z; a face without a square has Rect.z 0). A cube face is
// drawn a few texels wider than 90 degrees (AtlasFaceMatrix), so filtering never leaves it.
struct ShadowTile { mat4 ViewProj; vec4 Rect; };    // Rect: corner x, y and size (0..1 of the atlas), far plane
layout(std430, binding = 9) readonly buffer ShadowTiles { ShadowTile u_ShadowTiles[]; };
uniform sampler2DShadow u_ShadowAtlas;     // unit 18
uniform sampler2D u_ShadowAtlasRaw;        // unit 19: the same texture, plain depth (the blocker search)
uniform int   u_AtlasOn;
uniform float u_AtlasBias;                 // world units (Scene Settings > Shadows > point bias)
uniform float u_AtlasTexel;                // 1 / the atlas's size
uniform float u_ShadowFrame;               // 0..63, turns the soft-shadow pattern each frame (TAA averages it)

// Interleaved gradient noise (0..1), different each frame
float ShadowNoise() {
    vec2 q = gl_FragCoord.xy + 5.588238 * u_ShadowFrame;
    return fract(52.9829189 * fract(dot(q, vec2(0.06711056, 0.00583715))));
}
// Point k of n spread evenly over the unit disc (a Vogel spiral)
vec2 VogelDisk(int k, int n) {
    float r = sqrt((float(k) + 0.5) / float(n)), a = float(k) * 2.3999632;
    return r * vec2(cos(a), sin(a));
}

// How much light reaches p through an atlas square (1 = lit), once p is placed in it: rect = ShadowTile.Rect, uv = 0..1
// across it, tanHalf = tan of the view's half angle. dist: from the light; taps = 1 (fog) or 4; radius: the
// light's size (> 0 with taps > 1: shadows soften with the gap between the casters and p, PCSS)
float AtlasLitUV(vec4 rect, vec2 uv, float tanHalf, float dist, float NdotL, int taps, float radius) {
    float texels = rect.z / u_AtlasTexel;
    if (taps <= 1) {             // the fog: one tap, a plain bias (no surface to slope)
        float h1 = 1.5 / texels;
        return texture(u_ShadowAtlas, vec3(rect.xy + clamp(uv, vec2(h1), vec2(1.0 - h1)) * rect.z,
                                           (dist - u_AtlasBias - 2.0 * dist / texels) / rect.w));
    }
    float toWorld = 2.0 * dist * tanHalf;                      // uv -> world units at p
    // Bias: the light's setting, more on slopes, plus about a texel's size at that distance (small squares are coarse)
    float slope = 1.0 - NdotL;
    float bias = max(u_AtlasBias * 2.0 * slope, u_AtlasBias * 0.5) + toWorld / texels * (0.75 + 1.5 * slope);
    float ref = (dist - bias) / rect.w;
    float h = 1.5 / texels;                                    // keep every 2 x 2 fetch inside the square
    vec2 lo = vec2(h), hi = vec2(1.0 - h);
    if (radius > 0.0) {
        float a = ShadowNoise() * 6.2831853;
        mat2 turn = mat2(cos(a), sin(a), -sin(a), cos(a));
        // Blocker search: the casters between p and the light's disc lie within about radius / distance of p's spot
        float search = clamp(radius / (tanHalf * dist), 2.0 / texels, 0.25);
        float sum = 0.0, n = 0.0;
        for (int k = 0; k < 8; k++) {
            vec2 q = clamp(uv + turn * VogelDisk(k, 8) * search, lo, hi);
            float d = textureLod(u_ShadowAtlasRaw, rect.xy + q * rect.z, 0.0).r * rect.w;
            if (d < dist - bias) { sum += d; n += 1.0; }
        }
        if (n == 0.0) return 1.0;                              // nothing in the way
        float blocker = max(sum / n, 0.02);
        // The penumbra at p grows with the gap: radius x (receiver - blocker) / blocker (world), as uv here
        float spread = clamp(radius * (dist - blocker) / blocker / toWorld, 0.75 / texels, 0.25);
        // A wide filter reaches past the surface's own plane on slopes: more bias for it
        float spreadBias = spread * toWorld * min(sqrt(max(1.0 - NdotL * NdotL, 0.0)) / max(NdotL, 0.2), 3.0);
        float refSoft = (dist - bias - spreadBias) / rect.w;
        float lit = 0.0;
        for (int k = 0; k < 12; k++)
            lit += texture(u_ShadowAtlas, vec3(rect.xy + clamp(uv + turn * VogelDisk(k, 12) * spread, lo, hi) * rect.z, refSoft));
        return lit / 12.0;
    }
    vec2 o = vec2(0.75 / texels);
    float lit = 0.0;
    for (int k = 0; k < 4; k++) {
        vec2 q = uv + o * vec2((k & 1) == 0 ? -1.0 : 1.0, k < 2 ? -1.0 : 1.0);
        lit += texture(u_ShadowAtlas, vec3(rect.xy + clamp(q, lo, hi) * rect.z, ref));
    }
    return lit * 0.25;
}
// A spot light's square: placed through the view-projection it was drawn with
float AtlasSpotLit(int t, vec3 p, float NdotL, float dist, int taps, float radius) {
    if (u_AtlasOn != 1 || t < 0) return 1.0;
    ShadowTile st = u_ShadowTiles[t];
    if (st.Rect.z <= 0.0 || dist >= st.Rect.w) return 1.0;
    vec4 c = st.ViewProj * vec4(p, 1.0);
    if (c.w <= 0.0) return 1.0;
    // tan of the view's half angle (row 1 of view-projection = row 1 of the projection, turned)
    float tanHalf = 1.0 / length(vec3(st.ViewProj[0][1], st.ViewProj[1][1], st.ViewProj[2][1]));
    return AtlasLitUV(st.Rect, c.xy / c.w * 0.5 + 0.5, tanHalf, dist, NdotL, taps, radius);
}
// A cube's six squares: the face the light-to-p direction points into, placed without its matrix (only the square's
// Rect is read): the face's right / up axes as glm::lookAt builds them for AtlasFaceMatrix (written out per face),
// over the 90 degrees plus 3 texels of guard band each side
float AtlasCubeLit(int first, vec3 lightPos, vec3 p, float NdotL, int taps, float radius) {
    if (u_AtlasOn != 1 || first < 0) return 1.0;
    vec3 v = p - lightPos, a = abs(v);
    int face;
    vec2 ndc;          // (right, up) over forward: +X (-z, -y), -X (z, -y), +Y (x, z), -Y (x, -z), +Z (x, -y), -Z (-x, -y)
    if (a.x >= a.y && a.x >= a.z) { face = v.x > 0.0 ? 0 : 1; ndc = vec2(v.x > 0.0 ? -v.z : v.z, -v.y) / a.x; }
    else if (a.y >= a.z)          { face = v.y > 0.0 ? 2 : 3; ndc = vec2(v.x, v.y > 0.0 ? v.z : -v.z) / a.y; }
    else                          { face = v.z > 0.0 ? 4 : 5; ndc = vec2(v.z > 0.0 ? v.x : -v.x, -v.y) / a.z; }
    vec4 rect = u_ShadowTiles[first + face].Rect;
    float dist = length(v);
    if (rect.z <= 0.0 || dist >= rect.w) return 1.0;
    float texels = rect.z / u_AtlasTexel;
    float tanHalf = texels / max(texels - 6.0, 1.0);
    ndc /= tanHalf;
    return AtlasLitUV(rect, ndc * 0.5 + 0.5, tanHalf, dist, NdotL, taps, radius);
}

// Contact shadows: a short ray from the surface towards the light through the depth of the scene (the depth pre-pass,
// copied by SceneRenderer; the camera's jittered view-projection it was drawn with). Clip w is the view depth.
uniform sampler2D u_ContactDepth;          // unit 1
uniform float u_ContactLength;             // metres (0 = off: ShadowSettings::ContactLength)
uniform mat4  u_ContactVP;
uniform vec2  u_ContactNearFar;

float ContactViewDepth(float d01) {
    float n = u_ContactNearFar.x, f = u_ContactNearFar.y, z = d01 * 2.0 - 1.0;
    return 2.0 * n * f / (f + n - z * (f - n));
}
// 1 = lit, 0 = something on screen lies across the first u_ContactLength metres towards the light (L, unit; the
// light lightDist away). Ng: the geometric normal (the ray starts a little off the surface). 6 steps, jittered per
// pixel and frame (TAA smooths it); a hit far behind the depth seen there is something in front, not in the way
float ContactLit(vec3 p, vec3 Ng, vec3 L, float lightDist) {
    float len = min(u_ContactLength, lightDist * 0.8);
    if (len <= 0.0) return 1.0;
    const int STEPS = 6;
    float jitter = ShadowNoise();
    float viewDepth = (u_ContactVP * vec4(p, 1.0)).w;
    vec3 start = p + Ng * (0.004 + 0.0015 * viewDepth);
    float thickness = max(0.15, len) + 0.01 * viewDepth;
    // The ray's ends in clip space: points along a straight segment interpolate linearly there (before the divide)
    vec4 c0 = u_ContactVP * vec4(start, 1.0), c1 = u_ContactVP * vec4(start + L * len, 1.0);
    for (int k = 0; k < STEPS; k++) {
        float f = (float(k) + jitter) / float(STEPS);
        vec4 c = mix(c0, c1, f);
        if (c.w <= 0.0) return 1.0;
        vec2 ndc = c.xy / c.w;
        if (abs(ndc.x) > 1.0 || abs(ndc.y) > 1.0) return 1.0;
        float diff = c.w - ContactViewDepth(textureLod(u_ContactDepth, ndc * 0.5 + 0.5, 0.0).r);
        if (diff > 0.003 * c.w && diff < thickness) return smoothstep(0.6, 1.0, f);   // fades out at the ray's end
    }
    return 1.0;
}

// A light's size as an angle at a surface: GGX roughness (alpha) widens by radius / (2 distance), so a big light up
// close leaves a broad highlight instead of a pin-point (alpha is roughness squared; the lobe stays normalised)
float SizedRoughness(float rough, float radius, float dist) {
    if (radius <= 0.0) return rough;
    return sqrt(clamp(rough * rough + radius / (2.0 * max(dist, 1e-3)), 0.0, 1.0));
}


// Cookies and profiles (LightClusters): images a light shines through, in one sRGB array (unit 20, 512 x 512 layers).
// A spot projects its cookie along the cone (a gobo); a point light wraps it round itself as a panorama (its middle
// along the light's aim, Spot.xyz = the entity's -Z, the top along the entity's +Y). A flipbook cookie is a grid of
// frames (left to right, top row first). A profile (an IES file, IesProfile::Bake) is brightness over the angle from
// the aim (v) and round it (u). The frame (r, u, d) is CookieBasis (LightClusters.cpp) turned by the roll.
uniform sampler2DArray u_LightCookies;    // unit 20, sRGB (read as linear light)
const float COOKIE_PI = 3.14159265;

// What multiplies the light leaving towards -L. spot: aim, cos outer (points < -1.5); ck: cookie layer, roll, profile
// layer; frames: columns, rows, frame. footprint: the size of the pixel at the light's distance over that distance
// (radians; picks the mip level: lit.frag passes about 1.5 mrad x view distance / light distance)
vec3 LightCookie(vec4 spot, vec3 ck, vec3 frames, vec3 L, float footprint) {
    if (ck.x < 0.0 && ck.z < 0.0) return vec3(1.0);
    vec3 d = spot.xyz;
    vec3 ref = abs(d.y) > 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(d, ref)), up = cross(right, d);
    float cr = cos(ck.y), sr = sin(ck.y);
    vec3 r = right * cr + up * sr, u = up * cr - right * sr;
    vec3 o = -L;
    vec3 local = vec3(dot(o, r), dot(o, u), dot(o, d));
    vec3 result = vec3(1.0);
    if (ck.x >= 0.0) {
        vec2 uv;
        float texelsPerRadian;
        if (spot.w > -1.5) {              // a spot: the plane across the cone (any width below 180 degrees)
            if (local.z <= 1e-4) return vec3(0.0);
            float c = spot.w, tanHalf = sqrt(max(1.0 - c * c, 1e-6)) / max(c, 1e-4);
            uv = local.xy / (local.z * tanHalf) * 0.5 + 0.5;
            texelsPerRadian = 512.0 / (2.0 * tanHalf);
        } else {                          // a point: latitude / longitude round the light
            float lon = (abs(local.x) + abs(local.z) > 1e-6) ? atan(local.x, local.z) : 0.0;
            uv = vec2(0.5 + lon / (2.0 * COOKIE_PI), 0.5 + asin(clamp(local.y, -1.0, 1.0)) / COOKIE_PI);
            texelsPerRadian = 512.0 / COOKIE_PI;
        }
        float cols = max(frames.x, 1.0), rows = max(frames.y, 1.0), cells = max(cols, rows);
        float lod = log2(max(footprint * texelsPerRadian / cells, 1.0));
        if (cols * rows > 1.0) {
            float h = 0.5 * exp2(lod) / (512.0 / cells);    // keep the filter inside the frame
            uv = clamp(uv, vec2(h), vec2(1.0 - h));
            float col = mod(frames.z, cols), row = floor(frames.z / cols);
            uv = (vec2(col, rows - 1.0 - row) + uv) / vec2(cols, rows);
        }
        result = textureLod(u_LightCookies, vec3(uv, ck.x), lod).rgb;
    }
    if (ck.z >= 0.0) {
        float theta = acos(clamp(local.z, -1.0, 1.0));
        float phi = (abs(local.x) + abs(local.y) > 1e-6) ? atan(local.y, local.x) : 0.0;
        if (phi < 0.0) phi += 2.0 * COOKIE_PI;
        result *= textureLod(u_LightCookies, vec3(phi / (2.0 * COOKIE_PI), theta / COOKIE_PI, ck.z), 0.0).r;
    }
    return result;
}
vec3 LightCookie(PointLightGpu pl, vec3 L, float footprint) {
    return LightCookie(pl.Spot, vec3(pl.Shadow.zw, pl.Cookie.x), pl.Cookie.yzw, L, footprint);
}
