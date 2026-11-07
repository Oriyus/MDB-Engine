#include "Renderer/SSAO.h"
#include "Renderer/Shader.h"
#include "Core/Log.h"

#include <glad/glad.h>
#include <algorithm>

namespace Engine {

    static void MakeTarget(uint32_t w, uint32_t h, uint32_t& tex, uint32_t& fbo) {
        glCreateTextures(GL_TEXTURE_2D, 1, &tex);
        glTextureStorage2D(tex, 1, GL_R8, (GLsizei)w, (GLsizei)h);
        glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glCreateFramebuffers(1, &fbo);
        glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, tex, 0);
        if (glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            ENGINE_ERROR("SSAO framebuffer is incomplete");
    }

    SSAO::SSAO(uint32_t width, uint32_t height)
        : m_Width(std::max(width, 1u)), m_Height(std::max(height, 1u)) {
        glCreateVertexArrays(1, &m_VAO);
        Invalidate();
    }

    SSAO::~SSAO() {
        Destroy();
        glDeleteVertexArrays(1, &m_VAO);
    }

    void SSAO::Destroy() {
        if (m_RawFBO)  glDeleteFramebuffers(1, &m_RawFBO);
        if (m_BlurFBO) glDeleteFramebuffers(1, &m_BlurFBO);
        if (m_RawTex)  glDeleteTextures(1, &m_RawTex);
        if (m_BlurTex) glDeleteTextures(1, &m_BlurTex);
        m_RawFBO = m_BlurFBO = m_RawTex = m_BlurTex = 0;
    }

    void SSAO::Invalidate() {
        Destroy();
        MakeTarget(m_Width, m_Height, m_RawTex, m_RawFBO);
        MakeTarget(m_Width, m_Height, m_BlurTex, m_BlurFBO);
    }

    void SSAO::Resize(uint32_t width, uint32_t height) {
        width = std::max(width, 1u);
        height = std::max(height, 1u);
        if (width == m_Width && height == m_Height) return;
        m_Width = width;
        m_Height = height;
        Invalidate();
    }

    void SSAO::Compute(uint32_t depthTexture, const glm::mat4& projection, Shader& aoShader, Shader& blurShader) {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glBindVertexArray(m_VAO);
        glViewport(0, 0, (GLsizei)m_Width, (GLsizei)m_Height);

        // 1) raw occlusion from depth
        glBindFramebuffer(GL_FRAMEBUFFER, m_RawFBO);
        aoShader.Bind();
        aoShader.SetInt("u_Depth", 0);
        aoShader.SetMat4("u_Proj", projection);
        aoShader.SetMat4("u_InvProj", glm::inverse(projection));
        aoShader.SetFloat("u_Radius", Radius);
        aoShader.SetFloat("u_Bias", Bias);
        aoShader.SetInt("u_Samples", std::clamp(Samples, 1, 64));
        glBindTextureUnit(0, depthTexture);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // 2) blur away the per-pixel noise
        glBindFramebuffer(GL_FRAMEBUFFER, m_BlurFBO);
        blurShader.Bind();
        blurShader.SetInt("u_AO", 0);
        glBindTextureUnit(0, m_RawTex);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
    }

} // namespace Engine