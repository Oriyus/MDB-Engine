#include "Renderer/ShadowMap.h"
#include "Core/Log.h"
#include <glad/glad.h>
#include <algorithm>

namespace Engine {

    // ------------------------------------------------------------ the shadow atlas
    ShadowAtlas::ShadowAtlas(uint32_t size) { Resize(size); }

    ShadowAtlas::~ShadowAtlas() { Release(); }

    void ShadowAtlas::Release() {
        if (m_FBO) glDeleteFramebuffers(1, &m_FBO);
        if (m_Tex) glDeleteTextures(1, &m_Tex);
        if (m_RawSampler) glDeleteSamplers(1, &m_RawSampler);
        if (m_DebugView) glDeleteTextures(1, &m_DebugView);
        m_FBO = m_Tex = m_RawSampler = m_DebugView = 0;
    }

    void ShadowAtlas::Resize(uint32_t size) {
        uint32_t s = 1024;
        while (s < size && s < 8192) s <<= 1;
        if (s != m_Size) Release();                 // made again at the new size on next use
        m_Size = s;
        m_Used = 0;
        m_Free.assign((size_t)Level(MinTile) + 1, {});
        m_Free[0].push_back(glm::uvec2(0));
    }

    int ShadowAtlas::Level(uint32_t tile) const {
        int level = 0;
        for (uint32_t s = m_Size; s > tile && s > MinTile; s >>= 1) level++;
        return level;
    }

    bool ShadowAtlas::Alloc(uint32_t tile, glm::uvec2& at) {
        if (tile < MinTile || tile > m_Size) return false;
        const int level = Level(tile);
        // The smallest free square at least this big, split down to this size (the other three quarters go free)
        int from = level;
        while (from >= 0 && m_Free[(size_t)from].empty()) from--;
        if (from < 0) return false;
        glm::uvec2 p = m_Free[(size_t)from].back();
        m_Free[(size_t)from].pop_back();
        for (int l = from + 1; l <= level; l++) {
            const uint32_t h = m_Size >> l;
            m_Free[(size_t)l].push_back(p + glm::uvec2(h, 0));
            m_Free[(size_t)l].push_back(p + glm::uvec2(0, h));
            m_Free[(size_t)l].push_back(p + glm::uvec2(h, h));
        }
        at = p;
        m_Used += (uint64_t)tile * tile;
        return true;
    }

    void ShadowAtlas::Free(const glm::uvec2& at, uint32_t tile) {
        int level = Level(tile);
        m_Used -= std::min<uint64_t>(m_Used, (uint64_t)tile * tile);
        glm::uvec2 p = at;
        // While its three neighbours (the other quarters of the square above) are free too, merge them
        while (level > 0) {
            const uint32_t h = m_Size >> level;
            const glm::uvec2 parent(p.x & ~(2 * h - 1), p.y & ~(2 * h - 1));
            auto& list = m_Free[(size_t)level];
            int found[3], n = 0;
            for (const glm::uvec2 q : { parent, parent + glm::uvec2(h, 0), parent + glm::uvec2(0, h), parent + glm::uvec2(h, h) }) {
                if (q == p) continue;
                for (int i = 0; i < (int)list.size(); i++) if (list[(size_t)i] == q) { found[n++] = i; break; }
            }
            if (n < 3) break;
            std::sort(found, found + 3);
            for (int k = 2; k >= 0; k--) { list[(size_t)found[k]] = list.back(); list.pop_back(); }
            p = parent;
            level--;
        }
        m_Free[(size_t)level].push_back(p);
    }

