#include "Renderer/LightClusters.h"
#include "Renderer/Camera.h"
#include "Renderer/Shader.h"
#include "Renderer/Texture.h"
#include "Core/Log.h"

#include <glad/glad.h>
#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

namespace Engine {

    float PointLightFalloff(float d, float range) {
        if (range <= 0.0f || d >= range) return 0.0f;
        float q = d / range;
        q *= q; q *= q;
        const float w = std::clamp(1.0f - q, 0.0f, 1.0f);
        return w * w / (1.0f + 0.02f * d * d);
    }

    float AutoPointLightRange(const glm::vec3& c) {
        const float peak = std::max({ c.r, c.g, c.b, 0.0f });
        if (peak <= 0.0f) return 0.0f;
        const float d2 = (peak * 100.0f - 1.0f) / 0.02f;
        return std::clamp(d2 > 0.0f ? std::sqrt(d2) : 0.0f, 1.0f, 100.0f);
    }

    void PointLightData::SetCone(float angle, float blend) {
        if (angle <= 0.0f) { CosOuter = -2.0f; CosInner = -1.0f; return; }
        const float half = glm::radians(std::clamp(angle, 1.0f, 179.0f) * 0.5f);
        CosOuter = std::cos(half);
        CosInner = std::max(std::cos(half * (1.0f - std::clamp(blend, 0.0f, 1.0f))), CosOuter + 1e-4f);
    }

    glm::vec4 PointLightBounds(const PointLightData& l) {
        const float r = l.Range;
        if (l.Shape != 0) return glm::vec4(l.Pos, r + l.Extent);              // an area light: its range round any point of it
        if (!l.IsSpot() || l.CosOuter <= 0.0f) return glm::vec4(l.Pos, r);   // a point light, or a cone wider than a hemisphere
        const float c = l.CosOuter, s = std::sqrt(std::max(1.0f - c * c, 0.0f));
        if (c < 0.70710678f) return glm::vec4(l.Pos + l.Dir * (r * c), r * s);            // wider than 90 degrees: round the cap
        const float R = r / (2.0f * c);                                                    // narrow: apex and rim on the sphere
        return glm::vec4(l.Pos + l.Dir * R, R);
    }

