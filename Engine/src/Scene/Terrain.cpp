#include "Scene/Terrain.h"
#include "Scene/Scene.h"
#include "Scene/Components.h"
#include "Renderer/Mesh.h"
#include "Core/Log.h"

#include <glad/glad.h>
#include <stb_image.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace Engine {

    namespace {
        constexpr char kMagic[4] = { 'M', 'D', 'B', 'T' };
        constexpr uint32_t kVersion = 1;

        // 0..1 weight of a brush at t = distance / radius
        float BrushWeight(float t, float falloff) {
            if (t >= 1.0f) return 0.0f;
            const float inner = 1.0f - std::clamp(falloff, 0.0f, 1.0f);
            if (t <= inner) return 1.0f;
            const float s = (t - inner) / std::max(1.0f - inner, 1e-4f);
            return 1.0f - s * s * (3.0f - 2.0f * s);
        }

        float Hash2(int x, int y, uint32_t seed) {
            uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
            h = (h ^ (h >> 13)) * 1274126177u;
            return ((h ^ (h >> 16)) & 0xFFFFFFu) / 16777216.0f;
        }
        // Smooth value noise, 0..1, one feature per unit (quintic fade: no creases between cells)
        float ValueNoise(float px, float py, uint32_t seed) {
            const float fx = std::floor(px), fy = std::floor(py);
            const float tx = px - fx, ty = py - fy;
            const float ux = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);
            const float uy = ty * ty * ty * (ty * (ty * 6.0f - 15.0f) + 10.0f);
            const int x = (int)fx, y = (int)fy;
            const float a = Hash2(x, y, seed), b = Hash2(x + 1, y, seed), c = Hash2(x, y + 1, seed), d = Hash2(x + 1, y + 1, seed);
            return (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uy;
        }
        float Fbm(float x, float y, int octaves, float persistence, float ridges, uint32_t seed) {
            float sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = 1.0f;
            for (int o = 0; o < octaves; o++) {
                float n = ValueNoise(x * freq + o * 17.31f, y * freq - o * 9.17f, seed + (uint32_t)o * 1013u);
                if (ridges > 0.0f) n = n + (1.0f - std::abs(n * 2.0f - 1.0f) - n) * ridges;   // folded: sharp crests
                sum += n * amp;
                norm += amp;
                amp *= persistence;
                freq *= 2.03f;
            }
            return norm > 0.0f ? sum / norm : 0.5f;
        }

        uint64_t Fnv(uint64_t h, const void* data, size_t bytes) {
            const auto* p = static_cast<const uint8_t*>(data);
            size_t i = 0;
            for (; i + 8 <= bytes; i += 8) {               // a word at a time (a 513 terrain is 2 MB)
                uint64_t w;
                std::memcpy(&w, p + i, 8);
                h = (h ^ w) * 0x100000001B3ull;
            }
            for (; i < bytes; i++) h = (h ^ p[i]) * 0x100000001B3ull;
            return h;
        }
    }

    // ---------------------------------------------------------------- GL texture holder
    struct TerrainData::GLTexture {
        uint32_t ID = 0;
        GLTexture() = default;
        GLTexture(const GLTexture&) = delete;
        ~GLTexture() { if (ID) glDeleteTextures(1, &ID); }
    };

    uint32_t TerrainData::SplatResolutionFor(uint32_t resolution) {
        return std::clamp((resolution - 1) * 2, 64u, 1024u);
    }

    TerrainData::TerrainData(uint32_t resolution, float height) {
        m_Res = std::clamp(resolution, MinResolution, MaxResolution);
        m_SplatRes = SplatResolutionFor(m_Res);
        m_Heights = std::make_shared<std::vector<float>>(size_t(m_Res) * m_Res, height);
        m_Splat = std::make_shared<std::vector<uint8_t>>(size_t(m_SplatRes) * m_SplatRes * 4, 0);
        for (size_t i = 0; i < m_Splat->size(); i += 4) (*m_Splat)[i] = 255;   // all layer 0
        MarkAll();
    }

    TerrainData::TerrainData(const TerrainData& o)
        : Material(o.Material), m_Res(o.m_Res), m_SplatRes(o.m_SplatRes), m_Heights(o.m_Heights), m_Splat(o.m_Splat),
          m_Revision(o.m_Revision), m_HashRevision(o.m_HashRevision), m_Hash(o.m_Hash),
          m_RangeRevision(o.m_RangeRevision), m_Min(o.m_Min), m_Max(o.m_Max),
          m_Chunks(o.m_Chunks), m_ChunkDirty(o.m_ChunkDirty), m_BuiltSize(o.m_BuiltSize),
          m_SplatTex(o.m_SplatTex), m_SplatOwned(false), m_SplatDirty(o.m_SplatDirty), m_SplatRect(o.m_SplatRect) {}

    TerrainData::~TerrainData() = default;

    float TerrainData::At(int x, int z) const {
        x = std::clamp(x, 0, (int)m_Res - 1);
        z = std::clamp(z, 0, (int)m_Res - 1);
        return (*m_Heights)[size_t(z) * m_Res + x];
    }

    void TerrainData::Set(int x, int z, float h) {
        if (x < 0 || z < 0 || x >= (int)m_Res || z >= (int)m_Res) return;
        OwnHeights();
        (*m_Heights)[size_t(z) * m_Res + x] = h;
        MarkHeights(x, z, x, z);
    }

    // The surface of the two triangles of a grid square, split along (0,0)-(1,1) like Jolt's height field
    float TerrainData::SampleHeight(float gx, float gz) const {
        const float maxG = (float)(m_Res - 1);
        gx = std::clamp(gx, 0.0f, maxG);
        gz = std::clamp(gz, 0.0f, maxG);
        const int x = std::min((int)gx, (int)m_Res - 2), z = std::min((int)gz, (int)m_Res - 2);
        const float fx = gx - x, fz = gz - z;
        const float h00 = At(x, z), h10 = At(x + 1, z), h01 = At(x, z + 1), h11 = At(x + 1, z + 1);
        if (fx > fz) return h00 + (h10 - h00) * fx + (h11 - h10) * fz;
        return h00 + (h11 - h01) * fx + (h01 - h00) * fz;
    }

    bool TerrainData::HeightAt(const glm::vec2& size, float x, float z, float& h) const {
        const float gx = (x / size.x + 0.5f) * (m_Res - 1), gz = (z / size.y + 0.5f) * (m_Res - 1);
        const float maxG = (float)(m_Res - 1);
        if (!(gx >= 0.0f && gz >= 0.0f && gx <= maxG && gz <= maxG)) return false;
        h = SampleHeight(gx, gz);
        return true;
    }

    glm::vec3 TerrainData::NormalAt(const glm::vec2& size, float x, float z) const {
        const float dx = size.x / (m_Res - 1), dz = size.y / (m_Res - 1);
        const float gx = (x / size.x + 0.5f) * (m_Res - 1), gz = (z / size.y + 0.5f) * (m_Res - 1);
        const float hl = SampleHeight(gx - 1.0f, gz), hr = SampleHeight(gx + 1.0f, gz);
        const float hb = SampleHeight(gx, gz - 1.0f), hf = SampleHeight(gx, gz + 1.0f);
        return glm::normalize(glm::vec3(-(hr - hl) / (2.0f * dx), 1.0f, -(hf - hb) / (2.0f * dz)));
    }

    float TerrainData::MinHeight() const {
        if (m_RangeRevision != m_Revision) {
            const auto [mn, mx] = std::minmax_element(m_Heights->begin(), m_Heights->end());
            m_Min = *mn; m_Max = *mx;
            m_RangeRevision = m_Revision;
        }
        return m_Min;
    }
    float TerrainData::MaxHeight() const { MinHeight(); return m_Max; }

    glm::vec4 TerrainData::WeightsAt(float u, float v) const {
        const float sx = std::clamp(u * m_SplatRes - 0.5f, 0.0f, (float)m_SplatRes - 1.0f);
        const float sz = std::clamp(v * m_SplatRes - 0.5f, 0.0f, (float)m_SplatRes - 1.0f);
        const int x0 = (int)sx, z0 = (int)sz;
        const int x1 = std::min(x0 + 1, (int)m_SplatRes - 1), z1 = std::min(z0 + 1, (int)m_SplatRes - 1);
        const float fx = sx - x0, fz = sz - z0;
        auto texel = [&](int x, int z) {
            const uint8_t* p = &(*m_Splat)[(size_t(z) * m_SplatRes + x) * 4];
            return glm::vec4(p[0], p[1], p[2], p[3]);
        };
        glm::vec4 w = glm::mix(glm::mix(texel(x0, z0), texel(x1, z0), fx), glm::mix(texel(x0, z1), texel(x1, z1), fx), fz);
        const float sum = w.x + w.y + w.z + w.w;
        return sum > 0.0f ? w / sum : glm::vec4(1, 0, 0, 0);
    }

    // ---------------------------------------------------------------- change tracking
    void TerrainData::MarkHeights(int x0, int z0, int x1, int z1) {
        m_Revision++;
        // Normals read the neighbours, so the chunks one sample around the change are rebuilt too
        const int n = (int)((m_Res - 1 + ChunkQuads - 1) / ChunkQuads);
        auto first = [](int s) { return s <= (int)ChunkQuads ? 0 : (s - 1) / (int)ChunkQuads; };
        const int cx0 = first(std::max(x0 - 1, 0)), cz0 = first(std::max(z0 - 1, 0));
        const int cx1 = std::min(n - 1, (x1 + 1) / (int)ChunkQuads), cz1 = std::min(n - 1, (z1 + 1) / (int)ChunkQuads);
        if (m_ChunkDirty.size() != size_t(n) * n) m_ChunkDirty.assign(size_t(n) * n, 1);
        for (int cz = cz0; cz <= cz1; cz++)
            for (int cx = cx0; cx <= cx1; cx++) m_ChunkDirty[size_t(cz) * n + cx] = 1;
    }

    void TerrainData::MarkSplat(int x0, int z0, int x1, int z1) {
        m_Revision++;
        if (!m_SplatDirty) m_SplatRect = glm::ivec4(x0, z0, x1, z1);
        else m_SplatRect = glm::ivec4(std::min(m_SplatRect.x, x0), std::min(m_SplatRect.y, z0),
                                      std::max(m_SplatRect.z, x1), std::max(m_SplatRect.w, z1));
        m_SplatDirty = true;
    }

    void TerrainData::MarkAll() {
        MarkHeights(0, 0, (int)m_Res - 1, (int)m_Res - 1);
        MarkSplat(0, 0, (int)m_SplatRes - 1, (int)m_SplatRes - 1);
    }

    uint64_t TerrainData::Hash() const {
        if (m_HashRevision != m_Revision || m_Hash == 0) {
            uint64_t h = 1469598103934665603ull;
            h = Fnv(h, &m_Res, sizeof(m_Res));
            h = Fnv(h, m_Heights->data(), m_Heights->size() * sizeof(float));
            h = Fnv(h, m_Splat->data(), m_Splat->size());
            m_Hash = h ? h : 1;
            m_HashRevision = m_Revision;
        }
        return m_Hash;
    }

    // ---------------------------------------------------------------- editing
    void TerrainData::ApplyBrush(const TerrainBrush& b, const glm::vec2& size, const glm::vec2& c, float dt) {
        const float radius = std::max(b.Radius, 0.01f);
        const float strength = std::clamp(b.Strength, 0.0f, 1.0f);

        if (b.Mode == TerrainBrushMode::Paint) {
            const int layer = std::clamp(b.Layer, 0, 3);
            const float res = (float)m_SplatRes;
            const int x0 = std::max(0, (int)std::floor(((c.x - radius) / size.x + 0.5f) * res - 0.5f));
            const int x1 = std::min((int)m_SplatRes - 1, (int)std::ceil(((c.x + radius) / size.x + 0.5f) * res - 0.5f));
            const int z0 = std::max(0, (int)std::floor(((c.y - radius) / size.y + 0.5f) * res - 0.5f));
            const int z1 = std::min((int)m_SplatRes - 1, (int)std::ceil(((c.y + radius) / size.y + 0.5f) * res - 0.5f));
            if (x0 > x1 || z0 > z1) return;
            OwnSplat();
            for (int z = z0; z <= z1; z++) {
                for (int x = x0; x <= x1; x++) {
                    const glm::vec2 p(((x + 0.5f) / res - 0.5f) * size.x, ((z + 0.5f) / res - 0.5f) * size.y);
                    const float w = BrushWeight(glm::length(p - c) / radius, b.Falloff);
                    if (w <= 0.0f) continue;
                    const float a = std::clamp(w * strength * dt * 8.0f, 0.0f, 1.0f);
                    uint8_t* s = &(*m_Splat)[(size_t(z) * m_SplatRes + x) * 4];
                    if (s[layer] == 255) continue;
                    int q[4], sum = 0;
                    for (int i = 0; i < 4; i++) {
                        const float v = s[i] / 255.0f * (1.0f - a) + (i == layer ? a : 0.0f);
                        q[i] = (int)std::lround(v * 255.0f);
                        sum += q[i];
                    }
                    q[layer] = std::clamp(q[layer] + 255 - sum, 0, 255);
                    // Low strength at a high frame rate can round to no change: always move at least one step
                    if (q[layer] == s[layer] && a > 0.001f) {
                        int big = layer == 0 ? 1 : 0;
                        for (int i = 0; i < 4; i++) if (i != layer && q[i] > q[big]) big = i;
                        if (q[big] > 0) { q[big]--; q[layer]++; }
                    }
                    for (int i = 0; i < 4; i++) s[i] = (uint8_t)q[i];
                }
            }
            MarkSplat(x0, z0, x1, z1);
            return;
        }

        const float g = (float)(m_Res - 1);
        const float dx = size.x / g, dz = size.y / g;
        const int x0 = std::max(0, (int)std::floor(((c.x - radius) / size.x + 0.5f) * g));
        const int x1 = std::min((int)m_Res - 1, (int)std::ceil(((c.x + radius) / size.x + 0.5f) * g));
        const int z0 = std::max(0, (int)std::floor(((c.y - radius) / size.y + 0.5f) * g));
        const int z1 = std::min((int)m_Res - 1, (int)std::ceil(((c.y + radius) / size.y + 0.5f) * g));
        if (x0 > x1 || z0 > z1) return;
        OwnHeights();

        // Raise / lower: metres per second, growing with the brush so big hills take as long as small bumps
        const float rate = strength * std::max(radius, 1.0f) * 0.8f;
        std::vector<float> before;                       // Smooth reads the heights as they were
        if (b.Mode == TerrainBrushMode::Smooth) {
            before.reserve(size_t(x1 - x0 + 3) * (z1 - z0 + 3));
            for (int z = z0 - 1; z <= z1 + 1; z++) for (int x = x0 - 1; x <= x1 + 1; x++) before.push_back(At(x, z));
        }
        const int bw = x1 - x0 + 3;
        auto old = [&](int x, int z) { return before[size_t(z - z0 + 1) * bw + (x - x0 + 1)]; };

        for (int z = z0; z <= z1; z++) {
            for (int x = x0; x <= x1; x++) {
                const glm::vec2 p(x * dx - size.x * 0.5f, z * dz - size.y * 0.5f);
                const float w = BrushWeight(glm::length(p - c) / radius, b.Falloff);
                if (w <= 0.0f) continue;
                float& h = (*m_Heights)[size_t(z) * m_Res + x];
                switch (b.Mode) {
                case TerrainBrushMode::Raise: h += rate * w * dt; break;
                case TerrainBrushMode::Lower: h -= rate * w * dt; break;
                case TerrainBrushMode::Smooth: {
                    float sum = 0.0f;
                    for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) sum += old(x + i, z + j);
                    h += (sum / 9.0f - h) * std::clamp(w * strength * dt * 15.0f, 0.0f, 1.0f);
                    break;
                }
                case TerrainBrushMode::Flatten:
                    h += (b.Target - h) * std::clamp(w * strength * dt * 6.0f, 0.0f, 1.0f);
                    break;
                case TerrainBrushMode::Noise: {
                    const float scale = std::max(b.NoiseScale, 0.1f);
                    const float n = Fbm(p.x / scale, p.y / scale, 3, 0.5f, 0.0f, b.Seed) * 2.0f - 1.0f;
                    h += n * rate * 0.6f * w * dt;
                    break;
                }
                default: break;
                }
            }
        }
        MarkHeights(x0, z0, x1, z1);
    }

    void TerrainData::Generate(const TerrainNoise& n, const glm::vec2& size) {
        OwnHeights();
        std::vector<float> v(m_Heights->size());
        float mn = 1e30f, mx = -1e30f;
        const float feature = std::max(n.FeatureSize, 0.01f);
        const int octaves = std::clamp(n.Octaves, 1, 10);
        const float dx = size.x / (m_Res - 1), dz = size.y / (m_Res - 1);   // metres per sample
        for (uint32_t z = 0; z < m_Res; z++) {
            for (uint32_t x = 0; x < m_Res; x++) {
                const float f = Fbm(x * dx / feature, z * dz / feature, octaves, std::clamp(n.Roughness, 0.0f, 1.0f),
                                    std::clamp(n.Ridges, 0.0f, 1.0f), n.Seed);
                v[size_t(z) * m_Res + x] = f;
                mn = std::min(mn, f); mx = std::max(mx, f);
            }
        }
        const float range = std::max(mx - mn, 1e-6f);
        for (size_t i = 0; i < v.size(); i++) {
            const float t = (v[i] - mn) / range;          // 0..1 over this terrain
            if (n.Add) (*m_Heights)[i] += (t - 0.5f) * n.Height;
            else       (*m_Heights)[i] = t * n.Height;
        }
        MarkHeights(0, 0, (int)m_Res - 1, (int)m_Res - 1);
    }

    void TerrainData::Fill(float height) {
        OwnHeights();
        std::fill(m_Heights->begin(), m_Heights->end(), height);
        MarkHeights(0, 0, (int)m_Res - 1, (int)m_Res - 1);
    }

    void TerrainData::FillPaint(int layer) {
        layer = std::clamp(layer, 0, 3);
        OwnSplat();
        for (size_t i = 0; i < m_Splat->size(); i += 4)
            for (int k = 0; k < 4; k++) (*m_Splat)[i + k] = k == layer ? 255 : 0;
        MarkSplat(0, 0, (int)m_SplatRes - 1, (int)m_SplatRes - 1);
    }

    void TerrainData::AutoPaint(const glm::vec2& size, const AutoPaintRules& r) {
        OwnSplat();
        const float res = (float)m_SplatRes;
        auto layerOk = [](int l) { return l >= 0 && l < 4; };
        for (uint32_t z = 0; z < m_SplatRes; z++) {
            for (uint32_t x = 0; x < m_SplatRes; x++) {
                const float lx = ((x + 0.5f) / res - 0.5f) * size.x, lz = ((z + 0.5f) / res - 0.5f) * size.y;
                float h = 0.0f;
                HeightAt(size, lx, lz, h);
                const glm::vec3 n = NormalAt(size, lx, lz);
                const float slope = glm::degrees(std::acos(std::clamp(n.y, -1.0f, 1.0f)));
                float w[4] = { 0, 0, 0, 0 };
                float rest = 1.0f;                        // what the base layer gets
                auto take = [&](int layer, float amount) {
                    if (!layerOk(layer) || amount <= 0.0f) return;
                    amount *= rest;
                    w[layer] += amount;
                    rest -= amount;
                };
                // Steep ground first (cliffs stay rock at any height), then low, then high
                take(r.SteepLayer, glm::smoothstep(r.SteepDegrees - 3.0f, r.SteepDegrees + 3.0f, slope));
                take(r.LowLayer, 1.0f - glm::smoothstep(r.LowHeight - 0.5f, r.LowHeight + 0.5f, h));
                take(r.HighLayer, glm::smoothstep(r.HighHeight - 0.5f, r.HighHeight + 0.5f, h));
                if (layerOk(r.BaseLayer)) w[r.BaseLayer] += rest;
                else {                                    // no base layer: the rest goes to whatever is there
                    float s = w[0] + w[1] + w[2] + w[3];
                    if (s <= 0.0f) { w[0] = 1.0f; s = 1.0f; }
                    for (float& v : w) v /= s;
                }
                uint8_t* p = &(*m_Splat)[(size_t(z) * m_SplatRes + x) * 4];
                int q[4], sum = 0, big = 0;
                for (int i = 0; i < 4; i++) { q[i] = (int)std::lround(w[i] * 255.0f); sum += q[i]; if (q[i] > q[big]) big = i; }
                q[big] = std::clamp(q[big] + 255 - sum, 0, 255);
                for (int i = 0; i < 4; i++) p[i] = (uint8_t)q[i];
            }
        }
        MarkSplat(0, 0, (int)m_SplatRes - 1, (int)m_SplatRes - 1);
    }

    bool TerrainData::ImportHeightmap(const std::string& path, float low, float range, std::string* error) {
        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(false);         // row 0 = the top of the image = -Z
        stbi_us* px = stbi_load_16(path.c_str(), &w, &h, &ch, 1);
        stbi_set_flip_vertically_on_load(true);          // what Texture2D expects
        OwnHeights();
        if (!px) {
            if (error) *error = stbi_failure_reason() ? stbi_failure_reason() : "can't read the image";
            return false;
        }
        for (uint32_t z = 0; z < m_Res; z++) {
            for (uint32_t x = 0; x < m_Res; x++) {
                const float sx = (float)x / (m_Res - 1) * (w - 1), sz = (float)z / (m_Res - 1) * (h - 1);
                const int ix = std::min((int)sx, w - 1), iz = std::min((int)sz, h - 1);
                const int jx = std::min(ix + 1, w - 1), jz = std::min(iz + 1, h - 1);
                const float fx = sx - ix, fz = sz - iz;
                auto P = [&](int a, int b) { return px[size_t(b) * w + a] / 65535.0f; };
                const float v = (P(ix, iz) * (1 - fx) + P(jx, iz) * fx) * (1 - fz) + (P(ix, jz) * (1 - fx) + P(jx, jz) * fx) * fz;
                (*m_Heights)[size_t(z) * m_Res + x] = low + v * range;
            }
        }
        stbi_image_free(px);
        MarkHeights(0, 0, (int)m_Res - 1, (int)m_Res - 1);
        return true;
    }

    std::shared_ptr<TerrainData> TerrainData::Resampled(uint32_t resolution) const {
        auto out = std::make_shared<TerrainData>(resolution);
        const uint32_t r = out->m_Res;
        for (uint32_t z = 0; z < r; z++)
            for (uint32_t x = 0; x < r; x++)
                (*out->m_Heights)[size_t(z) * r + x] = SampleHeight((float)x / (r - 1) * (m_Res - 1), (float)z / (r - 1) * (m_Res - 1));
        const uint32_t s = out->m_SplatRes;
        for (uint32_t z = 0; z < s; z++) {
            for (uint32_t x = 0; x < s; x++) {
                const glm::vec4 w = WeightsAt((x + 0.5f) / s, (z + 0.5f) / s) * 255.0f;
                uint8_t* p = &(*out->m_Splat)[(size_t(z) * s + x) * 4];
                int q[4] = { (int)std::lround(w.x), (int)std::lround(w.y), (int)std::lround(w.z), (int)std::lround(w.w) };
                int big = 0;
                for (int i = 1; i < 4; i++) if (q[i] > q[big]) big = i;
                q[big] = std::clamp(q[big] + 255 - (q[0] + q[1] + q[2] + q[3]), 0, 255);
                for (int i = 0; i < 4; i++) p[i] = (uint8_t)q[i];
            }
        }
        out->MarkAll();
        return out;
    }

    // ---------------------------------------------------------------- ray test
    bool TerrainData::Raycast(const glm::vec2& size, const glm::vec3& o, const glm::vec3& d, float maxT, float& tHit) const {
        // Clip to the terrain's box first
        const glm::vec3 bmin(-size.x * 0.5f, MinHeight() - 0.01f, -size.y * 0.5f);
        const glm::vec3 bmax(size.x * 0.5f, MaxHeight() + 0.01f, size.y * 0.5f);
        float t0 = 0.0f, t1 = maxT;
        for (int a = 0; a < 3; a++) {
            if (std::abs(d[a]) < 1e-12f) {
                if (o[a] < bmin[a] || o[a] > bmax[a]) return false;
                continue;
            }
            float ta = (bmin[a] - o[a]) / d[a], tb = (bmax[a] - o[a]) / d[a];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) return false;
        }
        const float len = glm::length(d);
        if (len <= 0.0f) return false;
        const float step = 0.4f * std::min(size.x, size.y) / (m_Res - 1) / len;   // under half a grid square
        auto above = [&](float t, float& f) {
            const glm::vec3 p = o + d * t;
            float h;
            if (!HeightAt(size, p.x, p.z, h)) { f = 1.0f; return true; }   // off the edge: counts as above
            f = p.y - h;
            return f >= 0.0f;
        };
        float f;
        if (!above(t0, f)) return false;                 // starts under the surface
        float prev = t0;
        for (float t = t0 + step; ; t += step) {
            const bool last = t >= t1;
            if (last) t = t1;
            if (!above(t, f)) {
                float lo = prev, hi = t;                  // the crossing is in (lo, hi]: halve it down
                for (int i = 0; i < 20; i++) {
                    const float mid = 0.5f * (lo + hi);
                    if (above(mid, f)) lo = mid; else hi = mid;
                }
                tHit = hi;
                return true;
            }
            prev = t;
            if (last) return false;
        }
    }

    // ---------------------------------------------------------------- GPU
    void TerrainData::UpdateGPU(const glm::vec2& size) {
        const int n = (int)((m_Res - 1 + ChunkQuads - 1) / ChunkQuads);
        if (m_Chunks.size() != size_t(n) * n) { m_Chunks.assign(size_t(n) * n, Chunk{}); m_ChunkDirty.assign(size_t(n) * n, 1); }
        if (m_ChunkDirty.size() != m_Chunks.size()) m_ChunkDirty.assign(m_Chunks.size(), 1);
        if (size != m_BuiltSize) { std::fill(m_ChunkDirty.begin(), m_ChunkDirty.end(), 1); m_BuiltSize = size; }

        const float g = (float)(m_Res - 1);
        const float dx = size.x / g, dz = size.y / g;
        std::vector<Vertex> verts;
        std::vector<uint32_t> idx;
        for (int cz = 0; cz < n; cz++) {
            for (int cx = 0; cx < n; cx++) {
                const size_t ci = size_t(cz) * n + cx;
                if (!m_ChunkDirty[ci] && m_Chunks[ci].Geometry) continue;
                m_ChunkDirty[ci] = 0;
                const int sx = cx * (int)ChunkQuads, sz = cz * (int)ChunkQuads;
                const int qx = std::min((int)ChunkQuads, (int)m_Res - 1 - sx), qz = std::min((int)ChunkQuads, (int)m_Res - 1 - sz);
                verts.clear(); idx.clear();
                verts.reserve(size_t(qx + 1) * (qz + 1));
                glm::vec3 mn(1e30f), mx(-1e30f);
                for (int z = sz; z <= sz + qz; z++) {
                    for (int x = sx; x <= sx + qx; x++) {
                        Vertex v;
                        v.Position = glm::vec3(x * dx - size.x * 0.5f, At(x, z), z * dz - size.y * 0.5f);
                        v.Normal = glm::normalize(glm::vec3(-(At(x + 1, z) - At(x - 1, z)) / (2.0f * dx), 1.0f,
                                                            -(At(x, z + 1) - At(x, z - 1)) / (2.0f * dz)));
                        v.TexCoord = glm::vec2(x / g, z / g);
                        mn = glm::min(mn, v.Position); mx = glm::max(mx, v.Position);
                        verts.push_back(v);
                    }
                }
                const uint32_t row = (uint32_t)qx + 1;
                for (int z = 0; z < qz; z++) {
                    for (int x = 0; x < qx; x++) {
                        const uint32_t a = z * row + x, b = a + 1, c = a + row, d = c + 1;   // a (x,z) b (x+1,z) c (x,z+1) d (x+1,z+1)
                        idx.insert(idx.end(), { a, c, d, a, d, b });   // counter-clockwise from above, split a-d like Jolt
                    }
                }
                m_Chunks[ci].Geometry = std::make_shared<Mesh>(verts, idx);   // a new mesh: copies keep the old one
                m_Chunks[ci].Min = mn;
                m_Chunks[ci].Max = mx;
            }
        }

        // The paint: a texture of its own once this copy paints (until then it may share its original's)
        if (!m_SplatTex || (m_SplatDirty && !m_SplatOwned)) {
            auto tex = std::make_shared<GLTexture>();
            const int levels = 1 + (int)std::floor(std::log2((float)m_SplatRes));
            glCreateTextures(GL_TEXTURE_2D, 1, &tex->ID);
            glTextureStorage2D(tex->ID, levels, GL_RGBA8, m_SplatRes, m_SplatRes);
            glTextureParameteri(tex->ID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTextureParameteri(tex->ID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(tex->ID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(tex->ID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            m_SplatTex = tex;
            m_SplatOwned = true;
            m_SplatDirty = true;
            m_SplatRect = glm::ivec4(0, 0, (int)m_SplatRes - 1, (int)m_SplatRes - 1);
        }
        if (m_SplatDirty) {
            const glm::ivec4 r = m_SplatRect;
            glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)m_SplatRes);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTextureSubImage2D(m_SplatTex->ID, 0, r.x, r.y, r.z - r.x + 1, r.w - r.y + 1, GL_RGBA, GL_UNSIGNED_BYTE,
                                &(*m_Splat)[(size_t(r.y) * m_SplatRes + r.x) * 4]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glGenerateTextureMipmap(m_SplatTex->ID);
            m_SplatDirty = false;
        }
    }

    uint32_t TerrainData::SplatTexture() const { return m_SplatTex ? m_SplatTex->ID : 0; }

    // ---------------------------------------------------------------- files
    bool TerrainData::Save(const std::string& path) const {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        const std::string tmp = path + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) { ENGINE_ERROR("Terrain: can't write {}", tmp); return false; }
            f.write(kMagic, 4);
            f.write(reinterpret_cast<const char*>(&kVersion), 4);
            f.write(reinterpret_cast<const char*>(&m_Res), 4);
            f.write(reinterpret_cast<const char*>(&m_SplatRes), 4);
            f.write(reinterpret_cast<const char*>(m_Heights->data()), (std::streamsize)(m_Heights->size() * sizeof(float)));
            f.write(reinterpret_cast<const char*>(m_Splat->data()), (std::streamsize)m_Splat->size());
            if (!f) { ENGINE_ERROR("Terrain: writing {} failed", tmp); return false; }
        }
        fs::rename(tmp, path, ec);
        if (ec) {                                         // rename over an existing file can fail on some setups
            fs::remove(path, ec);
            fs::rename(tmp, path, ec);
            if (ec) { ENGINE_ERROR("Terrain: can't replace {}: {}", path, ec.message()); return false; }
        }
        return true;
    }

    std::shared_ptr<TerrainData> TerrainData::Load(const std::string& path, std::string* error) {
        auto fail = [&](const std::string& why) -> std::shared_ptr<TerrainData> { if (error) *error = why; return nullptr; };
        std::ifstream f(path, std::ios::binary);
        if (!f) return fail("can't open the file");
        char magic[4];
        uint32_t version = 0, res = 0, splatRes = 0;
        f.read(magic, 4);
        f.read(reinterpret_cast<char*>(&version), 4);
        f.read(reinterpret_cast<char*>(&res), 4);
        f.read(reinterpret_cast<char*>(&splatRes), 4);
        if (!f || std::memcmp(magic, kMagic, 4) != 0) return fail("not a terrain file");
        if (version != kVersion) return fail("unknown version " + std::to_string(version));
        if (res < MinResolution || res > MaxResolution || splatRes < 1 || splatRes > 4096) return fail("bad size");
        auto d = std::make_shared<TerrainData>(res);
        d->m_SplatRes = splatRes;
        d->m_Splat->assign(size_t(splatRes) * splatRes * 4, 0);
        f.read(reinterpret_cast<char*>(d->m_Heights->data()), (std::streamsize)(d->m_Heights->size() * sizeof(float)));
        f.read(reinterpret_cast<char*>(d->m_Splat->data()), (std::streamsize)d->m_Splat->size());
        if (!f) return fail("the file is cut short");
        d->MarkAll();
        return d;
    }

    // ---------------------------------------------------------------- scene helpers
    bool TerrainSurfaceAt(Scene& scene, float x, float z, float& y, glm::vec3* normal) {
        auto& reg = scene.Registry();
        for (auto [e, t] : reg.view<TerrainComponent>().each()) {
            if (!t.Data) continue;
            const glm::mat4 M = scene.GetWorldMatrix(e);
            const glm::mat4 inv = glm::inverse(M);
            const glm::vec3 down = glm::vec3(inv * glm::vec4(0.0f, -1.0f, 0.0f, 0.0f));
            glm::vec3 local;
            if (std::abs(down.x) + std::abs(down.z) < 1e-5f * std::abs(down.y)) {
                // Not tilted (the usual case): straight down in its own space too
                local = glm::vec3(inv * glm::vec4(x, 0.0f, z, 1.0f));
                float h;
                if (!t.Data->HeightAt(t.Size, local.x, local.z, h)) continue;
                local.y = h;
            }
            else {
                // Tilted: a ray down through it, from above its highest point
                const glm::vec3 top = glm::vec3(M * glm::vec4(0.0f, t.Data->MaxHeight(), 0.0f, 1.0f));
                const float above = std::abs(top.y) + glm::length(t.Size) * glm::length(glm::vec3(M[0])) + 10.0f;
                const glm::vec3 o = glm::vec3(inv * glm::vec4(x, above, z, 1.0f));
                float hit;
                if (!t.Data->Raycast(t.Size, o, down, 1e6f, hit)) continue;
                local = o + down * hit;
            }
            y = (M * glm::vec4(local, 1.0f)).y;
            if (normal) {
                const glm::vec3 n = t.Data->NormalAt(t.Size, local.x, local.z);
                *normal = glm::normalize(glm::transpose(glm::mat3(inv)) * n);
            }
            return true;
        }
        return false;
    }

    uint64_t TerrainsHash(Scene& scene) {
        uint64_t h = 1469598103934665603ull;
        auto& reg = scene.Registry();
        for (auto [e, t] : reg.view<TerrainComponent>().each()) {
            if (!t.Data) continue;
            const glm::mat4 M = scene.GetWorldMatrix(e);
            const TerrainData* data = t.Data.get();          // a copy (undo, a new stroke) counts as a change too
            const uint64_t revision = t.Data->Revision();    // cheap: no pass over the contents every frame
            h = Fnv(h, &M, sizeof(M));
            h = Fnv(h, &t.Size, sizeof(t.Size));
            h = Fnv(h, &data, sizeof(data));
            h = Fnv(h, &revision, sizeof(revision));
        }
        return h;
    }

} // namespace Engine
