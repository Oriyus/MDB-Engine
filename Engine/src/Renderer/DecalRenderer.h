#pragma once
#include "Scene/Decal.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Engine {

    class Camera;
    class Shader;

    // Draws Scene::Decals over the colour target that is bound (the HDR framebuffer), after the opaque
    // scene: one instanced draw per texture, each decal a box whose pixels are projected from the depth
    // buffer (Shaders/decal.*). Blended over the scene, no depth writes, the entity-ID target untouched.
    class DecalRenderer {
    public:
        DecalRenderer();
        ~DecalRenderer();
        DecalRenderer(const DecalRenderer&) = delete;
        DecalRenderer& operator=(const DecalRenderer&) = delete;

        // depthCopy: a texture holding the scene depth (not the attached one: sampling that is a feedback loop)
        void Render(const std::vector<Decal>& decals, const Camera& camera, uint32_t depthCopy,
            uint32_t width, uint32_t height, Shader& shader);

    private:
        uint32_t m_Buffer = 0, m_VAO = 0;
        size_t m_Capacity = 0;
    };

} // namespace Engine
