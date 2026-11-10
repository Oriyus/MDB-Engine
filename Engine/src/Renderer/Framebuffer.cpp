#include "Renderer/Framebuffer.h"
#include "Core/Log.h"

#include <glad/glad.h>
#include <algorithm>

namespace Engine {

    static constexpr uint32_t kMaxSize = 8192;

    Framebuffer::Framebuffer(uint32_t width, uint32_t height)
        : m_Width(std::clamp(width, 1u, kMaxSize)), m_Height(std::clamp(height, 1u, kMaxSize)) {
        Invalidate();
    }

    Framebuffer::~Framebuffer() { Destroy(); }

    void Framebuffer::Destroy() {
        if (m_FBO)      glDeleteFramebuffers(1, &m_FBO);
        if (m_ColorTex) glDeleteTextures(1, &m_ColorTex);
        if (m_IdTex)    glDeleteTextures(1, &m_IdTex);
        if (m_Depth)    glDeleteRenderbuffers(1, &m_Depth);
        if (m_Depth)    glDeleteTextures(1, &m_Depth);
        m_FBO = m_ColorTex = m_IdTex = m_Depth = 0;
    }

    void Framebuffer::Invalidate() {
        Destroy();
        glCreateFramebuffers(1, &m_FBO);

        // Attachment 0: what the user sees
        glCreateTextures(GL_TEXTURE_2D, 1, &m_ColorTex);
        glTextureStorage2D(m_ColorTex, 1, GL_RGBA16F, (GLsizei)m_Width, (GLsizei)m_Height);   // was GL_RGBA8
        glTextureParameteri(m_ColorTex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_ColorTex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_ColorTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_ColorTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glNamedFramebufferTexture(m_FBO, GL_COLOR_ATTACHMENT0, m_ColorTex, 0);

        // Attachment 1: one 32-bit integer per pixel = entity ID
        glCreateTextures(GL_TEXTURE_2D, 1, &m_IdTex);
        glTextureStorage2D(m_IdTex, 1, GL_R32I, (GLsizei)m_Width, (GLsizei)m_Height);
        glTextureParameteri(m_IdTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_IdTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glNamedFramebufferTexture(m_FBO, GL_COLOR_ATTACHMENT1, m_IdTex, 0);

        // Depth + stencil as a texture, so the SSAO pass can read it
        glCreateTextures(GL_TEXTURE_2D, 1, &m_Depth);
        glTextureStorage2D(m_Depth, 1, GL_DEPTH24_STENCIL8, (GLsizei)m_Width, (GLsizei)m_Height);
        glTextureParameteri(m_Depth, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_Depth, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(m_Depth, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_Depth, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glNamedFramebufferTexture(m_FBO, GL_DEPTH_STENCIL_ATTACHMENT, m_Depth, 0);

        const GLenum buffers[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glNamedFramebufferDrawBuffers(m_FBO, 2, buffers);

        if (glCheckNamedFramebufferStatus(m_FBO, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            ENGINE_ERROR("Framebuffer is incomplete ({}x{})", m_Width, m_Height);
    }

    void Framebuffer::Resize(uint32_t width, uint32_t height) {
        width = std::clamp(width, 1u, kMaxSize);
        height = std::clamp(height, 1u, kMaxSize);
        if (width == m_Width && height == m_Height) return;
        m_Width = width;
        m_Height = height;
        Invalidate();
    }

    void Framebuffer::Bind() const {
        glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
        glViewport(0, 0, (GLsizei)m_Width, (GLsizei)m_Height);
    }

    void Framebuffer::Unbind() { glBindFramebuffer(GL_FRAMEBUFFER, 0); }

    void Framebuffer::Clear(float r, float g, float b, float a) const {
        const float color[4] = { r, g, b, a };
        const GLint noEntity = -1;
        glClearNamedFramebufferfv(m_FBO, GL_COLOR, 0, color);
        glClearNamedFramebufferiv(m_FBO, GL_COLOR, 1, &noEntity);   // "1" = draw buffer index 1
        glClearNamedFramebufferfi(m_FBO, GL_DEPTH_STENCIL, 0, 1.0f, 0);
    }

    int Framebuffer::ReadEntityID(int x, int y) const {
        if (x < 0 || y < 0 || x >= (int)m_Width || y >= (int)m_Height) return -1;
        GLint id = -1;
        glNamedFramebufferReadBuffer(m_FBO, GL_COLOR_ATTACHMENT1);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_FBO);
        glReadPixels(x, y, 1, 1, GL_RED_INTEGER, GL_INT, &id);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        return id;
    }

    std::vector<int> Framebuffer::ReadEntityIDs(int x, int y, int w, int h) const {
        const int x0 = std::max(x, 0), y0 = std::max(y, 0);
        const int x1 = std::min(x + w, (int)m_Width), y1 = std::min(y + h, (int)m_Height);
        std::vector<int> ids;
        if (x1 <= x0 || y1 <= y0) return ids;
        std::vector<GLint> px(size_t(x1 - x0) * size_t(y1 - y0));
        glNamedFramebufferReadBuffer(m_FBO, GL_COLOR_ATTACHMENT1);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_FBO);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(x0, y0, x1 - x0, y1 - y0, GL_RED_INTEGER, GL_INT, px.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        std::sort(px.begin(), px.end());
        px.erase(std::unique(px.begin(), px.end()), px.end());
        for (GLint id : px) if (id >= 0) ids.push_back(id);
        return ids;
    }

    float Framebuffer::ReadDepth(int x, int y) const {
        if (x < 0 || y < 0 || x >= (int)m_Width || y >= (int)m_Height) return 1.0f;
        float depth = 1.0f;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_FBO);
        glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        return depth;
    }

} // namespace Engine