    void CookieBasis(const glm::vec3& dir, glm::vec3& right, glm::vec3& up) {
        const glm::vec3 ref = std::abs(dir.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        right = glm::normalize(glm::cross(dir, ref));
        up = glm::cross(right, dir);
    }

    glm::mat4 AtlasFaceMatrix(const glm::vec3& pos, int face, uint32_t tile, float farPlane) {
        static const glm::vec3 dirs[6] = { { 1, 0, 0 },{ -1, 0, 0 },{ 0, 1, 0 },{ 0, -1, 0 },{ 0, 0, 1 },{ 0, 0, -1 } };
        static const glm::vec3 ups[6] = { { 0, -1, 0 },{ 0, -1, 0 },{ 0, 0, 1 },{ 0, 0, -1 },{ 0, -1, 0 },{ 0, -1, 0 } };
        constexpr float kGuard = 3.0f;              // texels each side: the 90 degree face fills tile - 2 x guard
        const float t = (float)tile;
        const float halfTan = t / std::max(t - 2.0f * kGuard, 1.0f);
        const float n = std::clamp(farPlane * 0.002f, 0.02f, 0.1f);
        return glm::perspective(2.0f * std::atan(halfTan), 1.0f, n, std::max(farPlane, n * 2.0f)) * glm::lookAt(pos, pos + dirs[face], ups[face]);
    }

    glm::mat4 SpotShadowMatrix(const PointLightData& l) {
        const float half = std::acos(std::clamp(l.CosOuter, -1.0f, 1.0f));
        const float fov = std::min(2.0f * half + glm::radians(4.0f), glm::radians(150.0f));
        const glm::vec3 up = std::abs(l.Dir.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const float n = std::clamp(l.Range * 0.002f, 0.02f, 0.2f);
        return glm::perspective(fov, 1.0f, n, std::max(l.Range, n * 2.0f)) * glm::lookAt(l.Pos, l.Pos + l.Dir, up);
    }

    // ---------------------------------------------------------------- the shared compute shader
    namespace {
        std::string s_ShaderDir;
        Shader* s_Compute = nullptr;     // never deleted: it would outlive the GL context at exit
        bool s_Tried = false;

        Shader* ComputeShader() {
            static const bool forceCpu = [] { const char* v = std::getenv("MDB_CPU_LIGHT_BINNING"); return v && std::string(v) == "1"; }();
            if (forceCpu) return nullptr;      // test hook: bin on the CPU
            if (!s_Tried && !s_ShaderDir.empty()) {
                s_Tried = true;
                auto sh = Shader::CreateCompute(s_ShaderDir + "/light_clusters.comp");
                if (sh->Valid()) s_Compute = sh.release();
                else ENGINE_WARN("Light clusters: the compute shader didn't build, binning on the CPU");
            }
            return s_Compute;
        }
    }

    void LightClusters::SetShaderDir(const std::string& dir) {
        if (dir != s_ShaderDir) { s_ShaderDir = dir; if (!s_Compute) s_Tried = false; }
    }
    bool LightClusters::GpuAvailable() { return ComputeShader() != nullptr; }
    void LightClusters::ReloadShader() {
        if (s_Compute) s_Compute->Reload();
        else s_Tried = false;
    }

    // Stats buffer: next free index (= indices used), most lights in one cluster, lights seen, spare; then one bit per light
    static constexpr uint32_t kStatsHeader = 4;
    static constexpr uint32_t kStatsBytes = (kStatsHeader + LightClusters::MaxLights / 32) * sizeof(uint32_t);

    LightClusters::LightClusters() {
        glCreateBuffers(3, m_Buffers);
        m_Cells.assign(Count, glm::uvec2(0));
        for (int i = 0; i < MaxDirect; i++) { m_DirectCookie[i] = glm::vec4(-1.0f, 0.0f, -1.0f, 0.0f); m_DirectFrames[i] = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f); }
    }

    LightClusters::~LightClusters() {
        glDeleteBuffers(3, m_Buffers);
        if (m_Readback) { glUnmapNamedBuffer(m_Readback); glDeleteBuffers(1, &m_Readback); }
        if (m_StatsBuffer) glDeleteBuffers(1, &m_StatsBuffer);
        for (void* fence : m_Fence) if (fence) glDeleteSync((GLsync)fence);
        if (m_CookieTex) { glDeleteTextures(1, &m_CookieTex); glDeleteFramebuffers(2, m_CookieFbo); }
        if (m_TileBuffer) glDeleteBuffers(1, &m_TileBuffer);
    }

    void LightClusters::Build(const std::vector<PointLightData>& all, const Camera& camera, bool gpu) {
        // The lights without a shadow slot (those are shaded from uniforms), that give any light
        m_Frame++;
        m_Kept.clear();
        for (const PointLightData& l : all)
            if (!l.Shadows && l.Range > 0.0f && (l.Color.r > 0.0f || l.Color.g > 0.0f || l.Color.b > 0.0f)) m_Kept.push_back(&l);
        size_t count = m_Kept.size();
        if (count > MaxLights) {
            if (!m_Warned) { ENGINE_WARN("{} point lights: only the first {} are drawn", count, MaxLights); m_Warned = true; }
            count = MaxLights;
        }
        auto frames = [](const PointLightData& l) {
            const int cols = std::clamp(l.CookieGrid.x, 1, 64), rows = std::clamp(l.CookieGrid.y, 1, 64);
            const int n = cols * rows, f = ((l.CookieFrame % n) + n) % n;
            return glm::vec3((float)cols, (float)rows, (float)f);
        };
        m_Lights.resize(count);
        for (size_t i = 0; i < count; i++) {
            const PointLightData& l = *m_Kept[i];
            m_Lights[i] = { glm::vec4(l.Pos, l.Range), glm::vec4(l.Color, l.CosInner), glm::vec4(l.Dir, l.CosOuter),
                             glm::vec4((float)l.AtlasSpot, (float)l.AtlasCube, (float)CookieLayer(l.Cookie), l.CookieRoll),
                             glm::vec4((float)CookieLayer(l.Profile), frames(l)), glm::vec4(l.Radius, l.Contact ? 1.0f : 0.0f, (float)l.Shape, l.Extent), glm::vec4(l.AreaX, l.AreaHalfY) };
        }
        // The shadow slot lights (shaded from uniforms, Scene::SetLightingUniforms) take their layers here too, so
        // UploadCookies copies their images with the rest
        for (int i = 0; i < MaxDirect; i++) {
            m_DirectCookie[i] = glm::vec4(-1.0f, 0.0f, -1.0f, 0.0f);
            m_DirectFrames[i] = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
            if (i >= (int)all.size() || !all[i].Shadows) continue;
            const PointLightData& l = all[i];
            m_DirectCookie[i] = glm::vec4((float)CookieLayer(l.Cookie), l.CookieRoll, (float)CookieLayer(l.Profile), 0.0f);
            m_DirectFrames[i] = glm::vec4(frames(l), 0.0f);
        }
        m_LightCount = (uint32_t)count;

        // The slices: logarithmic from the near plane to the far one (at most 500 m: beyond, the last slice)
        const float n = std::max(camera.GetNear(), 0.01f);
        const float f = std::clamp(camera.GetFar(), n * 2.0f, std::max(500.0f, n * 2.0f));
        const float logRatio = std::log(f / n);
        m_ZScale = (float)Slices / logRatio;
        m_ZBias = -(float)Slices * std::log(n) / logRatio;
        const glm::mat4& view = camera.GetView();
        m_View = view;
        m_ViewRow = glm::row(view, 2);
        const float tanV = std::tan(glm::radians(camera.GetFov()) * 0.5f);
        const float tanH = tanV * camera.GetAspect();
        m_Near = n; m_Far = f; m_TanH = tanH; m_TanV = tanV;

        m_Gpu = gpu && count > 0 && GpuAvailable();
        if (m_Gpu) return;        // binned in Upload; the stats come from ReadGpuStats

        auto sliceStart = [&](int s) { return n * std::pow(f / n, (float)s / (float)Slices); };
        auto sliceOf = [&](float z) {
            return std::clamp((int)std::floor(std::log(std::max(z, n)) * m_ZScale + m_ZBias), 0, Slices - 1);
        };
        // A little more than the box, for the sub-pixel jitter (TAA) and rounding
        constexpr float kMargin = 0.004f;

        m_Pairs.clear();
        m_Visible = 0;
        for (uint32_t i = 0; i < (uint32_t)count; i++) {
            const glm::vec4 b = PointLightBounds(*m_Kept[i]);
            const float r = b.w;
            const glm::vec3 c = glm::vec3(view * glm::vec4(glm::vec3(b), 1.0f));
            const float depth = -c.z;                       // in front of the camera = positive
            const float zmin = depth - r, zmax = depth + r;
            if (zmax <= n || zmin >= f * 4.0f) continue;   // behind the camera, or far beyond everything drawn
            const int s0 = sliceOf(std::max(zmin, n)), s1 = sliceOf(zmax);
            bool any = false;
            for (int s = s0; s <= s1; s++) {
                // The part of the sphere inside this slice: its depth range, and the widest circle across it
                const float z0 = std::max({ sliceStart(s), zmin, n });
                const float z1 = s == Slices - 1 ? zmax : std::min(sliceStart(s + 1), zmax);
                if (z1 < z0) continue;
                const float dz = (depth >= z0 && depth <= z1) ? 0.0f : std::min(std::abs(z0 - depth), std::abs(z1 - depth));
                const float rr2 = r * r - dz * dz;
                if (rr2 < 0.0f) continue;
                const float rr = std::sqrt(rr2);
                // That box's corners on the screen (x / depth is extreme at a corner): NDC bounds
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                for (float z : { z0, z1 }) {
                    for (float x : { c.x - rr, c.x + rr }) { const float v = x / (z * tanH); x0 = std::min(x0, v); x1 = std::max(x1, v); }
                    for (float y : { c.y - rr, c.y + rr }) { const float v = y / (z * tanV); y0 = std::min(y0, v); y1 = std::max(y1, v); }
                }
                if (x1 < -1.0f - kMargin || x0 > 1.0f + kMargin || y1 < -1.0f - kMargin || y0 > 1.0f + kMargin) continue;
                auto tile = [](float ndc, int tiles) { return (int)std::floor((ndc * 0.5f + 0.5f) * (float)tiles); };
                const int tx0 = std::clamp(tile(x0 - kMargin, TilesX), 0, TilesX - 1), tx1 = std::clamp(tile(x1 + kMargin, TilesX), 0, TilesX - 1);
                const int ty0 = std::clamp(tile(y0 - kMargin, TilesY), 0, TilesY - 1), ty1 = std::clamp(tile(y1 + kMargin, TilesY), 0, TilesY - 1);
                for (int ty = ty0; ty <= ty1; ty++)
                    for (int tx = tx0; tx <= tx1; tx++)
                        m_Pairs.emplace_back((uint32_t)((s * TilesY + ty) * TilesX + tx), i);
                any = true;
            }
            if (any) m_Visible++;
        }

        // Counting sort by cluster (lights stay in index order within a cluster)
        std::fill(m_Cells.begin(), m_Cells.end(), glm::uvec2(0));
        for (const auto& p : m_Pairs) m_Cells[p.first].y++;
        uint32_t first = 0;
        m_MaxPerCluster = 0;
        for (auto& cell : m_Cells) {
            cell.x = first;
            first += cell.y;
            m_MaxPerCluster = std::max(m_MaxPerCluster, cell.y);
            cell.y = 0;
        }
        m_Indices.resize(m_Pairs.size());
        for (const auto& p : m_Pairs) {
            glm::uvec2& cell = m_Cells[p.first];
            m_Indices[cell.x + cell.y++] = p.second;
        }
        m_References = (uint32_t)m_Indices.size();
    }


    int LightClusters::CookieLayer(const Texture2D* cookie) {
        if (!cookie || !cookie->GetID()) return -1;
        const uint32_t id = cookie->GetID();
        for (int i = 0; i < CookieLayers; i++)
            if (m_Cookies[i].Tex == cookie && m_Cookies[i].Id == id) { m_Cookies[i].LastUsed = m_Frame; return i; }
        // A new image: a free layer, else the one used longest ago (not one this frame needs)
        int best = -1;
        for (int i = 0; i < CookieLayers; i++) {
            if (m_Cookies[i].LastUsed == m_Frame) continue;
            if (best < 0 || !m_Cookies[i].Tex || (m_Cookies[best].Tex && m_Cookies[i].LastUsed < m_Cookies[best].LastUsed)) best = i;
        }
        if (best < 0) {
            if (!m_WarnedCookies) { ENGINE_WARN("More than {} different light cookies and profiles in view: the rest are drawn without", CookieLayers); m_WarnedCookies = true; }
            return -1;
        }
        m_Cookies[best] = { cookie, id, m_Frame, true };
        return best;
    }

    void LightClusters::UploadCookies() {
        bool any = false;
        for (const CookieSlot& s : m_Cookies) any = any || s.Dirty;
        if (!any) return;
        if (!m_CookieTex) {
            int levels = 1;
            while ((CookieSize >> levels) > 0) levels++;
            glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_CookieTex);
            glTextureStorage3D(m_CookieTex, levels, GL_SRGB8_ALPHA8, CookieSize, CookieSize, CookieLayers);
            glTextureParameteri(m_CookieTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTextureParameteri(m_CookieTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(m_CookieTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(m_CookieTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glCreateFramebuffers(2, m_CookieFbo);
        }
        // A scaled copy of each new image (sRGB to sRGB with sRGB writes off: the stored values copy as they are)
        const GLboolean srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_FRAMEBUFFER_SRGB);
        for (CookieSlot& s : m_Cookies) {
            if (!s.Dirty) continue;
            s.Dirty = false;
            const int layer = (int)(&s - m_Cookies);
            glNamedFramebufferTexture(m_CookieFbo[0], GL_COLOR_ATTACHMENT0, s.Id, 0);
            glNamedFramebufferReadBuffer(m_CookieFbo[0], GL_COLOR_ATTACHMENT0);
            glNamedFramebufferTextureLayer(m_CookieFbo[1], GL_COLOR_ATTACHMENT0, m_CookieTex, 0, layer);
            glNamedFramebufferDrawBuffer(m_CookieFbo[1], GL_COLOR_ATTACHMENT0);
            glBlitNamedFramebuffer(m_CookieFbo[0], m_CookieFbo[1], 0, 0, (GLint)s.Tex->GetWidth(), (GLint)s.Tex->GetHeight(),
                                   0, 0, CookieSize, CookieSize, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
        glNamedFramebufferTexture(m_CookieFbo[0], GL_COLOR_ATTACHMENT0, 0, 0);   // don't keep the image attached
        glGenerateTextureMipmap(m_CookieTex);
    }
    void LightClusters::Upload() {
        auto upload = [this](int i, const void* data, size_t bytes) {
            if (bytes > m_Capacity[i]) m_Capacity[i] = std::max(bytes, m_Capacity[i] * 2);
            if (m_Capacity[i] == 0) m_Capacity[i] = 256;
            glNamedBufferData(m_Buffers[i], (GLsizeiptr)m_Capacity[i], nullptr, GL_DYNAMIC_DRAW);   // orphan
            if (bytes) glNamedBufferSubData(m_Buffers[i], 0, (GLsizeiptr)bytes, data);
        };
        UploadCookies();
        upload(0, m_Lights.data(), m_Lights.size() * sizeof(GpuLight));
        if (!m_Tiles.empty() || m_TileBuffer) {
            if (!m_TileBuffer) glCreateBuffers(1, &m_TileBuffer);
            const size_t bytes = std::max<size_t>(m_Tiles.size(), 1) * sizeof(ShadowTileGpu);
            if (bytes > m_TileCapacity) m_TileCapacity = std::max(bytes, m_TileCapacity * 2);
            glNamedBufferData(m_TileBuffer, (GLsizeiptr)m_TileCapacity, nullptr, GL_DYNAMIC_DRAW);   // orphan
            if (!m_Tiles.empty()) glNamedBufferSubData(m_TileBuffer, 0, (GLsizeiptr)(m_Tiles.size() * sizeof(ShadowTileGpu)), m_Tiles.data());
        }
        if (m_Gpu) { m_DispatchPending = true; return; }   // binned by Dispatch (after the shadow passes) or the first Bind
        if (m_LightCount == 0) {                  // nothing clustered: the shaders skip the lookups, no cells needed
            m_Visible = m_MaxPerCluster = m_References = 0;
            m_Indices.clear();
        }
        upload(1, m_Cells.data(), m_Cells.size() * sizeof(glm::uvec2));
        upload(2, m_Indices.data(), m_Indices.size() * sizeof(uint32_t));
    }

    void LightClusters::ReadGpuStats() {
        const int slot = m_StatsSlot;
        if (!m_Fence[slot]) return;
        // Copied three frames ago: normally long done. If the GPU is further behind, skip these stats rather than stall
        const GLenum r = glClientWaitSync((GLsync)m_Fence[slot], 0, 0);
        glDeleteSync((GLsync)m_Fence[slot]);
        m_Fence[slot] = nullptr;
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) return;
        const uint32_t* head = static_cast<const uint32_t*>(m_ReadbackPtr) + slot * kStatsHeader;   // persistent, coherent
        m_References = std::min(head[0], m_GpuIndexCapacity);
        m_MaxPerCluster = head[1];
        m_Visible = head[2];
        if (head[0] > m_GpuIndexCapacity) {          // that frame ran out of room: the next ones get more
            uint32_t grown = m_GpuIndexCapacity;
            while (grown < head[0] + head[0] / 4) grown *= 2;
            if (!m_WarnedFull) { ENGINE_INFO("Light clusters: index list grown to {} entries", grown); m_WarnedFull = true; }
            m_GpuIndexCapacity = grown;
        }
    }

    void LightClusters::Dispatch() {
        if (!m_DispatchPending) return;
        m_DispatchPending = false;
        GLint program = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);   // put back after (a lazy dispatch from Bind runs mid-setup)
        DispatchGpu();
        glUseProgram((GLuint)program);
    }

    void LightClusters::DispatchGpu() {
        Shader* cs = ComputeShader();
        if (!cs) return;
        if (!m_StatsBuffer) {
            // The counters live in GPU memory (the shader's atomics); after each dispatch their header is copied into
            // a slot of a mapped buffer the CPU reads a few frames later (reading the GPU buffer directly would wait
            // for every queued command)
            glCreateBuffers(1, &m_StatsBuffer);
            glNamedBufferStorage(m_StatsBuffer, kStatsBytes, nullptr, 0);
            glCreateBuffers(1, &m_Readback);
            const GLbitfield flags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
            const GLsizeiptr bytes = kStatsRing * kStatsHeader * sizeof(uint32_t);
            glNamedBufferStorage(m_Readback, bytes, nullptr, flags | GL_CLIENT_STORAGE_BIT);
            m_ReadbackPtr = glMapNamedBufferRange(m_Readback, 0, bytes, flags);
        }
        ReadGpuStats();
        if (m_GpuIndexCapacity == 0) m_GpuIndexCapacity = (uint32_t)Count * 32;
        const size_t cellBytes = (size_t)Count * sizeof(glm::uvec2), indexBytes = (size_t)m_GpuIndexCapacity * sizeof(uint32_t);
        if (m_Capacity[1] != cellBytes) { m_Capacity[1] = cellBytes; glNamedBufferData(m_Buffers[1], (GLsizeiptr)cellBytes, nullptr, GL_DYNAMIC_COPY); }
        if (m_Capacity[2] != indexBytes) { m_Capacity[2] = indexBytes; glNamedBufferData(m_Buffers[2], (GLsizeiptr)indexBytes, nullptr, GL_DYNAMIC_COPY); }

        const uint32_t zero = 0;
        glClearNamedBufferData(m_StatsBuffer, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);

        cs->SetMat4("u_View", m_View);
        cs->SetVec4("u_Lens", glm::vec4(m_TanH, m_TanV, m_Near, m_Far));
        cs->SetVec2("u_ClusterZ", glm::vec2(m_ZScale, m_ZBias));
        cs->SetInt("u_LightCount", (int)m_LightCount);
        cs->SetInt("u_Capacity", (int)m_GpuIndexCapacity);
        cs->Bind();
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_Buffers[0]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_Buffers[1]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, m_Buffers[2]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, m_StatsBuffer);
        glDispatchCompute(TilesX, TilesY, 1);            // a work group per screen tile, for all its slices
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);   // the shaders and the copy see the writes
        if (m_ReadbackPtr) {
            glCopyNamedBufferSubData(m_StatsBuffer, m_Readback, 0, (GLintptr)(m_StatsSlot * kStatsHeader * sizeof(uint32_t)),
                                     kStatsHeader * sizeof(uint32_t));
            if (m_Fence[m_StatsSlot]) glDeleteSync((GLsync)m_Fence[m_StatsSlot]);
            m_Fence[m_StatsSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }
        m_StatsSlot = (m_StatsSlot + 1) % kStatsRing;
    }

    void LightClusters::Bind() {
        Dispatch();
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_Buffers[0]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, m_Buffers[1]);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, m_Buffers[2]);
        glBindTextureUnit(20, m_CookieTex);           // cookies and profiles (0 until the first one)
        if (m_TileBuffer) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, m_TileBuffer);   // shadow atlas squares
    }

    std::vector<uint32_t> LightClusters::LightsAt(int x, int y, int slice) const {
        std::vector<uint32_t> out;
        if (m_Gpu || x < 0 || x >= TilesX || y < 0 || y >= TilesY || slice < 0 || slice >= Slices) return out;
        const glm::uvec2 cell = m_Cells[(slice * TilesY + y) * TilesX + x];
        if (cell.x + cell.y > m_Indices.size()) return out;
        out.assign(m_Indices.begin() + cell.x, m_Indices.begin() + cell.x + cell.y);
        return out;
    }

} // namespace Engine
