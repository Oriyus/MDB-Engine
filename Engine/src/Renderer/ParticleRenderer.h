#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Engine {

    class Camera;
    class Shader;
    class Texture2D;

    // One particle as the GPU reads it (std430, storage buffer binding 2)
    struct ParticleInstance {
        glm::vec4 PositionSize{ 0.0f };   // xyz world position, w size (metres)
        glm::vec4 Color{ 1.0f };          // linear rgb (emission included), alpha
        glm::vec4 Extra{ 0.0f };          // x rotation (radians), y sprite-sheet frame
    };
    static_assert(sizeof(ParticleInstance) == 48, "must match particle.vert");

    // Everything one emitter draws this frame, built by Scene::GatherParticles
    struct ParticleDraw {
        std::vector<ParticleInstance> Instances;   // alpha-blended ones already back to front
        const Texture2D* Texture = nullptr;        // none: a soft round dot
        bool Additive = false;
        float Softness = 0.0f;                     // metres: fade near the scene behind (0 = hard edges)
        glm::ivec2 Sheet{ 1, 1 };                  // the texture's frames: columns, rows
        float Distance = 0.0f;                     // emitter to camera, for ordering emitters
    };

    // Draws camera-facing particle quads, one instanced call per emitter, after the see-through pass:
    // depth tested but not written, premultiplied blending (additive or over). Soft particles read a copy of the
    // scene depth and fade where they near it (ParticleDraw::Softness).
    class ParticleRenderer {
    public:
        ParticleRenderer();
        ~ParticleRenderer();
        ParticleRenderer(const ParticleRenderer&) = delete;
        ParticleRenderer& operator=(const ParticleRenderer&) = delete;

        // draws: sorted here (farthest first). The HDR framebuffer must be bound. depthTex: a copy of the scene depth
        // (not the attached one), or 0 to draw every particle with hard edges.
        void Render(std::vector<ParticleDraw>& draws, const Camera& camera, Shader& shader, uint32_t depthTex = 0);

    private:
        uint32_t m_Buffer = 0, m_VAO = 0;
        size_t m_Capacity = 0;
    };

} // namespace Engine
