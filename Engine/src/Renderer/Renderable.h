#pragma once
#include <cstdint>
#include <glm/glm.hpp>

namespace Engine {

    struct Material;

    class Renderable {
    public:
        virtual ~Renderable() = default;
        virtual void Draw() const = 0;

        // Axis-aligned box in the renderable's own space
        virtual void GetLocalBounds(glm::vec3& mn, glm::vec3& mx) const {
            mn = glm::vec3(-0.5f);
            mx = glm::vec3(0.5f);
        }

        // Renderables made of several parts, each with its own material (models).
        // Simple meshes keep these defaults: one part, no material.
        virtual uint32_t SubmeshCount() const { return 1; }
        virtual void DrawSubmesh(uint32_t) const { Draw(); }
        virtual const Material* GetSubmeshMaterial(uint32_t) const { return nullptr; }
    };

} // namespace Engine