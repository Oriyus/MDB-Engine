#pragma once
#include <cstdint>
#include <glm/glm.hpp>
#include <vector>

namespace Engine {

    struct Material;
    struct Vertex;

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

        // Full vertices (position, normal, uv) of one part (-1: all of them), for static mesh merging.
        // Appends; indices count from the vertices already there. False if not available (skinned).
        virtual bool GetPartVertices(int32_t part, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) const { return false; }

        // True when the vertices carry wind weights (Vertex::Wind, from a vegetation model's COLOR_0): lit.vert and
        // shadow.vert then sway by them instead of by height (InstanceData::Info.w)
        virtual bool HasVertexWind() const { return false; }
    };

} // namespace Engine