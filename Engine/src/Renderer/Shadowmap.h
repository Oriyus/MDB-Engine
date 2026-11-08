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

    // The sun's depth maps: one layer per cascade (a 2D array texture). Cascades cover slices of the
    // camera view, near ones sharper; with cascades off only layer 0 is used, for a fixed area.
    class DirectionalShadowMap {
    public:
        static constexpr uint32_t MaxCascades = 4;

        explicit DirectionalShadowMap(uint32_t size = 2048);
        ~DirectionalShadowMap();
        DirectionalShadowMap(const DirectionalShadowMap&) = delete;
        DirectionalShadowMap& operator=(const DirectionalShadowMap&) = delete;

        void Bind(uint32_t cascade) const;                    // binds FBO to that layer, sets viewport, clears depth
        void BindTexture(uint32_t slot) const;
        uint32_t GetSize() const { return m_Size; }

    private:
        uint32_t m_FBO = 0, m_Tex = 0, m_Size = 0;
    };

    struct ShadowSettings {
        bool  Enabled = true;
        float PointBias = 0.05f;      // world units
        float PointFar = 30.0f;       // how far point-light shadows reach
        float SunBias = 0.002f;       // sun depth bias; scaled to each cascade's texel size (0.002 = default)
        // Cascaded sun shadows follow the camera: CascadeCount slices up to ShadowDistance from it
        bool  SunCascades = true;
        int   CascadeCount = 4;         // 1..4
        float ShadowDistance = 60.0f;   // how far from the camera sun shadows reach
        float CascadeSplit = 0.75f;     // 0 = even slices, 1 = logarithmic (more detail near the camera)
        bool  CascadeDebug = false;     // tint each cascade (a debug view, not saved)
        // Without cascades: a fixed area around the origin
        float SunExtent = 12.0f;      // half-width of the area around the origin the sun shadows cover
        float SunDistance = 30.0f;    // how far back the sun "camera" sits (casters this far behind still count)
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