#pragma once
#include <array>
#include <cstdint>

namespace Engine {

    class Shader;

    class Bloom {
    public:
        static constexpr uint32_t Levels = 6;

        Bloom(uint32_t width, uint32_t height);   // the size of the HDR image it will read
        ~Bloom();

        Bloom(const Bloom&) = delete;
        Bloom& operator=(const Bloom&) = delete;

        void Resize(uint32_t width, uint32_t height);
        void Compute(uint32_t hdrTexture, Shader& downShader, Shader& upShader);

        uint32_t GetOutput() const { return m_Levels[0].Tex; }

        float Threshold = 1.0f;   // brightness above which pixels start to glow
        float Knee = 0.5f;   // softness of the cutoff
        float Radius = 1.0f;   // spread of the glow
        float MaxBrightness = 8.0f;   // per-pixel ceiling before blurring; lower = fewer sparkles

    private:
        struct Level { uint32_t Tex = 0, FBO = 0, W = 1, H = 1; };

        void Invalidate();
        void Destroy();

        std::array<Level, Levels> m_Levels;
        uint32_t m_VAO = 0;
        uint32_t m_Width = 1, m_Height = 1;
    };

} // namespace Engine