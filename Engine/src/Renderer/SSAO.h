#pragma once
#include <cstdint>
#include <glm/glm.hpp>

namespace Engine {

    class Shader;

    class SSAO {
    public:
        SSAO(uint32_t width, uint32_t height);
        ~SSAO();

        SSAO(const SSAO&) = delete;
        SSAO& operator=(const SSAO&) = delete;

        void Resize(uint32_t width, uint32_t height);

        // Run after the scene is rendered and its framebuffer is unbound
        void Compute(uint32_t depthTexture, const glm::mat4& projection, Shader& aoShader, Shader& blurShader);

        uint32_t GetOutput() const { return m_BlurTex; }   // 1 = open, 0 = fully occluded

        float Radius = 0.5f;     // view-space units: how far to look for occluders
        float Bias = 0.025f;   // raise it if flat surfaces look speckled
        int   Samples = 16;

    private:
        void Invalidate();
        void Destroy();

        uint32_t m_RawTex = 0, m_RawFBO = 0, m_BlurTex = 0, m_BlurFBO = 0, m_VAO = 0;
        uint32_t m_Width = 1, m_Height = 1;
    };

} // namespace Engine