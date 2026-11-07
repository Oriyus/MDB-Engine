#include "Renderer/Bloom.h"
#include "Renderer/Shader.h"
#include "Core/Log.h"

#include <glad/glad.h>
#include <algorithm>

namespace Engine {

    Bloom::Bloom(uint32_t width, uint32_t height)
        : m_Width(std::max(width, 1u)), m_Height(std::max(height, 1u)) {
        glCreateVertexArrays(1, &m_VAO);
        Invalidate();
    }

    Bloom::~Bloom() {
        Destroy();
        glDeleteVertexArrays(1, &m_VAO);
    }

    void Bloom::Destroy() {
        for (auto& l : m_Levels) {
            if (l.FBO) glDeleteFramebuffers(1, &l.FBO);
            if (l.Tex) glDeleteTextures(1, &l.Tex);
            l = Level{};
        }
    }

    void Bloom::Invalidate() {
        Destroy();
        uint32_t w = std::max(m_Width / 2, 1u);     // level 0 is half resolution
        uint32_t h = std::max(m_Height / 2, 1u);
        for (auto& l : m_Levels) {
            l.W = w;
            l.H = h;
            glCreateTextures(GL_TEXTURE_2D, 1, &l.Tex);
            glTextureStorage2D(l.Tex, 1, GL_RGBA16F, (GLsizei)w, (GLsizei)h);
            glTextureParameteri(l.Tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTextureParameteri(l.Tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(l.Tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTextureParameteri(l.Tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            glCreateFramebuffers(1, &l.FBO);
            glNamedFramebufferTexture(l.FBO, GL_COLOR_ATTACHMENT0, l.Tex, 0);
            if (glCheckNamedFramebufferStatus(l.FBO, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                ENGINE_ERROR("Bloom framebuffer is incomplete ({}x{})", w, h);

            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
        }
    }

    void Bloom::Resize(uint32_t width, uint32_t height) {
        width = std::max(width, 1u);
        height = std::max(height, 1u);
        if (width == m_Width && height == m_Height) return;
        m_Width = width;
        m_Height = height;
        Invalidate();
    }

    void Bloom::Compute(uint32_t hdrTexture, Shader& down, Shader& up) {
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        glBindVertexArray(m_VAO);

        // Downsample chain: scene -> level 0 (with threshold) -> level 1 -> ... each half the size
        down.Bind();
        down.SetInt("u_Src", 0);
        down.SetFloat("u_Threshold", Threshold);
        down.SetFloat("u_Knee", Knee);
        down.SetFloat("u_MaxBrightness", MaxBrightness);

        uint32_t src = hdrTexture;
        for (uint32_t i = 0; i < Levels; i++) {
            glBindFramebuffer(GL_FRAMEBUFFER, m_Levels[i].FBO);
            glViewport(0, 0, (GLsizei)m_Levels[i].W, (GLsizei)m_Levels[i].H);
            down.SetInt("u_FirstPass", i == 0 ? 1 : 0);
            glBindTextureUnit(0, src);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            src = m_Levels[i].Tex;
        }

        // Upsample chain: blur each level and ADD it onto the larger one above it
        up.Bind();
        up.SetInt("u_Src", 0);
        up.SetFloat("u_Radius", Radius);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        for (int i = (int)Levels - 2; i >= 0; i--) {
            glBindFramebuffer(GL_FRAMEBUFFER, m_Levels[i].FBO);
            glViewport(0, 0, (GLsizei)m_Levels[i].W, (GLsizei)m_Levels[i].H);
            glBindTextureUnit(0, m_Levels[i + 1].Tex);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glDisable(GL_BLEND);

        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
    }

} // namespace Engine