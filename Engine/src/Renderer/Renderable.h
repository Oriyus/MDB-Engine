#pragma once
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

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
        // Instanced: count copies, each reading its own per-instance data (see DrawList)
        virtual void DrawInstanced(uint32_t count) const = 0;
        virtual void DrawSubmeshInstanced(uint32_t, uint32_t count) const { DrawInstanced(count); }
        virtual const Material* GetSubmeshMaterial(uint32_t) const { return nullptr; }

        // Triangles in the renderable's own space, for physics (convex hull and mesh colliders).
        // Appends to the vectors; indices count from the positions already there.
        virtual void GetCollisionGeometry(std::vector<glm::vec3>& positions, std::vector<uint32_t>& indices) const {}
        virtual uint32_t TriangleCount() const { return 0; }
    };

} // namespace Engine