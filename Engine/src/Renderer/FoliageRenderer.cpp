#include "Renderer/FoliageRenderer.h"
#include "Renderer/Mesh.h"
#include "Renderer/Shader.h"
#include "Renderer/Camera.h"
#include "Renderer/RenderStats.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Core/Profiler.h"

#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
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
    }

    struct FoliageRenderer::Impl {
        std::shared_ptr<Mesh> Meshes[2];         // 0 grass, 1 flower

        struct Chunk { glm::vec3 Min{ 0.0f }, Max{ 0.0f }; uint32_t First[2]{}, Count[2]{}; };
        struct Field {
            uint64_t Hash = 0;
            bool Scattered = false;
            uint32_t Buffer = 0;
            std::vector<Chunk> Chunks;
            float MaxHeight = 0.0f;
            uint32_t Trample = 0;
            int TW = 0, TH = 0;
            std::vector<glm::vec2> Push;          // world X/Z push per texel
            bool Active = false, Dirty = false;
            bool Seen = false;
            ~Field() {
                if (Buffer) glDeleteBuffers(1, &Buffer);
                if (Trample) glDeleteTextures(1, &Trample);
            }
        };
        std::unordered_map<uint64_t, std::unique_ptr<Field>> Fields;   // by the entity's IDComponent
        struct Obstacle { Box Bounds; glm::mat4 World; std::shared_ptr<Renderable> Geometry; };
        std::vector<Obstacle> Obstacles;
        uint64_t ObstacleHash = 0;
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

        // Where an obstacle meets the ground: the X/Z box of its vertices within 0.35 m of the field's plane, so
        // grass grows under fence rails and branches but not on walls, posts, floors or trunks. Empty: it floats.
        bool Footprint(const Obstacle& o, float plane, Box& out) {
            glm::vec3 mn(1e30f), mx(-1e30f);
            bool any = false;
            for (const glm::vec3& v : PointsOf(o.Geometry)) {
                const glm::vec3 w = glm::vec3(o.World * glm::vec4(v, 1.0f));
                if (w.y > plane + 0.35f || w.y < plane - 0.5f) continue;
                mn = glm::min(mn, w); mx = glm::max(mx, w); any = true;
            }
            out = { mn, mx };
            return any;
        }
        std::vector<Presser> Pressers;
        std::chrono::steady_clock::time_point Last = std::chrono::steady_clock::now();
        float Time = 0.0f;

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
            for (auto [e, mr, t] : reg.view<MeshRendererComponent, TransformComponent>().each()) {
                if (!mr.Static || !mr.Geometry) continue;
                Mix(h, &t.Position, sizeof(glm::vec3) * 3);              // position, rotation, scale
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
                if (!mr.Static || !mr.Geometry) continue;
                glm::vec3 mn, mx;
                mr.Geometry->GetLocalBounds(mn, mx);
                const glm::mat4 M = scene.GetWorldMatrix(e) * mr.LocalOffset;
                const glm::vec3 c = glm::vec3(M * glm::vec4((mn + mx) * 0.5f, 1.0f));
                const glm::vec3 hh = (mx - mn) * 0.5f;
                const glm::vec3 ext = glm::abs(glm::vec3(M[0])) * hh.x + glm::abs(glm::vec3(M[1])) * hh.y + glm::abs(glm::vec3(M[2])) * hh.z;
                Obstacles.push_back({ { c - ext, c + ext }, M, mr.Geometry });
            }
        }

        // Moving bodies (dynamic and kinematic, not triggers) and this frame's script pushes
        void GatherPressers(Scene& scene) {
            Pressers.clear();
            auto& reg = scene.Registry();
            for (auto [e, rb] : reg.view<RigidBodyComponent>().each()) {
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

        static uint64_t HashOf(const FoliageComponent& f, const glm::mat4& world, uint64_t obstacles) {
            Hasher h;
            h.Add(f.Size); h.Add(f.Density); h.Add(f.Seed); h.Add(f.Height); h.Add(f.HeightVariation); h.Add(f.Width);
            h.Add(f.DryAmount); h.Add(f.FlowerAmount); h.Add(f.AvoidStatic); h.Add(world);
            if (f.AvoidStatic) h.Add(obstacles);
            return h.H;
        }

        void Scatter(Field& field, const FoliageComponent& f, const glm::mat4& world) {
            const glm::vec2 size = glm::max(f.Size, glm::vec2(0.1f));
            float cell = 1.0f / std::sqrt(std::max(f.Density, 0.01f));
            if ((size.x / cell) * (size.y / cell) > (float)kMaxClumps) cell = std::sqrt(size.x * size.y / (float)kMaxClumps);
            const int nx = (int)std::ceil(size.x / cell), nz = (int)std::ceil(size.y / cell);
            const int cx = std::max(1, (int)std::ceil(size.x / kChunk)), cz = std::max(1, (int)std::ceil(size.y / kChunk));

            // Obstacles near the field only (its world bounds), when it avoids them
            std::vector<Box> near;
            if (f.AvoidStatic) {
                glm::vec3 mn(1e30f), mx(-1e30f);
                for (int i = 0; i < 4; i++) {
                    const glm::vec3 p = glm::vec3(world * glm::vec4((i & 1 ? 0.5f : -0.5f) * size.x, 0.0f, (i & 2 ? 0.5f : -0.5f) * size.y, 1.0f));
                    mn = glm::min(mn, p); mx = glm::max(mx, p);
                }
                const float plane = world[3].y;
                for (const Obstacle& o : Obstacles) {
                    const Box& b = o.Bounds;
                    if (b.Max.x < mn.x || b.Min.x > mx.x || b.Max.z < mn.z || b.Min.z > mx.z) continue;
                    if (b.Max.y <= plane + 0.03f || b.Min.y >= plane + 1.0f) continue;   // the ground itself, or up in the air
                    Box foot;
                    if (Footprint(o, plane, foot)) near.push_back(foot);
                }
            }
            // ... bucketed in 4 m cells over the field's area (local), so each clump checks only a few
            const float bucket = 4.0f;
            const int bx = std::max(1, (int)std::ceil(size.x / bucket)), bz = std::max(1, (int)std::ceil(size.y / bucket));
            std::vector<std::vector<uint32_t>> buckets(size_t(bx) * bz);
            const glm::mat4 inv = glm::inverse(world);
            for (uint32_t i = 0; i < near.size(); i++) {
                glm::vec2 lmn(1e30f), lmx(-1e30f);
                for (int k = 0; k < 4; k++) {
                    const glm::vec3 w((k & 1) ? near[i].Max.x : near[i].Min.x, 0.0f, (k & 2) ? near[i].Max.z : near[i].Min.z);
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
            std::vector<std::vector<Clump>> lists(size_t(cx) * cz * 2);
            float maxHeight = 0.0f;
            for (int iz = 0; iz < nz; iz++) {
                for (int ix = 0; ix < nx; ix++) {
                    // every random number is drawn whether or not the clump is kept: the rest stay where they were
                    const float jx = rng.F(), jz = rng.F(), rh = rng.F(), rw = rng.F(), rr = rng.F(), rp = rng.F(),
                        rb = rng.F(), rt = rng.F(), rd = rng.F(), rf = rng.F(), rc = rng.F();
                    const float lx = -size.x * 0.5f + (ix + jx) * cell, lz = -size.y * 0.5f + (iz + jz) * cell;
                    if (lx > size.x * 0.5f || lz > size.y * 0.5f) continue;
                    const glm::vec3 p = glm::vec3(world * glm::vec4(lx, 0.0f, lz, 1.0f));
                    if (!near.empty()) {
                        const int b = std::clamp((int)((lz + size.y * 0.5f) / bucket), 0, bz - 1) * bx +
                                      std::clamp((int)((lx + size.x * 0.5f) / bucket), 0, bx - 1);
                        bool blocked = false;
                        for (uint32_t i : buckets[b]) {
                            const Box& o = near[i];
                            if (p.x > o.Min.x - 0.05f && p.x < o.Max.x + 0.05f && p.z > o.Min.z - 0.05f && p.z < o.Max.z + 0.05f) {
                                blocked = true; break;
                            }
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
                    lists[(size_t(chz) * cx + chx) * 2 + (flower ? 1 : 0)].push_back(c);
                }
            }

            // Each chunk: its grass, then its flowers, each shuffled and ranked (a draw takes the first n: distance thinning)
            std::vector<Clump> all;
            field.Chunks.clear();
            for (int i = 0; i < cx * cz; i++) {
                Chunk ch;
                glm::vec3 mn(1e30f), mx(-1e30f);
                for (int kind = 0; kind < 2; kind++) {
                    auto& list = lists[size_t(i) * 2 + kind];
                    for (size_t k = list.size(); k > 1; k--) std::swap(list[k - 1], list[rng.Next() % k]);
                    ch.First[kind] = (uint32_t)all.size();
                    ch.Count[kind] = (uint32_t)list.size();
                    for (size_t k = 0; k < list.size(); k++) {
                        list[k].Params.w = (k + 0.5f) / list.size();
                        mn = glm::min(mn, glm::vec3(list[k].PosRot));
                        mx = glm::max(mx, glm::vec3(list[k].PosRot));
                        all.push_back(list[k]);
                    }
                }
                if (ch.Count[0] + ch.Count[1] == 0) continue;
                ch.Min = mn - glm::vec3(0.6f, 0.1f, 0.6f);         // blades lean and bend beyond their roots
                ch.Max = mx + glm::vec3(0.6f, maxHeight * 1.2f + 0.1f, 0.6f);
                field.Chunks.push_back(ch);
            }
            if (!field.Buffer) glCreateBuffers(1, &field.Buffer);
            glNamedBufferData(field.Buffer, std::max<size_t>(all.size(), 1) * sizeof(Clump), all.empty() ? nullptr : all.data(), GL_STATIC_DRAW);
            field.MaxHeight = maxHeight;

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
            scene.FoliagePushes.clear();
            return;
        }
        if (update) { PROFILE_SCOPE("Gather obstacles"); I.GatherObstacles(scene); }
        if (update) { PROFILE_SCOPE("Gather pressers"); I.GatherPressers(scene); }
        for (auto& [id, field] : I.Fields) field->Seen = false;

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
            const uint64_t hash = Impl::HashOf(f, world, I.ObstacleHash);
            if (!field.Scattered || hash != field.Hash) {
                PROFILE_SCOPE("Scatter");
                I.Scatter(field, f, world);
                field.Hash = hash;
                field.Scattered = true;
            }
            if (update) { PROFILE_SCOPE("Trample"); I.UpdateTrample(field, f, world, dt); }
            if (field.Chunks.empty()) continue;

            const float a = glm::radians(f.WindDirection);
            const float fadeEnd = std::max(f.DrawDistance, 1.0f), fadeStart = fadeEnd * 0.5f;
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
        for (auto it = I.Fields.begin(); it != I.Fields.end();)     // fields whose entity is gone
            it = it->second->Seen ? std::next(it) : I.Fields.erase(it);
    }

} // namespace Engine
