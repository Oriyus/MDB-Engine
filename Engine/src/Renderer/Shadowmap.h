#pragma once
#include <cstdint>

namespace Engine {

    class Shader;

    // Depth cubemaps for several point lights in ONE texture: layer = light index, 6 faces each
    class PointShadowArray {
    public:
        static constexpr uint32_t MaxLights = 4;

        explicit PointShadowArray(uint32_t size = 1024);
        ~PointShadowArray();
        PointShadowArray(const PointShadowArray&) = delete;
        PointShadowArray& operator=(const PointShadowArray&) = delete;

        void BindFace(uint32_t light, uint32_t face) const;   // binds FBO, sets viewport, clears depth
        void BindTexture(uint32_t slot) const;

    private:
        uint32_t m_FBO = 0, m_Tex = 0, m_Size = 0;
    };

    // A single 2D depth map for the sun
    class DirectionalShadowMap {
    public:
        explicit DirectionalShadowMap(uint32_t size = 2048);
        ~DirectionalShadowMap();
        DirectionalShadowMap(const DirectionalShadowMap&) = delete;
        DirectionalShadowMap& operator=(const DirectionalShadowMap&) = delete;

        void Bind() const;
        void BindTexture(uint32_t slot) const;
        uint32_t GetSize() const { return m_Size; }

    private:
        uint32_t m_FBO = 0, m_Tex = 0, m_Size = 0;
    };

    struct ShadowSettings {
        bool  Enabled = true;
        float PointBias = 0.05f;      // world units
        float PointFar = 30.0f;       // how far point-light shadows reach
        float SunBias = 0.002f;       // depth units (0..1 across the sun's range)
        float SunExtent = 12.0f;      // half-width of the area around the origin the sun shadows cover
        float SunDistance = 30.0f;    // how far back from the origin the sun "camera" sits
    };

    // Everything the scene needs to render shadows, handed over by the app
    struct ShadowContext {
        PointShadowArray* Points = nullptr;
        DirectionalShadowMap* Sun = nullptr;
        Shader* PointDepth = nullptr;
        Shader* SunDepth = nullptr;
        ShadowSettings Settings;
    };

} // namespace Engine