#include "Renderer/ShadowMap.h"
#include "Core/Log.h"
#include <glad/glad.h>

namespace Engine {

    // ------------------------------------------------------------ point lights
    PointShadowArray::PointShadowArray(uint32_t size) : m_Size(size) {
        glCreateTextures(GL_TEXTURE_CUBE_MAP_ARRAY, 1, &m_Tex);
        // depth parameter of a cubemap array = layers * 6
        glTextureStorage3D(m_Tex, 1, GL_DEPTH_COMPONENT32F, (GLsizei)size, (GLsizei)size, (GLsizei)(MaxLights * 6));
        glTextureParameteri(m_Tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_Tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(m_Tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_Tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_Tex, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

        glCreateFramebuffers(1, &m_FBO);
        glNamedFramebufferTextureLayer(m_FBO, GL_DEPTH_ATTACHMENT, m_Tex, 0, 0);
        glNamedFramebufferDrawBuffer(m_FBO, GL_NONE);
        glNamedFramebufferReadBuffer(m_FBO, GL_NONE);

        if (glCheckNamedFramebufferStatus(m_FBO, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            ENGINE_ERROR("Point shadow framebuffer is incomplete");
    }

    PointShadowArray::~PointShadowArray() {
        glDeleteFramebuffers(1, &m_FBO);
        glDeleteTextures(1, &m_Tex);
    }

    void PointShadowArray::BindFace(uint32_t light, uint32_t face) const {
        // In a cubemap array, layer-face index = light * 6 + face  (+X, -X, +Y, -Y, +Z, -Z)
        glNamedFramebufferTextureLayer(m_FBO, GL_DEPTH_ATTACHMENT, m_Tex, 0, (GLint)(light * 6 + face));
        glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
        glViewport(0, 0, (GLsizei)m_Size, (GLsizei)m_Size);
        const float one = 1.0f;
        glClearNamedFramebufferfv(m_FBO, GL_DEPTH, 0, &one);
    }

    void PointShadowArray::BindTexture(uint32_t slot) const { glBindTextureUnit(slot, m_Tex); }

    // ------------------------------------------------------------ sun
    DirectionalShadowMap::DirectionalShadowMap(uint32_t size) : m_Size(size) {
        glCreateTextures(GL_TEXTURE_2D, 1, &m_Tex);
        glTextureStorage2D(m_Tex, 1, GL_DEPTH_COMPONENT32F, (GLsizei)size, (GLsizei)size);
        glTextureParameteri(m_Tex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_Tex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        // Outside the sun's area counts as "nothing in the way" = lit
        glTextureParameteri(m_Tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTextureParameteri(m_Tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        const float border[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        glTextureParameterfv(m_Tex, GL_TEXTURE_BORDER_COLOR, border);

        glCreateFramebuffers(1, &m_FBO);
        glNamedFramebufferTexture(m_FBO, GL_DEPTH_ATTACHMENT, m_Tex, 0);
        glNamedFramebufferDrawBuffer(m_FBO, GL_NONE);
        glNamedFramebufferReadBuffer(m_FBO, GL_NONE);

        if (glCheckNamedFramebufferStatus(m_FBO, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            ENGINE_ERROR("Sun shadow framebuffer is incomplete");
    }

    DirectionalShadowMap::~DirectionalShadowMap() {
        glDeleteFramebuffers(1, &m_FBO);
        glDeleteTextures(1, &m_Tex);
    }

    void DirectionalShadowMap::Bind() const {
        glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
        glViewport(0, 0, (GLsizei)m_Size, (GLsizei)m_Size);
        const float one = 1.0f;
        glClearNamedFramebufferfv(m_FBO, GL_DEPTH, 0, &one);
    }

    void DirectionalShadowMap::BindTexture(uint32_t slot) const { glBindTextureUnit(slot, m_Tex); }

} // namespace Engine