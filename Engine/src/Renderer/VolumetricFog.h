#pragma once
#include <glm/glm.hpp>
#include <chrono>
#include <cstdint>

namespace Engine {

    class Scene;
    class Camera;
    class Shader;
    class DrawList;
    class Environment;
    struct ShadowContext;

    // Height fog lit by the sun (through its shadow cascades: light shafts), the sky and point lights, with
    // drifting wisps. Part of SceneSettings ("volumetricFog" in scene files).
    struct VolumetricFogSettings {
        bool  Enabled = false;
        float Density = 0.04f;            // extinction per metre at and below BaseHeight
        float HeightFalloff = 3.0f;       // metres: above BaseHeight the density drops to 37% every this many metres
        float BaseHeight = 0.0f;
        glm::vec3 Color{ 0.86f, 0.89f, 0.93f };   // sRGB-style: the fog's colour (how it tints the light it scatters)
        float Anisotropy = 0.6f;          // -1..1: 0 scatters evenly, towards 1 bright looking towards the sun
        float SunIntensity = 1.0f;        // light shafts
        float AmbientIntensity = 1.0f;    // sky light in the fog
        float PointLightIntensity = 1.0f; // glow around lamps
        float NoiseScale = 0.12f;         // wisps per metre (smaller = bigger wisps)
        float NoiseAmount = 0.6f;         // 0 = even fog, 1 = patchy
        float WindSpeed = 0.5f;           // metres per second the wisps drift
        float WindDirection = 30.0f;      // degrees around Y; 0 = towards +X, 90 = towards +Z
        float MaxDistance = 80.0f;        // metres marched along each view ray (the sky gets this much fog)
        int   Steps = 32;                 // samples per ray (8..96)
    };

    // Raymarches the fog at half resolution (volfog.frag, jittered steps, the sun cascades for shadowing), then
    // blends it into the bound target with a depth-aware upsample (volfog_composite.frag), so edges against
    // nearby walls stay sharp.
    class VolumetricFog {
    public:
        VolumetricFog();
        ~VolumetricFog();
        VolumetricFog(const VolumetricFog&) = delete;
        VolumetricFog& operator=(const VolumetricFog&) = delete;

        // depthTex: the scene depth (full resolution, a copy that is not attached). Draws into the target bound now.
        void Render(Scene& scene, const Camera& camera, Shader& march, Shader& composite, const DrawList& list,
                    const ShadowContext* shadows, const Environment* env, uint32_t depthTex,
                    uint32_t width, uint32_t height, const VolumetricFogSettings& settings);

    private:
        void Resize(uint32_t width, uint32_t height);
        uint32_t m_FBO = 0, m_Texture = 0, m_VAO = 0;
        uint32_t m_Width = 0, m_Height = 0;     // the half-resolution target
        std::chrono::steady_clock::time_point m_Last = std::chrono::steady_clock::now();
        float m_Time = 0.0f;
    };

} // namespace Engine
