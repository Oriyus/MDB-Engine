#include "Renderer/VolumetricFog.h"
#include "Renderer/Shader.h"
#include "Renderer/Camera.h"
#include "Scene/Scene.h"

#include <glad/glad.h>
#include <algorithm>
#include <cmath>

namespace Engine {

    VolumetricFog::VolumetricFog() {
        glCreateVertexArrays(1, &m_VAO);
    }

    VolumetricFog::~VolumetricFog() {
        if (m_FBO) glDeleteFramebuffers(1, &m_FBO);
        if (m_Texture) glDeleteTextures(1, &m_Texture);
        if (m_VAO) glDeleteVertexArrays(1, &m_VAO);
    }

    void VolumetricFog::Resize(uint32_t width, uint32_t height) {
        const uint32_t w = std::max(1u, (width + 1) / 2), h = std::max(1u, (height + 1) / 2);
        if (m_Texture && w == m_Width && h == m_Height) return;
        if (m_FBO) glDeleteFramebuffers(1, &m_FBO);
        if (m_Texture) glDeleteTextures(1, &m_Texture);
        glCreateTextures(GL_TEXTURE_2D, 1, &m_Texture);
        glTextureStorage2D(m_Texture, 1, GL_RGBA16F, (GLsizei)w, (GLsizei)h);   // rgb in-scattered light, a transmittance
        glTextureParameteri(m_Texture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(m_Texture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTextureParameteri(m_Texture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_Texture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glCreateFramebuffers(1, &m_FBO);
        glNamedFramebufferTexture(m_FBO, GL_COLOR_ATTACHMENT0, m_Texture, 0);
        m_Width = w;
        m_Height = h;
    }

    void VolumetricFog::Render(Scene& scene, const Camera& camera, Shader& march, Shader& composite, const DrawList& list,
                               const ShadowContext* shadows, const Environment* env, uint32_t depthTex,
                               uint32_t width, uint32_t height, const VolumetricFogSettings& s) {
        const auto now = std::chrono::steady_clock::now();
        m_Time += std::clamp(std::chrono::duration<float>(now - m_Last).count(), 0.0f, 0.1f);
        m_Last = now;
        Resize(width, height);

        GLint target = 0, viewport[4];
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &target);
        glGetIntegerv(GL_VIEWPORT, viewport);

        // 1) March at half resolution
        march.QuietMissing = true;                  // it takes only part of the lighting uniforms
        march.Bind();
        scene.SetLightingUniforms(camera, march, list, shadows, env);
        march.SetInt("u_Depth", 9);
        march.SetMat4("u_InvViewProjection", glm::inverse(camera.GetViewProjection()));
        march.SetFloat("u_Density", std::max(s.Density, 0.0f));
        march.SetFloat("u_HeightFalloff", std::max(s.HeightFalloff, 0.05f));
        march.SetFloat("u_BaseHeight", s.BaseHeight);
        march.SetVec3("u_FogColor", glm::pow(glm::clamp(s.Color, glm::vec3(0.0f), glm::vec3(1.0f)), glm::vec3(2.2f)));
        march.SetFloat("u_Anisotropy", std::clamp(s.Anisotropy, -0.95f, 0.95f));
        march.SetFloat("u_SunScale", s.SunIntensity);
        march.SetFloat("u_AmbientScale", s.AmbientIntensity);
        march.SetFloat("u_PointScale", s.PointLightIntensity);
        march.SetFloat("u_NoiseScale", std::max(s.NoiseScale, 0.001f));
        march.SetFloat("u_NoiseAmount", std::clamp(s.NoiseAmount, 0.0f, 1.0f));
        const float a = glm::radians(s.WindDirection);
        march.SetVec3("u_Wind", glm::vec3(std::cos(a), 0.05f, std::sin(a)) * (s.WindSpeed * m_Time));
        march.SetFloat("u_MaxDistance", std::max(s.MaxDistance, 1.0f));
        march.SetInt("u_Steps", std::clamp(s.Steps, 8, 96));
        glBindTextureUnit(9, depthTex);

        glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
        glViewport(0, 0, (GLsizei)m_Width, (GLsizei)m_Height);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_BLEND);
        glBindVertexArray(m_VAO);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // 2) Blend into the target: colour x transmittance + in-scattered light
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)target);
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        composite.Bind();
        composite.SetInt("u_Fog", 10);
        composite.SetInt("u_Depth", 9);
        composite.SetMat4("u_InvProjection", glm::inverse(camera.GetProjection()));
        glBindTextureUnit(10, m_Texture);
        glEnable(GL_BLEND);
        glBlendFunci(0, GL_ONE, GL_SRC_ALPHA);
        glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);   // entity IDs stay (clicks pick through fog)
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_BLEND);
        glBindVertexArray(0);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
    }

} // namespace Engine
