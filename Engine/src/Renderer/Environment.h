#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace Engine {

    class Camera;
    class Shader;

    // Image-based lighting: bakes an HDR panorama (or a procedural sky) into the cubemaps
    // the lit shader needs, and draws the sky as the background.
    class Environment {
    public:
        explicit Environment(const std::string& shaderDir);   // folder with the .vert/.frag files, trailing slash
        ~Environment();

        Environment(const Environment&) = delete;
        Environment& operator=(const Environment&) = delete;

        bool LoadHDR(const std::string& path);   // Radiance .hdr, equirectangular
        bool UseProcedural();
        void ReloadSkyShader();

        void DrawSkybox(const Camera& camera, float intensity) const;   // call after the scene, same framebuffer
        void BindLighting(uint32_t irradianceSlot, uint32_t prefilterSlot) const;

        bool  IsReady() const { return m_Irradiance != 0; }
        float PrefilterMaxLod() const { return (float)(kPrefilterLevels - 1); }

        std::string Name = "none";
        bool  Show = true;
        float Blur = 0.0f;               // background blur, as a mip level
        float RotationDegrees = 0.0f;    // spin the environment around Y (affects lighting and background)

    private:
        static constexpr uint32_t kEnvSize = 512;
        static constexpr uint32_t kEnvLevels = 10;       // 1 + log2(512)
        static constexpr uint32_t kIrradianceSize = 32;
        static constexpr uint32_t kPrefilterSize = 128;
        static constexpr uint32_t kPrefilterLevels = 5;

        void Bake(uint32_t equirectTexture, bool procedural);
        void DestroyMaps();

        std::string m_ShaderDir;
        std::unique_ptr<Shader> m_Sky;
        uint32_t m_VAO = 0;                      // core profile needs one bound, even with no vertex data
        uint32_t m_Env = 0, m_Irradiance = 0, m_Prefilter = 0;
    };

} // namespace Engine