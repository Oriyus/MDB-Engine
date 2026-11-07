#include "Renderer/PostProcess.h"
#include "Renderer/Shader.h"
#include "Core/Log.h"

#include <glad/glad.h>
#include <algorithm>

namespace Engine {

    PostProcess::PostProcess(uint32_t width, uint32_t height)
        : m_Width(std::max(width, 1u)), m_Height(std::max(height, 1u)) {
        glCreateVertexArrays(1, &m_VAO);   // core profile needs a VAO bound even with no vertex data
        Invalidate();
    }

    PostProcess::~PostProcess() {
        Destroy();
        glDeleteVertexArrays(1, &m_VAO);
    }

    void PostProcess::Destroy() {
        if (m_FBO) glDeleteFramebuffers(1, &m_FBO);
        if (m_Tex) glDeleteTextures(1, &m_Tex);
        m_FBO = m_Tex = 0;
    }

    void PostProcess::Invalidate() {
        Destroy();
        glCreateTextures(GL_TEXTURE_2D, 1, &m_Tex);
        glTextureStorage2D(m_Tex, 1, GL_RGBA8, (GLsizei)m_Width, (GLsizei)m_Height);
        glTextureParameteri(m_Tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(m_Tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_Tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_Tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glCreateFramebuffers(1, &m_FBO);
        glNamedFramebufferTexture(m_FBO, GL_COLOR_ATTACHMENT0, m_Tex, 0);
        if (glCheckNamedFramebufferStatus(m_FBO, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            ENGINE_ERROR("Post-process framebuffer is incomplete");
    }

    void PostProcess::Resize(uint32_t width, uint32_t height) {
        width = std::max(width, 1u);
        height = std::max(height, 1u);
        if (width == m_Width && height == m_Height) return;
        m_Width = width;
        m_Height = height;
        Invalidate();
    }

    void PostProcess::Apply(uint32_t hdr, uint32_t ao, uint32_t bloom, Shader& shader, const PostSettings& s) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
        glViewport(0, 0, (GLsizei)m_Width, (GLsizei)m_Height);

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);

        shader.Bind();
        shader.SetInt("u_Scene", 0);
        shader.SetInt("u_AO", 1);
        shader.SetInt("u_Bloom", 2);
        shader.SetFloat("u_Exposure", s.Exposure);
        shader.SetInt("u_ToneMap", s.ToneMap);
        shader.SetInt("u_Gamma", s.Gamma ? 1 : 0);

        shader.SetInt("u_UseAO", (s.AOEnabled && ao) ? 1 : 0);
        shader.SetFloat("u_AOStrength", s.AOStrength);
        shader.SetInt("u_AODebug", s.AODebug ? 1 : 0);
        shader.SetInt("u_UseBloom", (s.BloomEnabled && bloom) ? 1 : 0);
        shader.SetFloat("u_BloomIntensity", s.BloomIntensity);

        glBindTextureUnit(0, hdr);
        glBindTextureUnit(1, ao);       // 0 simply unbinds when the effect is off
        glBindTextureUnit(2, bloom);
        glBindVertexArray(m_VAO);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

} // namespace Engine