    void ShadowAtlas::Allocate() const {
        glCreateTextures(GL_TEXTURE_2D, 1, &m_Tex);
        glTextureStorage2D(m_Tex, 1, GL_DEPTH_COMPONENT16, (GLsizei)m_Size, (GLsizei)m_Size);
        glTextureParameteri(m_Tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_Tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_Tex, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTextureParameteri(m_Tex, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glTextureParameteri(m_Tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_Tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glCreateFramebuffers(1, &m_FBO);
        glNamedFramebufferTexture(m_FBO, GL_DEPTH_ATTACHMENT, m_Tex, 0);
        glNamedFramebufferDrawBuffer(m_FBO, GL_NONE);
        glNamedFramebufferReadBuffer(m_FBO, GL_NONE);
        if (glCheckNamedFramebufferStatus(m_FBO, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            ENGINE_ERROR("Shadow atlas framebuffer is incomplete");
        const float one = 1.0f;
        glClearNamedFramebufferfv(m_FBO, GL_DEPTH, 0, &one);
    }

    void ShadowAtlas::BindTile(const glm::uvec2& at, uint32_t tile) const {
        if (!m_Tex) Allocate();
        glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
        glViewport((GLint)at.x, (GLint)at.y, (GLsizei)tile, (GLsizei)tile);
        glEnable(GL_SCISSOR_TEST);                 // the clear (and nothing drawn) leaves the square
        glScissor((GLint)at.x, (GLint)at.y, (GLsizei)tile, (GLsizei)tile);
        const float one = 1.0f;
        glClearNamedFramebufferfv(m_FBO, GL_DEPTH, 0, &one);
    }

    uint32_t ShadowAtlas::DebugView() const {
        if (!m_Tex) return 0;
        if (!m_DebugView) {
            glGenTextures(1, &m_DebugView);             // a view needs a fresh name (glGen, not glCreate)
            glTextureView(m_DebugView, GL_TEXTURE_2D, m_Tex, GL_DEPTH_COMPONENT16, 0, 1, 0, 1);
            glTextureParameteri(m_DebugView, GL_TEXTURE_COMPARE_MODE, GL_NONE);
            glTextureParameteri(m_DebugView, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(m_DebugView, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            const GLint grey[4] = { GL_RED, GL_RED, GL_RED, GL_ONE };
            glTextureParameteriv(m_DebugView, GL_TEXTURE_SWIZZLE_RGBA, grey);
        }
        return m_DebugView;
    }

    void ShadowAtlas::EndTiles() const { glDisable(GL_SCISSOR_TEST); }

    void ShadowAtlas::BindTexture(uint32_t unit, uint32_t rawUnit) const {
        glBindTextureUnit(unit, m_Tex);
        if (!m_Tex) return;
        if (!m_RawSampler) {
            glCreateSamplers(1, &m_RawSampler);
            glSamplerParameteri(m_RawSampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
            glSamplerParameteri(m_RawSampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glSamplerParameteri(m_RawSampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glSamplerParameteri(m_RawSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glSamplerParameteri(m_RawSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTextureUnit(rawUnit, m_Tex);
        glBindSampler(rawUnit, m_RawSampler);
    }

    // ------------------------------------------------------------ sun
    DirectionalShadowMap::DirectionalShadowMap(uint32_t size) : m_Size(size) {
        glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_Tex);
        glTextureStorage3D(m_Tex, 1, GL_DEPTH_COMPONENT32F, (GLsizei)size, (GLsizei)size, (GLsizei)MaxCascades);
        glTextureParameteri(m_Tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_Tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        // Outside the sun's area counts as "nothing in the way" = lit
        glTextureParameteri(m_Tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTextureParameteri(m_Tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        const float border[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        glTextureParameterfv(m_Tex, GL_TEXTURE_BORDER_COLOR, border);

        glCreateFramebuffers(1, &m_FBO);
        glNamedFramebufferTextureLayer(m_FBO, GL_DEPTH_ATTACHMENT, m_Tex, 0, 0);
        glNamedFramebufferDrawBuffer(m_FBO, GL_NONE);
        glNamedFramebufferReadBuffer(m_FBO, GL_NONE);

        if (glCheckNamedFramebufferStatus(m_FBO, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            ENGINE_ERROR("Sun shadow framebuffer is incomplete");
    }

    DirectionalShadowMap::~DirectionalShadowMap() {
        glDeleteFramebuffers(1, &m_FBO);
        glDeleteTextures(1, &m_Tex);
        if (m_StaticTex) glDeleteTextures(1, &m_StaticTex);
    }

    void DirectionalShadowMap::Bind(uint32_t cascade) const {
        glNamedFramebufferTextureLayer(m_FBO, GL_DEPTH_ATTACHMENT, m_Tex, 0, (GLint)cascade);
        glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
        glViewport(0, 0, (GLsizei)m_Size, (GLsizei)m_Size);
        const float one = 1.0f;
        glClearNamedFramebufferfv(m_FBO, GL_DEPTH, 0, &one);
    }

    void DirectionalShadowMap::BindStatic(uint32_t cascade) const {
        if (!m_StaticTex) {
            glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_StaticTex);
            glTextureStorage3D(m_StaticTex, 1, GL_DEPTH_COMPONENT32F, (GLsizei)m_Size, (GLsizei)m_Size, (GLsizei)MaxCascades);
        }
        glNamedFramebufferTextureLayer(m_FBO, GL_DEPTH_ATTACHMENT, m_StaticTex, 0, (GLint)cascade);
        glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
        glViewport(0, 0, (GLsizei)m_Size, (GLsizei)m_Size);
        const float one = 1.0f;
        glClearNamedFramebufferfv(m_FBO, GL_DEPTH, 0, &one);
    }

    void DirectionalShadowMap::RestoreStatic(uint32_t cascade) const {
        if (m_StaticTex)
            glCopyImageSubData(m_StaticTex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, (GLint)cascade, m_Tex, GL_TEXTURE_2D_ARRAY, 0, 0, 0, (GLint)cascade,
                               (GLsizei)m_Size, (GLsizei)m_Size, 1);
        glNamedFramebufferTextureLayer(m_FBO, GL_DEPTH_ATTACHMENT, m_Tex, 0, (GLint)cascade);
        glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
        glViewport(0, 0, (GLsizei)m_Size, (GLsizei)m_Size);
    }

    void DirectionalShadowMap::BindTexture(uint32_t slot) const { glBindTextureUnit(slot, m_Tex); }

} // namespace Engine