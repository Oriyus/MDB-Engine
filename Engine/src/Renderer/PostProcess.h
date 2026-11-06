#pragma once
#include <cstdint>

namespace Engine {

    class Shader;

    struct PostSettings {
        float Exposure = 1.0f;
        int   ToneMap = 2;       // 0 = clamp, 1 = Reinhard, 2 = ACES
        bool  Gamma = true;
    };

    // Turns an HDR texture into a displayable 8-bit one
    class PostProcess {
    public:
        PostProcess(uint32_t width, uint32_t height);
        ~PostProcess();

        PostProcess(const PostProcess&) = delete;
        PostProcess& operator=(const PostProcess&) = delete;

        void Resize(uint32_t width, uint32_t height);
        void Apply(uint32_t hdrTexture, Shader& shader, const PostSettings& settings);

        uint32_t GetOutput() const { return m_Tex; }

    private:
        void Invalidate();
        void Destroy();

        uint32_t m_FBO = 0, m_Tex = 0, m_VAO = 0;
        uint32_t m_Width = 1, m_Height = 1;
    };

} // namespace Engine