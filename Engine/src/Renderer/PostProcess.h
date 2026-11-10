#pragma once
#include <glm/glm.hpp>
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

        // Colour grading (tonemap.frag). The neutral values below change nothing.
        float Contrast = 1.0f;                // around mid grey, before the tone map: > 1 = deeper shadows, brighter lights
        float Saturation = 1.0f;              // 0 = grey, > 1 = richer colour
        float Temperature = 0.0f;             // white balance: -1 cool (blue) .. 1 warm (orange)
        float Tint = 0.0f;                    // -1 green .. 1 magenta
        glm::vec3 ShadowTint{ 1.0f };         // colour pushed into the dark parts (sRGB-style, 1 = none)
        glm::vec3 HighlightTint{ 1.0f };      // ... and into the bright parts ("teal and orange": cool shadows, warm lights)
        float Vignette = 0.0f;                // 0..1: darker towards the corners
        float Grain = 0.0f;                   // film grain, about 0.02 is visible
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
        uint32_t GetOutputFramebuffer() const { return m_UseAA ? m_AAFBO : m_FBO; }   // to draw overlays onto the result
    private:
        void Invalidate();
        void Destroy();

        uint32_t m_FBO = 0, m_Tex = 0, m_VAO = 0;
        uint32_t m_Width = 1, m_Height = 1;
        uint32_t m_AAFBO = 0, m_AATex = 0;
        bool m_UseAA = false;
        uint32_t m_Frame = 0;       // for the film grain pattern
    };

} // namespace Engine