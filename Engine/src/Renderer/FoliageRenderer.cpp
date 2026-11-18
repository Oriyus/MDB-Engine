#include "Renderer/FoliageRenderer.h"
#include "Core/Parallel.h"
#include "Renderer/Mesh.h"
#include "Renderer/Shader.h"
#include "Renderer/Camera.h"
#include "Renderer/RenderStats.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Scene/Terrain.h"
#include "Core/Profiler.h"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace Engine {

    namespace {
        constexpr float kChunk = 8.0f;          // metres: frustum culling granularity (one draw per chunk and kind)
        constexpr float kTexel = 0.2f;          // trample map resolution (metres per texel)
        constexpr int   kMaxTrample = 256;      // texels per side at most
        constexpr size_t kMaxClumps = 400000;   // per field

        // Matches foliage.vert's Clump (std430: four vec4s)
        struct Clump { glm::vec4 PosRot, Params, Color, Flower; };
        static_assert(sizeof(Clump) == 64, "Clump must match foliage.vert");

        struct Rng {                             // xorshift64*: small, fast and the same everywhere
            uint64_t S;
            explicit Rng(uint64_t seed) : S(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) { if (!S) S = 1; }
            uint32_t Next() { S ^= S >> 12; S ^= S << 25; S ^= S >> 27; return (uint32_t)((S * 0x2545F4914F6CDD1Dull) >> 32); }
            float F() { return (Next() >> 8) * (1.0f / 16777216.0f); }            // 0..1
            float R(float a, float b) { return a + (b - a) * F(); }
        };

        float Hash2(int x, int y, uint32_t seed) {
            uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
            h = (h ^ (h >> 13)) * 1274126177u;
            return ((h ^ (h >> 16)) & 0xFFFFFFu) / 16777216.0f;
        }
        float ValueNoise(glm::vec2 p, uint32_t seed) {         // smooth 0..1 noise, one feature per unit
            const glm::vec2 i = glm::floor(p), f = p - i;
            const glm::vec2 u = f * f * (3.0f - 2.0f * f);
            const int x = (int)i.x, y = (int)i.y;
            const float a = Hash2(x, y, seed), b = Hash2(x + 1, y, seed), c = Hash2(x, y + 1, seed), d = Hash2(x + 1, y + 1, seed);
            return glm::mix(glm::mix(a, b, u.x), glm::mix(c, d, u.x), u.y);
        }

        // One tapering blade: `segments` quads and a tip. x/z in metres, y 0..height (of a unit-height clump);
        // TexCoord.x = y (how far up the clump: how much it bends), TexCoord.y = petal flag
        void AddBlade(std::vector<Vertex>& v, std::vector<uint32_t>& idx, glm::vec2 base, float facing, glm::vec2 lean,
                      float height, float width, int segments) {
            const glm::vec3 side(std::cos(facing), 0.0f, std::sin(facing));
            const glm::vec3 n = glm::normalize(glm::vec3(-side.z, 0.35f, side.x));
            const uint32_t first = (uint32_t)v.size();
            for (int k = 0; k < segments; k++) {
                const float t = float(k) / segments;
                const glm::vec3 c(base.x + lean.x * t * t, height * t, base.y + lean.y * t * t);
                const float hw = width * 0.5f * (1.0f - 0.75f * t);
                v.push_back({ c - side * hw, n, { height * t, 0.0f } });
                v.push_back({ c + side * hw, n, { height * t, 0.0f } });
            }
            v.push_back({ { base.x + lean.x, height, base.y + lean.y }, n, { height, 0.0f } });
            for (int k = 0; k < segments - 1; k++) {
                const uint32_t a = first + 2 * k;
                idx.insert(idx.end(), { a, a + 1, a + 3, a, a + 3, a + 2 });
            }
            const uint32_t a = first + 2 * (segments - 1);
            idx.insert(idx.end(), { a, a + 1, a + 2 });
        }

        std::shared_ptr<Mesh> MakeGrass() {
            Rng rng(11);
            std::vector<Vertex> v;
            std::vector<uint32_t> idx;
            for (int b = 0; b < 9; b++) {
                const float a = rng.R(0.0f, 6.2832f), r = 0.09f * std::sqrt(rng.F());
                const glm::vec2 out(std::cos(a), std::sin(a));
                AddBlade(v, idx, out * r, rng.R(0.0f, 6.2832f), out * rng.R(0.05f, 0.28f) + glm::vec2(rng.R(-0.05f, 0.05f), rng.R(-0.05f, 0.05f)),
                         rng.R(0.55f, 1.0f), rng.R(0.03f, 0.05f), 4);
            }
            return std::make_shared<Mesh>(v, idx);
        }

        std::shared_ptr<Mesh> MakeFlower() {
            Rng rng(23);
            std::vector<Vertex> v;
            std::vector<uint32_t> idx;
            const glm::vec2 lean(0.05f, 0.02f);
            const float stem = 0.95f;
            AddBlade(v, idx, glm::vec2(0.0f), 0.4f, lean, stem, 0.014f, 3);
            for (int b = 0; b < 4; b++) {                          // leaves and a little grass around it
                const float a = rng.R(0.0f, 6.2832f);
                const glm::vec2 out(std::cos(a), std::sin(a));
                AddBlade(v, idx, out * 0.03f, a + 1.57f, out * rng.R(0.12f, 0.22f), rng.R(0.3f, 0.55f), rng.R(0.03f, 0.045f), 3);
            }
            // the head: a ring of petals around the top of the stem, facing up
            const glm::vec3 c(lean.x, stem, lean.y);
            const uint32_t centre = (uint32_t)v.size();
            v.push_back({ c + glm::vec3(0.0f, 0.012f, 0.0f), glm::vec3(0, 1, 0), { stem, 1.0f } });
            const int petals = 7;
            for (int i = 0; i <= petals; i++) {
                const float a = 6.2832f * i / petals;
                const float r = (i % 2) ? 0.065f : 0.08f;
                v.push_back({ c + glm::vec3(std::cos(a) * r, 0.02f, std::sin(a) * r), glm::vec3(0, 1, 0), { stem, 1.0f } });
            }
            for (int i = 0; i < petals; i++) idx.insert(idx.end(), { centre, centre + 1 + i, centre + 2 + i });
            return std::make_shared<Mesh>(v, idx);
        }

        struct Box { glm::vec3 Min, Max; };

        // Frustum test on a world-space box (see Scene.cpp's Frustum)
        struct Frustum {
            glm::vec4 Planes[6];
            explicit Frustum(const glm::mat4& vp) {
                const glm::mat4 m = glm::transpose(vp);
                Planes[0] = m[3] + m[0]; Planes[1] = m[3] - m[0];
                Planes[2] = m[3] + m[1]; Planes[3] = m[3] - m[1];
                Planes[4] = m[3] + m[2]; Planes[5] = m[3] - m[2];
                for (auto& p : Planes) p /= glm::length(glm::vec3(p));
            }
            bool Visible(const glm::vec3& mn, const glm::vec3& mx) const {
                const glm::vec3 c = (mn + mx) * 0.5f, e = (mx - mn) * 0.5f;
                for (const auto& p : Planes)
                    if (glm::dot(glm::vec3(p), c) + glm::dot(glm::abs(glm::vec3(p)), e) + p.w < 0.0f) return false;
                return true;
            }
        };

        struct Hasher {
            uint64_t H = 1469598103934665603ull;
            template <typename T> void Add(const T& value) {
                const auto* p = reinterpret_cast<const unsigned char*>(&value);
                for (size_t i = 0; i < sizeof(T); i++) { H ^= p[i]; H *= 1099511628211ull; }
            }
        };

        // Something that presses the grass this frame (world space)
        struct Presser { glm::vec3 Pos; float Radius, HalfHeight, Strength; };

        // ---- grass-free regions: static meshes' footprints (convex hulls) and Grass Blockers
        float Cross(glm::vec2 o, glm::vec2 a, glm::vec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

        // The convex hull of points in X/Z, counter-clockwise (Andrew's monotone chain). 1 or 2 points stay as they are.
        std::vector<glm::vec2> ConvexHull(std::vector<glm::vec2> p) {
            std::sort(p.begin(), p.end(), [](glm::vec2 a, glm::vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
            p.erase(std::unique(p.begin(), p.end(), [](glm::vec2 a, glm::vec2 b) { return glm::distance(a, b) < 1e-4f; }), p.end());
            if (p.size() < 3) return p;
            std::vector<glm::vec2> h(p.size() * 2);
            size_t k = 0;
            for (size_t i = 0; i < p.size(); i++) {                      // lower hull
                while (k >= 2 && Cross(h[k - 2], h[k - 1], p[i]) <= 0.0f) k--;
                h[k++] = p[i];
            }
            for (size_t i = p.size() - 1, t = k + 1; i > 0; i--) {      // upper hull
                while (k >= t && Cross(h[k - 2], h[k - 1], p[i - 1]) <= 0.0f) k--;
                h[k++] = p[i - 1];
            }
            h.resize(k - 1);
            return h;
        }

        float SegmentDistance(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
            const glm::vec2 ab = b - a;
            const float l2 = glm::dot(ab, ab);
            const float t = l2 > 0.0f ? std::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
            return glm::distance(p, a + ab * t);
        }

        // Signed distance from p to a convex polygon (counter-clockwise): negative inside. A point or a segment
        // (a hull of 1 or 2 points: a post, a thin rail on the ground) is never "inside", only near.
        float HullDistance(const std::vector<glm::vec2>& h, glm::vec2 p) {
            if (h.empty()) return 1e30f;
            if (h.size() == 1) return glm::distance(p, h[0]);
            float d = 1e30f;
            bool inside = h.size() >= 3;
            for (size_t i = 0; i < h.size(); i++) {
                const glm::vec2 a = h[i], b = h[(i + 1) % h.size()];
                d = std::min(d, SegmentDistance(p, a, b));
                if (Cross(a, b, p) < 0.0f) inside = false;
            }
            return inside ? -d : d;
        }

        // Signed distance to a Grass Blocker's shape in its own X/Z (negative inside)
        float BlockerDistance(GrassBlockerShape shape, glm::vec2 size, glm::vec2 p) {
            switch (shape) {
            case GrassBlockerShape::Circle: return glm::length(p) - std::max(size.x, 0.0f) * 0.5f;
            case GrassBlockerShape::Capsule: {
                const float r = std::max(size.y, 0.0f) * 0.5f;
                const float half = std::max(size.x * 0.5f - r, 0.0f);   // the straight part, along X
                return SegmentDistance(p, glm::vec2(-half, 0.0f), glm::vec2(half, 0.0f)) - r;
            }
            default: {
                const glm::vec2 q = glm::abs(p) - glm::max(size, glm::vec2(0.0f)) * 0.5f;
                return glm::length(glm::max(q, glm::vec2(0.0f))) + std::min(std::max(q.x, q.y), 0.0f);
            }
            }
        }

        // A region grass does not grow in: a footprint (Hull, blocked within Margin of it, inside included) or a
        // blocker (its shape in its entity's space, with a soft edge)
        struct Region {
            glm::vec2 Min{ 0.0f }, Max{ 0.0f };   // world X/Z bounds of what it can block
            const std::vector<glm::vec2>* Hull = nullptr;
            float Margin = 0.0f;
            bool Blocker = false;
            GrassBlockerShape Shape = GrassBlockerShape::Box;
            glm::vec2 Size{ 0.0f };
            float Softness = 0.0f;
            glm::mat4 Inverse{ 1.0f };          // world -> the blocker's space
            // rnd 0..1, fixed per grass spot: where the edge is soft, it decides which spots stay
            bool Blocks(const glm::vec3& p, float rnd) const {
                if (!Blocker) return HullDistance(*Hull, glm::vec2(p.x, p.z)) < Margin;
                const glm::vec3 l = glm::vec3(Inverse * glm::vec4(p, 1.0f));
                const float d = BlockerDistance(Shape, Size, glm::vec2(l.x, l.z));
                if (d >= 0.0f) return false;
                if (Softness <= 0.0f || -d >= Softness) return true;
                return rnd < -d / Softness;               // thinning towards the edge
            }
        };
    }

    struct FoliageRenderer::Impl {
        std::shared_ptr<Mesh> Meshes[2];         // 0 grass, 1 flower

        struct Chunk { glm::vec3 Min{ 0.0f }, Max{ 0.0f }; uint32_t First[2]{}, Count[2]{}; };
        struct Field {
            uint64_t Hash = 0;
            uint64_t TerrainHash = 0;             // the terrains it was scattered on
            float ScatterTime = -1e9f;            // Impl::Time of the last scatter
            bool Scattered = false;
            uint32_t Buffer = 0;
            std::vector<Chunk> Chunks;
            float MaxHeight = 0.0f;
            uint32_t Trample = 0;
            int TW = 0, TH = 0;
            std::vector<glm::vec2> Push;          // world X/Z push per texel
            bool Active = false, Dirty = false;
            bool Seen = false;
            // Ground shade (lit.frag GroundGrassShade): per cell coverage (8 bits) | ground height (24 bits over
            // ShadeMinY..ShadeMaxY, the field's space), made by Scatter; the rest is this frame's (Render)
            std::vector<uint32_t> ShadeCells;
            int SX = 0, SZ = 0;
            float ShadeMinY = 0.0f, ShadeMaxY = 0.0f, ShadeBand = 0.3f;
            glm::mat4 World{ 1.0f };
            glm::vec2 Size{ 1.0f };
            float Strength = 0.0f, DrawDistance = 0.0f;
            bool Shown = false;                   // drawn this frame (not hidden in the editor)
            ~Field() {
                if (Buffer) glDeleteBuffers(1, &Buffer);
                if (Trample) glDeleteTextures(1, &Trample);
            }
        };
        std::unordered_map<uint64_t, std::unique_ptr<Field>> Fields;   // by the entity's IDComponent
        struct Obstacle {
            Box Bounds; glm::mat4 World; std::shared_ptr<Renderable> Geometry;
            float Margin = 0.0f;                  // the Mesh Renderer's GrassMargin
            // its footprint, worked out for one ground (HullKey: the field's height and the terrains)
            uint64_t HullKey = 0;
            bool HullValid = false, Floats = true;
            std::vector<glm::vec2> Hull;
            glm::vec2 HullMin{ 0.0f }, HullMax{ 0.0f };
        };
        std::vector<Obstacle> Obstacles;
        uint64_t ObstacleHash = 0;
        struct Blocker { glm::mat4 World, Inverse; GrassBlockerComponent B; };
        std::vector<Blocker> Blockers;
        uint64_t BlockerHash = 0;
        // Collision vertices per geometry (for footprints), fetched once
        struct Points { std::weak_ptr<Renderable> Source; std::vector<glm::vec3> P; };
        std::unordered_map<const Renderable*, Points> PointCache;

        const std::vector<glm::vec3>& PointsOf(const std::shared_ptr<Renderable>& g) {
            Points& p = PointCache[g.get()];
            if (p.Source.lock() != g) {
                p.Source = g;
                p.P.clear();
                std::vector<uint32_t> idx;
                g->GetCollisionGeometry(p.P, idx);
            }
            return p.P;
        }

        // Where an obstacle meets the ground: the convex outline (X/Z) of its vertices from 0.5 m below to 0.35 m above
        // the ground under each of them (a terrain's surface there, else the field's height), so grass grows under
        // fence rails and branches but not on walls, posts, floors or trunks, and a rotated wall clears only its own
        // strip. Cached per ground; false if nothing of it is that low (it floats).
        bool Footprint(Obstacle& o, float plane, uint64_t key) {
            if (o.HullValid && o.HullKey == key) return !o.Floats;
            static thread_local std::vector<glm::vec2> pts;
            pts.clear();
            // Read-only lookup (Scatter runs this on several threads): PointsOf has filled the entry already
            const auto cached = PointCache.find(o.Geometry.get());
            static const std::vector<glm::vec3> none;
            const std::vector<glm::vec3>& points = cached != PointCache.end() ? cached->second.P : none;
            const glm::mat4& M = o.World;
            for (const glm::vec3& v : points) {
                // y first: most of a tree is far above the ground band, so x and z are often never needed
                // (summed in the order glm's mat4 * vec4 uses, so the hull is exactly what it was)
                const float wy = (M[0][1] * v.x + M[1][1] * v.y) + (M[2][1] * v.z + M[3][1]);
                if (Terrains.empty() ? (wy > plane + 0.35f || wy < plane - 0.5f)
                                     : (wy > TerrainMaxY + 0.35f && wy > plane + 0.35f)) continue;   // far above any ground: skip the lookup
                const glm::vec3 w((M[0][0] * v.x + M[1][0] * v.y) + (M[2][0] * v.z + M[3][0]), wy,
                                  (M[0][2] * v.x + M[1][2] * v.y) + (M[2][2] * v.z + M[3][2]));
                if (!Terrains.empty()) {
                    const float g = GroundAt(w.x, w.z, plane);
                    if (w.y > g + 0.35f || w.y < g - 0.5f) continue;
                }
                pts.push_back(glm::vec2(w.x, w.z));
            }
            o.Hull = ConvexHull(pts);
            o.Floats = o.Hull.empty();
            o.HullMin = glm::vec2(1e30f); o.HullMax = glm::vec2(-1e30f);
            for (const glm::vec2& p : o.Hull) { o.HullMin = glm::min(o.HullMin, p); o.HullMax = glm::max(o.HullMax, p); }
            o.HullKey = key;
            o.HullValid = true;
            return !o.Floats;
        }
        std::vector<Presser> Pressers;
        std::chrono::steady_clock::time_point Last = std::chrono::steady_clock::now();
        float Time = 0.0f;

        // Ground shade (BindGroundShade): one storage buffer with a header (field count), up to kShadeFields field
        // records and every field's cells after them; the cells are uploaded again only when a field scattered
        static constexpr int kShadeFields = 8;
        struct ShadeFieldGpu { glm::mat4 InvWorld; glm::vec4 Area, Params, Range; };
        static constexpr size_t kShadeHeader = 16 + kShadeFields * sizeof(ShadeFieldGpu);
        uint32_t ShadeBuffer = 0;
        size_t ShadeBufferSize = 0;
        bool ShadeDirty = true;
        std::unordered_map<uint64_t, uint32_t> ShadeOffset;    // by field id: its first cell in the buffer
        ~Impl() { if (ShadeBuffer) glDeleteBuffers(1, &ShadeBuffer); }

        // The ground shade map of a field from its clumps: each clump adds its footprint (about 0.2 m across x its
        // width, more for taller grass) to the cells round it, bilinearly; one 3 x 3 blur; coverage = 1 - exp(-sum).
        // The ground height of a cell is the weighted mean height of the roots there (the field's space).
        void BuildGroundShade(Field& field, const FoliageComponent& f, const glm::mat4& world, glm::vec2 size,
            const std::vector<Clump>& clumps) {
            const float cell = std::max(0.25f, std::max(size.x, size.y) / 512.0f);
            const int sx = std::clamp((int)std::ceil(size.x / cell), 1, 512), sz = std::clamp((int)std::ceil(size.y / cell), 1, 512);
            const glm::mat4 inv = glm::inverse(world);
            std::vector<float> w(size_t(sx) * sz, 0.0f), hsum(size_t(sx) * sz, 0.0f);
            const float cellArea = (size.x / sx) * (size.y / sz);
            for (const Clump& c : clumps) {
                const glm::vec3 p = glm::vec3(inv * glm::vec4(glm::vec3(c.PosRot), 1.0f));
                const float r = 0.1f * c.Params.y;                                    // footprint radius (metres)
                const float weight = 3.14159f * r * r / cellArea * std::clamp(c.Params.x / 0.45f, 0.3f, 2.0f);
                const float u = (p.x / size.x + 0.5f) * sx - 0.5f, v = (p.z / size.y + 0.5f) * sz - 0.5f;
                const int x0 = (int)std::floor(u), z0 = (int)std::floor(v);
                const float tx = u - x0, tz = v - z0;
                for (int k = 0; k < 4; k++) {
                    const int x = x0 + (k & 1), z = z0 + (k >> 1);
                    if (x < 0 || z < 0 || x >= sx || z >= sz) continue;
                    const float b = ((k & 1) ? tx : 1.0f - tx) * ((k >> 1) ? tz : 1.0f - tz) * weight;
                    w[size_t(z) * sx + x] += b;
                    hsum[size_t(z) * sx + x] += b * p.y;
                }
            }
            // 3 x 3 tent blur of both (soft edges, gaps between clumps filled)
            std::vector<float> bw(w.size(), 0.0f), bh(w.size(), 0.0f);
            for (int z = 0; z < sz; z++)
                for (int x = 0; x < sx; x++) {
                    float sw = 0.0f, sh = 0.0f, norm = 0.0f;
                    for (int dz = -1; dz <= 1; dz++)
                        for (int dx = -1; dx <= 1; dx++) {
                            const int xx = x + dx, zz = z + dz;
                            const float k = (dx ? 1.0f : 2.0f) * (dz ? 1.0f : 2.0f);
                            norm += k;
                            if (xx < 0 || zz < 0 || xx >= sx || zz >= sz) continue;
                            sw += k * w[size_t(zz) * sx + xx];
                            sh += k * hsum[size_t(zz) * sx + xx];
                        }
                    bw[size_t(z) * sx + x] = sw / norm;
                    bh[size_t(z) * sx + x] = sh / norm;
                }
            float mn = 1e30f, mx = -1e30f;
            for (size_t i = 0; i < bw.size(); i++)
                if (bw[i] > 1e-6f) { const float h = bh[i] / bw[i]; mn = std::min(mn, h); mx = std::max(mx, h); }
            if (mn > mx) { mn = mx = 0.0f; }
            if (mx - mn < 1e-3f) mx = mn + 1e-3f;
            field.ShadeCells.assign(bw.size(), 0u);
            for (size_t i = 0; i < bw.size(); i++) {
                if (bw[i] <= 1e-6f) continue;
                const float coverage = 1.0f - std::exp(-3.0f * bw[i]);
                const float h = (bh[i] / bw[i] - mn) / (mx - mn);
                field.ShadeCells[i] = (uint32_t)std::lround(std::clamp(coverage, 0.0f, 1.0f) * 255.0f) |
                                      ((uint32_t)std::lround(std::clamp(h, 0.0f, 1.0f) * 16777215.0f) << 8);
            }
            field.SX = sx; field.SZ = sz;
            field.ShadeMinY = mn; field.ShadeMaxY = mx;
            field.ShadeBand = std::max(0.3f, f.Height * 0.8f);
            ShadeDirty = true;
        }

        // Static meshes resting on something (their world bounding boxes), and a hash to notice when they change.
        // The hash is of the raw inputs (transform, parent, offset, geometry), cheap enough for every frame; the
        // world boxes are only worked out again when it changes (that took 6.5 ms a frame on the village forest).
        static void Mix(uint64_t& h, const void* data, size_t bytes) {
            const auto* w = static_cast<const uint32_t*>(data);
            for (size_t i = 0; i < bytes / 4; i++) h = (h ^ w[i]) * 0x100000001B3ull;
        }
        void GatherObstacles(Scene& scene) {
            auto& reg = scene.Registry();
            uint64_t h = 1469598103934665603ull;
            // Grass Blockers: few, so their world placement is simply worked out every frame
            {
                uint64_t bh = 1469598103934665603ull;
                Blockers.clear();
                for (auto [e, b] : reg.view<GrassBlockerComponent>().each()) {
                    const glm::mat4 M = scene.GetWorldMatrix(e);
                    Blockers.push_back({ M, glm::inverse(M), b });
                    Mix(bh, &M, sizeof(M));
                    Mix(bh, &b, sizeof(b));
                }
                BlockerHash = bh;
            }
            for (auto [e, mr, t] : reg.view<MeshRendererComponent, TransformComponent>().each()) {
                if (!mr.Static || !mr.Geometry || !mr.BlocksGrass) continue;
                Mix(h, &t.Position, sizeof(glm::vec3) * 3);              // position, rotation, scale
                Mix(h, &mr.GrassMargin, sizeof(float));
                Mix(h, &mr.LocalOffset, sizeof(glm::mat4));
                const auto* g = mr.Geometry.get();
                Mix(h, &g, sizeof(g));
                if (const auto* p = reg.try_get<ParentComponent>(e)) {      // a moved parent moves it too
                    Mix(h, &p->Parent, sizeof(p->Parent));
                    const entt::entity pe = scene.FindByID(p->Parent);
                    if (pe != entt::null) Mix(h, &reg.get<TransformComponent>(pe).Position, sizeof(glm::vec3) * 3);
                }
            }
            if (h == ObstacleHash && !Obstacles.empty()) return;
            ObstacleHash = h;
            Obstacles.clear();
            for (auto [e, mr] : reg.view<MeshRendererComponent>().each()) {
                if (!mr.Static || !mr.Geometry || !mr.BlocksGrass) continue;
                glm::vec3 mn, mx;
                mr.Geometry->GetLocalBounds(mn, mx);
                const glm::mat4 M = scene.GetWorldMatrix(e) * mr.LocalOffset;
                const glm::vec3 c = glm::vec3(M * glm::vec4((mn + mx) * 0.5f, 1.0f));
                const glm::vec3 hh = (mx - mn) * 0.5f;
                const glm::vec3 ext = glm::abs(glm::vec3(M[0])) * hh.x + glm::abs(glm::vec3(M[1])) * hh.y + glm::abs(glm::vec3(M[2])) * hh.z;
                Obstacle o;
                o.Bounds = { c - ext, c + ext };
                o.World = M;
                o.Geometry = mr.Geometry;
                o.Margin = mr.GrassMargin;
                Obstacles.push_back(std::move(o));
            }
        }

        // Moving bodies (dynamic and kinematic, not triggers) and this frame's script pushes
        void GatherPressers(Scene& scene) {
            Pressers.clear();
            auto& reg = scene.Registry();
            for (auto [e, c] : reg.view<CharacterControllerComponent>().each()) {
                const glm::mat4 M = scene.GetWorldMatrix(e);
                const glm::vec3 s(glm::length(glm::vec3(M[0])), glm::length(glm::vec3(M[1])), glm::length(glm::vec3(M[2])));
                Pressers.push_back({ glm::vec3(M[3]) + c.Center * s, c.Radius * std::max(s.x, s.z), c.Height * s.y * 0.5f, 1.0f });
            }
            for (auto [e, rb] : reg.view<RigidBodyComponent>(entt::exclude<CharacterControllerComponent>).each()) {
                if (rb.Type == BodyType::Static || rb.IsTrigger) continue;
                const glm::mat4 M = scene.GetWorldMatrix(e);
                const glm::vec3 s(glm::length(glm::vec3(M[0])), glm::length(glm::vec3(M[1])), glm::length(glm::vec3(M[2])));
                const glm::vec3 c = glm::vec3(M * glm::vec4(rb.Center, 1.0f));
                float r = 0.4f, half = 0.5f;
                switch (rb.Shape) {
                case ColliderShape::Box:     r = std::max(rb.Size.x * s.x, rb.Size.z * s.z) * 0.5f; half = rb.Size.y * s.y * 0.5f; break;
                case ColliderShape::Sphere:  r = rb.Size.x * 0.5f * std::max({ s.x, s.y, s.z }); half = r; break;
                case ColliderShape::Capsule: r = rb.Size.x * 0.5f * std::max(s.x, s.z); half = rb.Size.y * s.y * 0.5f; break;
                default: break;
                }
                Pressers.push_back({ c, r, half, 1.0f });
            }
            for (const auto& p : scene.FoliagePushes) Pressers.push_back({ p.Position, p.Radius, 0.0f, p.Strength });
            scene.FoliagePushes.clear();
        }

        static uint64_t HashOf(const FoliageComponent& f, const glm::mat4& world, uint64_t obstacles, uint64_t blockers) {
            Hasher h;
            h.Add(f.Size); h.Add(f.Density); h.Add(f.Seed); h.Add(f.Height); h.Add(f.HeightVariation); h.Add(f.Width);
            h.Add(f.DryAmount); h.Add(f.FlowerAmount); h.Add(f.AvoidStatic); h.Add(world);
            if (f.AvoidStatic) h.Add(obstacles);
            h.Add(blockers);                       // Grass Blockers apply to every field
            h.Add(f.FollowTerrain); h.Add(f.TerrainLayer);
            return h.H;
        }
        uint64_t TerrainHash = 0;                  // TerrainsHash, once a frame

        // The terrains, placed, for standing clumps on them (looked up per clump while scattering)
        struct PlacedTerrain { const TerrainData* Data; glm::vec2 Size; glm::mat4 World, Inverse; };
        std::vector<PlacedTerrain> Terrains;
        float TerrainMaxY = -1e30f;                // the highest terrain surface (world), to skip lookups far above it
        void GatherTerrains(Scene& scene) {
            Terrains.clear();
            TerrainMaxY = -1e30f;
            for (auto [e, t] : scene.Registry().view<TerrainComponent>().each()) {
                if (!t.Data) continue;
                const glm::mat4 M = scene.GetWorldMatrix(e);
                Terrains.push_back({ t.Data.get(), t.Size, M, glm::inverse(M) });
                for (int i = 0; i < 8; i++) {          // the corners of its box
                    const glm::vec3 c((i & 1 ? 0.5f : -0.5f) * t.Size.x, i & 2 ? t.Data->MaxHeight() : t.Data->MinHeight(), (i & 4 ? 0.5f : -0.5f) * t.Size.y);
                    TerrainMaxY = std::max(TerrainMaxY, (M * glm::vec4(c, 1.0f)).y);
                }
            }
        }
        // The ground under a world X/Z: the first terrain covering it, else `fallback` (the field's height)
        float GroundAt(float x, float z, float fallback) const {
            for (const PlacedTerrain& t : Terrains) {
                glm::vec3 l = glm::vec3(t.Inverse * glm::vec4(x, 0.0f, z, 1.0f));
                float h;
                if (!t.Data->HeightAt(t.Size, l.x, l.z, h)) continue;
                l.y = h;
                return (t.World * glm::vec4(l, 1.0f)).y;
            }
            return fallback;
        }
        // The first terrain under p (world): its surface height there (world, straight down in the terrain's own
        // space, which is the same unless it is tilted) and the paint weights. False if none covers p.
        bool TerrainAt(const glm::vec3& p, float& y, glm::vec4& weights) const {
            for (const PlacedTerrain& t : Terrains) {
                glm::vec3 l = glm::vec3(t.Inverse * glm::vec4(p, 1.0f));
                float h;
                if (!t.Data->HeightAt(t.Size, l.x, l.z, h)) continue;
                l.y = h;
                y = (t.World * glm::vec4(l, 1.0f)).y;
                weights = t.Data->WeightsAt(l.x / t.Size.x + 0.5f, l.z / t.Size.y + 0.5f);
                return true;
            }
            return false;
        }

        void Scatter(Field& field, const FoliageComponent& f, const glm::mat4& world) {
            const glm::vec2 size = glm::max(f.Size, glm::vec2(0.1f));
            float cell = 1.0f / std::sqrt(std::max(f.Density, 0.01f));
            if ((size.x / cell) * (size.y / cell) > (float)kMaxClumps) cell = std::sqrt(size.x * size.y / (float)kMaxClumps);
            const int nx = (int)std::ceil(size.x / cell), nz = (int)std::ceil(size.y / cell);
            const int cx = std::max(1, (int)std::ceil(size.x / kChunk)), cz = std::max(1, (int)std::ceil(size.y / kChunk));

            // Grass-free regions near the field (its world bounds): static meshes' footprints when it avoids them,
            // and every Grass Blocker
            glm::vec3 fmn(1e30f), fmx(-1e30f);
            for (int i = 0; i < 4; i++) {
                const glm::vec3 p = glm::vec3(world * glm::vec4((i & 1 ? 0.5f : -0.5f) * size.x, 0.0f, (i & 2 ? 0.5f : -0.5f) * size.y, 1.0f));
                fmn = glm::min(fmn, p); fmx = glm::max(fmx, p);
            }
            auto overlaps = [&](glm::vec2 mn, glm::vec2 mx) { return !(mx.x < fmn.x || mn.x > fmx.x || mx.y < fmn.z || mn.y > fmx.z); };
            std::vector<Region> near;
            if (f.AvoidStatic) {
                const float plane = world[3].y;
                uint64_t key = TerrainHash * 0x9E3779B97F4A7C15ull;
                uint32_t planeBits;
                std::memcpy(&planeBits, &plane, 4);
                key ^= planeBits;
                std::vector<Obstacle*> candidates, stale;
                for (Obstacle& o : Obstacles) {
                    const Box& b = o.Bounds;
                    if (!overlaps(glm::vec2(b.Min.x, b.Min.z) - 1.0f, glm::vec2(b.Max.x, b.Max.z) + 1.0f)) continue;
                    // the ground itself (a floor, a ground tile), or up in the air (a roof, a canopy): measured from the
                    // ground under its middle
                    const float g = Terrains.empty() ? plane : GroundAt((b.Min.x + b.Max.x) * 0.5f, (b.Min.z + b.Max.z) * 0.5f, plane);
                    if (b.Max.y <= g + 0.03f || b.Min.y >= g + 1.0f) continue;
                    candidates.push_back(&o);
                    if (!(o.HullValid && o.HullKey == key)) { PointsOf(o.Geometry); stale.push_back(&o); }   // the cache isn't thread-safe: fill it here
                }
                // Footprints are independent (each obstacle its own hull): worked out on several threads
                // (the village forest: 240k vertices, 100 ms on one thread in Debug)
                ParallelFor(stale.size(), ParallelParts(stale.size(), 8), [&](size_t, size_t begin, size_t end) {
                    for (size_t i = begin; i < end; i++) Footprint(*stale[i], plane, key);
                });
                for (Obstacle* op : candidates) {
                    Obstacle& o = *op;
                    if (!Footprint(o, plane, key)) continue;   // cached now
                    Region r;
                    r.Hull = &o.Hull;
                    r.Margin = 0.05f + o.Margin;
                    const float grow = std::max(r.Margin, 0.0f);
                    r.Min = o.HullMin - grow;
                    r.Max = o.HullMax + grow;
                    if (r.Margin > -1e-3f || o.Hull.size() >= 3) near.push_back(r);
                }
            }
            for (const Blocker& bl : Blockers) {
                Region r;
                r.Blocker = true;
                r.Shape = bl.B.Shape;
                r.Size = bl.B.Size;
                r.Softness = std::max(bl.B.Softness, 0.0f);
                r.Inverse = bl.Inverse;
                // world bounds: the corners of its local rectangle (a circle's square, a capsule's box)
                const glm::vec2 half = bl.B.Shape == GrassBlockerShape::Circle ? glm::vec2(bl.B.Size.x * 0.5f)
                                     : bl.B.Shape == GrassBlockerShape::Capsule ? glm::vec2(bl.B.Size.x, bl.B.Size.y) * 0.5f
                                     : bl.B.Size * 0.5f;
                r.Min = glm::vec2(1e30f); r.Max = glm::vec2(-1e30f);
                for (int k = 0; k < 4; k++) {
                    const glm::vec3 w = glm::vec3(bl.World * glm::vec4((k & 1 ? half.x : -half.x), 0.0f, (k & 2 ? half.y : -half.y), 1.0f));
                    r.Min = glm::min(r.Min, glm::vec2(w.x, w.z)); r.Max = glm::max(r.Max, glm::vec2(w.x, w.z));
                }
                if (overlaps(r.Min, r.Max)) near.push_back(r);
            }
            // ... bucketed in 4 m cells over the field's area (local), so each clump checks only a few
            const float bucket = 4.0f;
            const int bx = std::max(1, (int)std::ceil(size.x / bucket)), bz = std::max(1, (int)std::ceil(size.y / bucket));
            std::vector<std::vector<uint32_t>> buckets(size_t(bx) * bz);
            const glm::mat4 inv = glm::inverse(world);
            for (uint32_t i = 0; i < near.size(); i++) {
                glm::vec2 lmn(1e30f), lmx(-1e30f);
                for (int k = 0; k < 4; k++) {
                    const glm::vec3 w((k & 1) ? near[i].Max.x : near[i].Min.x, 0.0f, (k & 2) ? near[i].Max.y : near[i].Min.y);
                    const glm::vec3 l = glm::vec3(inv * glm::vec4(w, 1.0f));
                    lmn = glm::min(lmn, glm::vec2(l.x, l.z)); lmx = glm::max(lmx, glm::vec2(l.x, l.z));
                }
                const int x0 = std::clamp((int)std::floor((lmn.x + size.x * 0.5f) / bucket), 0, bx - 1);
                const int x1 = std::clamp((int)std::floor((lmx.x + size.x * 0.5f) / bucket), 0, bx - 1);
                const int z0 = std::clamp((int)std::floor((lmn.y + size.y * 0.5f) / bucket), 0, bz - 1);
                const int z1 = std::clamp((int)std::floor((lmx.y + size.y * 0.5f) / bucket), 0, bz - 1);
                for (int z = z0; z <= z1; z++) for (int x = x0; x <= x1; x++) buckets[size_t(z) * bx + x].push_back(i);
            }

            static const glm::vec3 petals[] = { { 0.95f, 0.94f, 0.88f }, { 0.96f, 0.80f, 0.22f }, { 0.62f, 0.42f, 0.86f },
                                                { 0.42f, 0.56f, 0.95f }, { 0.90f, 0.42f, 0.38f } };
            Rng rng(f.Seed);
            const bool useTerrain = !Terrains.empty() && (f.FollowTerrain || f.TerrainLayer >= 0);
            // Every random number is drawn whether or not the clump is kept, so the rest stay where they were. They
            // are drawn here in order, one row after another, and the rows are then worked on several threads (the
            // village's forest floor: 122k spots, 110 ms on one thread in Debug); the parts' results are joined in row
            // order, so the result is exactly the single-threaded one.
            constexpr int kDraws = 11;
            std::vector<float> draws(size_t(nx) * nz * kDraws);
            {
                // Rng::F with the state in a local (the same numbers; a member access per step is slow in Debug)
                uint64_t s = rng.S;
                float* d = draws.data();
                for (size_t i = 0, n = draws.size(); i < n; i++) {
                    s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
                    d[i] = ((uint32_t)((s * 0x2545F4914F6CDD1Dull) >> 32) >> 8) * (1.0f / 16777216.0f);
                }
                rng.S = s;
            }
            // A lane is one chunk's grass (2 i) or flowers (2 i + 1). Each part writes its clumps into one flat array
            // with their lanes, kept in locals while it runs (the parts' vectors side by side in memory made every
            // push_back fight over shared cache lines)
            const size_t lanes = size_t(cx) * cz * 2;
            const size_t parts = ParallelParts((size_t)nz, 16);
            struct PartOut { std::vector<Clump> Clumps; std::vector<uint32_t> Lane, Count; float MaxHeight = 0.0f; };
            std::vector<PartOut> partOut(parts);
            ParallelFor((size_t)nz, parts, [&](size_t part, size_t rowBegin, size_t rowEnd) {
                PartOut out;
                out.Clumps.reserve((rowEnd - rowBegin) * nx);
                out.Lane.reserve((rowEnd - rowBegin) * nx);
                out.Count.assign(lanes, 0);
                float maxHeight = 0.0f;
                for (int iz = (int)rowBegin; iz < (int)rowEnd; iz++) {
                    for (int ix = 0; ix < nx; ix++) {
                        const float* d = draws.data() + (size_t(iz) * nx + ix) * kDraws;
                        const float jx = d[0], jz = d[1], rh = d[2], rw = d[3], rr = d[4], rp = d[5],
                            rb = d[6], rt = d[7], rd = d[8], rf = d[9], rc = d[10];
                        const float lx = -size.x * 0.5f + (ix + jx) * cell, lz = -size.y * 0.5f + (iz + jz) * cell;
                        if (lx > size.x * 0.5f || lz > size.y * 0.5f) continue;
                        glm::vec3 p = glm::vec3(world * glm::vec4(lx, 0.0f, lz, 1.0f));
                        if (useTerrain) {
                            float ty;
                            glm::vec4 w;
                            const bool on = TerrainAt(p, ty, w);
                            if (f.TerrainLayer >= 0) {
                                // Only where that layer is painted: the share of its paint decides, so edges thin out
                                const float share = on ? glm::smoothstep(0.15f, 0.75f, w[std::min(f.TerrainLayer, 3)]) : 0.0f;
                                if (Hash2(ix, iz, f.Seed * 7919u + 13u) >= share) continue;
                            }
                            if (on && f.FollowTerrain) p.y = ty;
                        }
                        if (!near.empty()) {
                            const int b = std::clamp((int)((lz + size.y * 0.5f) / bucket), 0, bz - 1) * bx +
                                          std::clamp((int)((lx + size.x * 0.5f) / bucket), 0, bx - 1);
                            bool blocked = false;
                            const float rnd = Hash2(ix, iz, f.Seed * 31u + 7u);   // fixed per spot: soft edges stay put
                            // Raw pointers, not a range-for: Debug's checked iterators register with their
                            // container under one global lock, which kept the threads waiting on each other
                            const std::vector<uint32_t>& ids = buckets[b];
                            const uint32_t* id = ids.data();
                            const Region* regions = near.data();
                            for (size_t q = 0, nq = ids.size(); q < nq; q++) {
                                const Region& r = regions[id[q]];
                                if (p.x < r.Min.x || p.x > r.Max.x || p.z < r.Min.y || p.z > r.Max.y) continue;
                                if (r.Blocks(p, rnd)) { blocked = true; break; }
                            }
                            if (blocked) continue;
                        }
                        const bool flower = rf < f.FlowerAmount;
                        Clump c;
                        const float height = std::max(0.05f, f.Height * (1.0f + (rh * 2.0f - 1.0f) * f.HeightVariation)) * (flower ? 1.15f : 1.0f);
                        maxHeight = std::max(maxHeight, height);
                        c.PosRot = glm::vec4(p, rr * 6.2832f);
                        c.Params = glm::vec4(height, f.Width * (0.85f + 0.3f * rw), rp * 6.2832f, 0.0f);
                        const float bright = 0.85f + 0.27f * rb, warm = rt * 2.0f - 1.0f;
                        const float n = ValueNoise(glm::vec2(p.x, p.z) * 0.11f, f.Seed) * 0.75f + rd * 0.25f;
                        const float t = 1.0f - f.DryAmount;
                        const float dry = glm::smoothstep(t - 0.12f, t + 0.12f, n);
                        c.Color = glm::vec4(bright * (1.0f + warm * 0.06f), bright, bright * (1.0f - warm * 0.08f), dry);
                        c.Flower = glm::vec4(petals[std::min(4, (int)(rc * 5.0f))], 0.0f);
                        const int chx = std::clamp((int)((lx + size.x * 0.5f) / kChunk), 0, cx - 1);
                        const int chz = std::clamp((int)((lz + size.y * 0.5f) / kChunk), 0, cz - 1);
                        const uint32_t lane = (uint32_t)((size_t(chz) * cx + chx) * 2 + (flower ? 1 : 0));
                        out.Clumps.push_back(c);
                        out.Lane.push_back(lane);
                        out.Count[lane]++;
                    }
                }
                out.MaxHeight = maxHeight;
                partOut[part] = std::move(out);
            });

            // Group by lane (lanes in order, each lane's clumps in row order: what one thread would have made).
            // Every part knows where its share of each lane starts, so the parts copy at the same time.
            float maxHeight = 0.0f;
            std::vector<uint32_t> first(lanes + 1, 0);
            for (const PartOut& out : partOut) {
                maxHeight = std::max(maxHeight, out.MaxHeight);
                for (size_t i = 0; i < lanes; i++) first[i + 1] += out.Count[i];
            }
            for (size_t i = 0; i < lanes; i++) first[i + 1] += first[i];
            const size_t total = first[lanes];
            std::vector<Clump> all(total);
            {
                std::vector<std::vector<uint32_t>> cursors(parts);
                std::vector<uint32_t> next(first.begin(), first.end() - 1);
                for (size_t k = 0; k < parts; k++) {
                    cursors[k] = next;
                    for (size_t i = 0; i < lanes; i++) next[i] += partOut[k].Count[i];
                }
                ParallelFor(parts, parts, [&](size_t k, size_t, size_t) {
                    uint32_t* cursor = cursors[k].data();
                    const PartOut& out = partOut[k];
                    const Clump* src = out.Clumps.data();
                    const uint32_t* lane = out.Lane.data();
                    Clump* dst = all.data();
                    for (size_t j = 0, n = out.Clumps.size(); j < n; j++) dst[cursor[lane[j]]++] = src[j];
                });
            }
            partOut.clear();

            // Each chunk: its grass, then its flowers, each shuffled and ranked (a draw takes the first n: distance
            // thinning). The shuffles' random numbers are drawn here in lane order; the lanes are then shuffled on
            // several threads.
            std::vector<uint32_t> swaps(total);
            for (size_t lane = 0; lane < lanes; lane++) {
                const size_t n = first[lane + 1] - first[lane];
                for (size_t k = n; k > 1; k--) swaps[first[lane] + k - 1] = (uint32_t)(rng.Next() % k);
            }
            std::vector<glm::vec3> laneMin(lanes, glm::vec3(1e30f)), laneMax(lanes, glm::vec3(-1e30f));
            ParallelFor(lanes, ParallelParts(lanes, 8), [&](size_t, size_t begin, size_t end) {
                for (size_t lane = begin; lane < end; lane++) {
                    Clump* list = all.data() + first[lane];
                    const size_t n = first[lane + 1] - first[lane];
                    for (size_t k = n; k > 1; k--) std::swap(list[k - 1], list[swaps[first[lane] + k - 1]]);
                    glm::vec3 mn(1e30f), mx(-1e30f);
                    for (size_t k = 0; k < n; k++) {
                        list[k].Params.w = (k + 0.5f) / n;
                        mn = glm::min(mn, glm::vec3(list[k].PosRot));
                        mx = glm::max(mx, glm::vec3(list[k].PosRot));
                    }
                    laneMin[lane] = mn;
                    laneMax[lane] = mx;
                }
            });
            field.Chunks.clear();
            for (int i = 0; i < cx * cz; i++) {
                Chunk ch;
                for (int kind = 0; kind < 2; kind++) {
                    ch.First[kind] = first[size_t(i) * 2 + kind];
                    ch.Count[kind] = first[size_t(i) * 2 + kind + 1] - first[size_t(i) * 2 + kind];
                }
                if (ch.Count[0] + ch.Count[1] == 0) continue;
                const glm::vec3 mn = glm::min(laneMin[size_t(i) * 2], laneMin[size_t(i) * 2 + 1]);
                const glm::vec3 mx = glm::max(laneMax[size_t(i) * 2], laneMax[size_t(i) * 2 + 1]);
                ch.Min = mn - glm::vec3(0.6f, 0.1f, 0.6f);         // blades lean and bend beyond their roots
                ch.Max = mx + glm::vec3(0.6f, maxHeight * 1.2f + 0.1f, 0.6f);
                field.Chunks.push_back(ch);
            }
            if (!field.Buffer) glCreateBuffers(1, &field.Buffer);
            glNamedBufferData(field.Buffer, std::max<size_t>(all.size(), 1) * sizeof(Clump), all.empty() ? nullptr : all.data(), GL_STATIC_DRAW);
            field.MaxHeight = maxHeight;
            BuildGroundShade(field, f, world, size, all);

            // The trample map over the area (kept when the size is unchanged, so a re-scatter keeps the trails)
            const int tw = std::clamp((int)std::ceil(size.x / kTexel), 4, kMaxTrample), th = std::clamp((int)std::ceil(size.y / kTexel), 4, kMaxTrample);
            if (!field.Trample || tw != field.TW || th != field.TH) {
                if (field.Trample) glDeleteTextures(1, &field.Trample);
                glCreateTextures(GL_TEXTURE_2D, 1, &field.Trample);
                glTextureStorage2D(field.Trample, 1, GL_RG32F, tw, th);
                glTextureParameteri(field.Trample, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTextureParameteri(field.Trample, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTextureParameteri(field.Trample, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTextureParameteri(field.Trample, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                field.TW = tw; field.TH = th;
                field.Push.assign(size_t(tw) * th, glm::vec2(0.0f));
                field.Active = false;
                field.Dirty = true;
            }
        }

        void UpdateTrample(Field& field, const FoliageComponent& f, const glm::mat4& world, float dt) {
            if (field.Active) {                                 // stand up again
                const float k = std::exp(-dt / std::max(f.Recovery, 0.05f));
                float most = 0.0f;
                for (auto& p : field.Push) { p *= k; most = std::max(most, glm::dot(p, p)); }
                field.Active = most > 1e-5f;
                if (!field.Active) std::fill(field.Push.begin(), field.Push.end(), glm::vec2(0.0f));
                field.Dirty = true;
            }
            if (f.Interaction > 0.0f && !Pressers.empty()) {
                const glm::mat4 inv = glm::inverse(world);
                const glm::vec2 size = glm::max(f.Size, glm::vec2(0.1f));
                const float plane = world[3].y;
                for (const Presser& pr : Pressers) {
                    if (pr.Pos.y - pr.HalfHeight > plane + field.MaxHeight + 0.15f || pr.Pos.y + pr.HalfHeight < plane - 0.3f) continue;
                    const glm::vec3 l = glm::vec3(inv * glm::vec4(pr.Pos, 1.0f));
                    const float reach = pr.Radius + 0.15f;
                    const int x0 = std::max(0, (int)std::floor((l.x - reach + size.x * 0.5f) / size.x * field.TW));
                    const int x1 = std::min(field.TW - 1, (int)std::floor((l.x + reach + size.x * 0.5f) / size.x * field.TW));
                    const int z0 = std::max(0, (int)std::floor((l.z - reach + size.y * 0.5f) / size.y * field.TH));
                    const int z1 = std::min(field.TH - 1, (int)std::floor((l.z + reach + size.y * 0.5f) / size.y * field.TH));
                    for (int z = z0; z <= z1; z++) {
                        for (int x = x0; x <= x1; x++) {
                            const glm::vec2 c((x + 0.5f) / field.TW * size.x - size.x * 0.5f, (z + 0.5f) / field.TH * size.y - size.y * 0.5f);
                            const glm::vec2 d = c - glm::vec2(l.x, l.z);
                            const float dist = glm::length(d);
                            const float w = 1.0f - glm::smoothstep(pr.Radius * 0.6f, reach, dist);
                            if (w <= 0.0f) continue;
                            const glm::vec2 dirLocal = dist > 1e-3f ? d / dist : glm::vec2(1.0f, 0.0f);
                            glm::vec3 dw = glm::vec3(world * glm::vec4(dirLocal.x, 0.0f, dirLocal.y, 0.0f));
                            glm::vec2 dir(dw.x, dw.z);
                            dir = glm::length(dir) > 1e-5f ? glm::normalize(dir) : glm::vec2(1.0f, 0.0f);
                            const glm::vec2 push = dir * (w * pr.Strength * f.Interaction);
                            glm::vec2& cur = field.Push[size_t(z) * field.TW + x];
                            if (glm::dot(push, push) > glm::dot(cur, cur)) cur = push;
                            field.Active = field.Dirty = true;
                        }
                    }
                }
            }
            if (field.Dirty) {
                glTextureSubImage2D(field.Trample, 0, 0, 0, field.TW, field.TH, GL_RG, GL_FLOAT, field.Push.data());
                field.Dirty = false;
            }
        }
    };

    FoliageRenderer::FoliageRenderer() : m_Impl(std::make_unique<Impl>()) {
        m_Impl->Meshes[0] = MakeGrass();
        m_Impl->Meshes[1] = MakeFlower();
    }

    FoliageRenderer::~FoliageRenderer() = default;

    void FoliageRenderer::Render(Scene& scene, const Camera& camera, Shader& shader, const DrawList& list,
                                 const ShadowContext* shadows, const Environment* env, bool update) {
        Impl& I = *m_Impl;
        const auto now = std::chrono::steady_clock::now();
        const float dt = update ? std::clamp(std::chrono::duration<float>(now - I.Last).count(), 0.0f, 0.1f) : 0.0f;
        if (update) I.Last = now;
        I.Time += dt;

        auto view = scene.Registry().view<FoliageComponent, IDComponent>();
        if (view.begin() == view.end()) {
            I.Fields.clear();
            I.ShadeDirty = true;
            scene.FoliagePushes.clear();
            return;
        }
        if (update) { PROFILE_SCOPE("Gather obstacles"); I.GatherObstacles(scene); }
        if (update) { PROFILE_SCOPE("Gather pressers"); I.GatherPressers(scene); }
        if (update) I.TerrainHash = TerrainsHash(scene);
        for (auto& [id, field] : I.Fields) { field->Seen = false; field->Shown = false; }

        shader.Bind();
        shader.QuietMissing = true;                  // foliage.vert has no u_SceneWind* (grass has its own wind)
        scene.SetLightingUniforms(camera, shader, list, shadows, env);
        shader.SetInt("u_UseTexture", 0);
        shader.SetInt("u_UseNormalMap", 0);
        shader.SetInt("u_UseMetalRough", 0);
        shader.SetInt("u_UseEmissive", 0);
        shader.SetInt("u_AlphaMode", 0);
        shader.SetFloat("u_AlphaCutoff", 0.5f);
        shader.SetInt("u_Trample", 8);
        shader.SetFloat("u_Time", I.Time);
        shader.SetInt("u_NoGroundShade", 1);         // the blades don't shade themselves

        const GLboolean culling = glIsEnabled(GL_CULL_FACE);
        glDisable(GL_CULL_FACE);                                       // blades are seen from both sides
        glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);       // keep the entity IDs: clicks pick what is under the grass
        const Frustum frustum(camera.GetViewProjection());
        const glm::vec3 eye = camera.Position;

        for (auto [e, f, id] : view.each()) {
            auto& slot = I.Fields[id.ID];
            if (!slot) slot = std::make_unique<Impl::Field>();
            Impl::Field& field = *slot;
            field.Seen = true;
            const glm::mat4 world = scene.GetWorldMatrix(e);
            const uint64_t hash = Impl::HashOf(f, world, I.ObstacleHash, I.BlockerHash);
            // Terrain changes (sculpting, painting) scatter again at most every 0.25 s: a stroke changes it every frame
            const bool onTerrain = f.FollowTerrain || f.TerrainLayer >= 0;
            const bool terrainMoved = onTerrain && I.TerrainHash != field.TerrainHash && I.Time - field.ScatterTime > 0.25f;
            if (!field.Scattered || hash != field.Hash || terrainMoved) {
                PROFILE_SCOPE("Scatter");
                I.GatherTerrains(scene);
                I.Scatter(field, f, world);
                field.TerrainHash = I.TerrainHash;
                field.ScatterTime = I.Time;
                field.Hash = hash;
                field.Scattered = true;
            }
            if (update) { PROFILE_SCOPE("Trample"); I.UpdateTrample(field, f, world, dt); }
            if (field.Chunks.empty() || scene.IsEditorHidden(e)) continue;   // hidden with the Hierarchy's eye
            field.Shown = true;                      // for the ground shade (BindGroundShade)
            field.World = world;
            field.Size = glm::max(f.Size, glm::vec2(0.1f));
            field.Strength = std::clamp(f.GroundShade, 0.0f, 1.0f);
            field.DrawDistance = f.DrawDistance;

            const float a = glm::radians(f.WindDirection);
            // The camera's GrassDistanceScale stretches or shortens the field's draw distance (0: no grass in this view)
            const float grassScale = std::max(camera.GrassDistanceScale, 0.0f);
            if (grassScale <= 0.0f) continue;
            const float fadeEnd = std::max(f.DrawDistance * grassScale, 1.0f), fadeStart = fadeEnd * 0.5f;
            shader.SetVec2("u_WindDir", glm::vec2(std::cos(a), std::sin(a)));
            shader.SetFloat("u_WindStrength", f.WindStrength);
            shader.SetFloat("u_WindSpeed", f.WindSpeed);
            shader.SetVec3("u_BaseColor", f.BaseColor);
            shader.SetVec3("u_TipColor", f.TipColor);
            shader.SetVec3("u_DryColor", f.DryColor);
            shader.SetFloat("u_Roughness", f.Roughness);
            shader.SetFloat("u_FadeStart", fadeStart);
            shader.SetFloat("u_FadeEnd", fadeEnd);
            shader.SetMat4("u_FieldInvWorld", glm::inverse(world));
            shader.SetVec2("u_FieldSize", glm::max(f.Size, glm::vec2(0.1f)));
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, field.Buffer);
            glBindTextureUnit(8, field.Trample);

            for (const Impl::Chunk& ch : field.Chunks) {
                const float d = glm::distance(eye, glm::clamp(eye, ch.Min, ch.Max));
                if (d > fadeEnd || !frustum.Visible(ch.Min, ch.Max)) continue;
                // the share the shader keeps at the chunk's nearest point (it shrinks the rest away smoothly)
                const float keep = std::min(1.0f, 1.0f - glm::smoothstep(fadeStart, fadeEnd, d) + 0.1f);
                for (int kind = 0; kind < 2; kind++) {
                    const uint32_t n = std::min(ch.Count[kind], (uint32_t)std::ceil(ch.Count[kind] * keep));
                    if (n == 0) continue;
                    shader.SetInt("u_InstanceOffset", (int)ch.First[kind]);
                    I.Meshes[kind]->DrawInstanced(n);
                    RenderStats::Foliage += n;
                }
            }
        }

        glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (culling) glEnable(GL_CULL_FACE);
        for (auto it = I.Fields.begin(); it != I.Fields.end();) {   // fields whose entity is gone
            if (it->second->Seen) { ++it; continue; }
            it = I.Fields.erase(it);
            I.ShadeDirty = true;
        }
    }

    void FoliageRenderer::BindGroundShade(const Camera& camera) {
        Impl& I = *m_Impl;
        if (!I.ShadeBuffer) glCreateBuffers(1, &I.ShadeBuffer);
        // The cells of every field, after the header (again only when a field scattered or went away)
        if (I.ShadeDirty || I.ShadeBufferSize == 0) {
            I.ShadeDirty = false;
            I.ShadeOffset.clear();
            size_t cells = 0;
            for (auto& [id, field] : I.Fields) { I.ShadeOffset[id] = (uint32_t)cells; cells += field->ShadeCells.size(); }
            I.ShadeBufferSize = Impl::kShadeHeader + std::max<size_t>(cells, 1) * 4;
            glNamedBufferData(I.ShadeBuffer, (GLsizeiptr)I.ShadeBufferSize, nullptr, GL_DYNAMIC_DRAW);
            for (auto& [id, field] : I.Fields)
                if (!field->ShadeCells.empty())
                    glNamedBufferSubData(I.ShadeBuffer, (GLintptr)(Impl::kShadeHeader + size_t(I.ShadeOffset[id]) * 4),
                        (GLsizeiptr)(field->ShadeCells.size() * 4), field->ShadeCells.data());
        }
        // This frame's fields: shown, shading, within this camera's grass distance (the nearest first if more than fit)
        struct Pick { float Distance; uint64_t Id; Impl::Field* F; };
        std::vector<Pick> picks;
        const float grassScale = std::max(camera.GrassDistanceScale, 0.0f);
        for (auto& [id, field] : I.Fields) {
            Impl::Field& f = *field;
            if (!f.Shown || f.Strength <= 0.0f || f.ShadeCells.empty() || grassScale <= 0.0f) continue;
            const glm::vec3 local = glm::vec3(glm::inverse(f.World) * glm::vec4(camera.Position, 1.0f));
            const glm::vec2 outside = glm::max(glm::abs(glm::vec2(local.x, local.z)) - f.Size * 0.5f, glm::vec2(0.0f));
            const float fadeEnd = std::max(f.DrawDistance * grassScale, 1.0f);
            const float distance = glm::length(outside);              // to the field's area (roughly, in its space)
            if (distance > fadeEnd) continue;
            picks.push_back({ distance, id, &f });
        }
        std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.Distance < b.Distance; });
        if (picks.size() > (size_t)Impl::kShadeFields) picks.resize(Impl::kShadeFields);

        std::vector<uint8_t> header(Impl::kShadeHeader, 0);
        const uint32_t count = (uint32_t)picks.size();
        std::memcpy(header.data(), &count, 4);
        for (size_t i = 0; i < picks.size(); i++) {
            const Impl::Field& f = *picks[i].F;
            const float fadeEnd = std::max(f.DrawDistance * grassScale, 1.0f);
            Impl::ShadeFieldGpu g;
            g.InvWorld = glm::inverse(f.World);
            g.Area = glm::vec4(f.Size, (float)f.SX, (float)f.SZ);
            g.Params = glm::vec4(f.Strength, fadeEnd * 0.5f, fadeEnd, f.ShadeBand);
            g.Range = glm::vec4(f.ShadeMinY, f.ShadeMaxY, (float)I.ShadeOffset[picks[i].Id], 0.0f);
            std::memcpy(header.data() + 16 + i * sizeof(Impl::ShadeFieldGpu), &g, sizeof(g));
        }
        glNamedBufferSubData(I.ShadeBuffer, 0, (GLsizeiptr)header.size(), header.data());
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, I.ShadeBuffer);
    }

} // namespace Engine
