#pragma once
#include <cstdint>

namespace Engine {

    class Shader;

    struct PostSettings {
        float Exposure = 1.0f;
        int   ToneMap = 2;           // 0 = clamp, 1 = Reinhard, 2 = ACES
        bool  Gamma = true;

        bool  AOEnabled = true;
        float AOStrength = 1.0f;
        bool  AODebug = false;       // show the AO buffer alone

        bool  BloomEnabled = true;
        float BloomIntensity = 0.12f;

        bool FXAA = true;
    };

    // Turns an HDR texture into a displayable 8-bit one
    class PostProcess {
    public:
        PostProcess(uint32_t width, uint32_t height);
        ~PostProcess();

        PostProcess(const PostProcess&) = delete;
        PostProcess& operator=(const PostProcess&) = delete;

        void Resize(uint32_t width, uint32_t height);
        void Apply(uint32_t hdrTexture, uint32_t aoTexture, uint32_t bloomTexture,
            Shader& shader, const PostSettings& settings);

        // Runs after Apply(). When disabled, GetOutput() keeps returning the unsmoothed image.
        void ApplyFXAA(Shader& shader, bool enabled);

        uint32_t GetOutput() const { return m_UseAA ? m_AATex : m_Tex; }
    private:
        void Invalidate();
        void Destroy();

        uint32_t m_FBO = 0, m_Tex = 0, m_VAO = 0;
        uint32_t m_Width = 1, m_Height = 1;
        uint32_t m_AAFBO = 0, m_AATex = 0;
        bool m_UseAA = false;
    };

} // namespace Engine