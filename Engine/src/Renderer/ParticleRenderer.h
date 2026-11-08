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
        glm::vec4 Extra{ 0.0f };          // x rotation (radians)
    };
    static_assert(sizeof(ParticleInstance) == 48, "must match particle.vert");

    // Everything one emitter draws this frame, built by Scene::GatherParticles
    struct ParticleDraw {
        std::vector<ParticleInstance> Instances;   // alpha-blended ones already back to front
        const Texture2D* Texture = nullptr;        // none: a soft round dot
        bool Additive = false;
        float Distance = 0.0f;                     // emitter to camera, for ordering emitters
    };

    // Draws camera-facing particle quads, one instanced call per emitter, after the see-through pass:
    // depth tested but not written, premultiplied blending (additive or over).
    class ParticleRenderer {
    public:
        ParticleRenderer();
        ~ParticleRenderer();
        ParticleRenderer(const ParticleRenderer&) = delete;
        ParticleRenderer& operator=(const ParticleRenderer&) = delete;

        // draws: sorted here (farthest first). The HDR framebuffer must be bound.
        void Render(std::vector<ParticleDraw>& draws, const Camera& camera, Shader& shader);

    private:
        uint32_t m_Buffer = 0, m_VAO = 0;
        size_t m_Capacity = 0;
    };

} // namespace Engine
