#pragma once
#include "Renderer/Renderable.h"
#include "Renderer/VertexArray.h"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace Engine {

    struct Vertex {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
        glm::vec2 TexCoord{ 0.0f };
    };
    static_assert(sizeof(Vertex) == 8 * sizeof(float), "Vertex must be tightly packed");

    // A vertex moved by up to four bones (skeletal animation). Shader locations 3 and 4.
    struct SkinnedVertex {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
        glm::vec2 TexCoord{ 0.0f };
        glm::ivec4 Bones{ 0 };            // indices into the bone palette
        glm::vec4 Weights{ 0.0f };        // sum to 1
    };
    static_assert(sizeof(SkinnedVertex) == 16 * 4, "SkinnedVertex must be tightly packed");

    class Mesh : public Renderable {
    public:
        Mesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
        // A skinned mesh. restPositions are the vertices as the model stands without animation:
        // they are used for bounds and colliders (the GPU gets the raw, unskinned positions).
        Mesh(const std::vector<SkinnedVertex>& vertices, const std::vector<uint32_t>& indices,
            const std::vector<glm::vec3>& restPositions);
        static std::shared_ptr<Mesh> CreateSphere(uint32_t segments = 32);
        static std::shared_ptr<Mesh> CreateCapsule(uint32_t segments = 32);    // 1 wide, 2 tall
        static std::shared_ptr<Mesh> CreateCylinder(uint32_t segments = 32);   // 1 wide, 1 tall
        static std::shared_ptr<Mesh> CreateCone(uint32_t segments = 32);       // 1 wide base, 1 tall

        void Draw() const override;
        void DrawInstanced(uint32_t count) const override;
        void GetLocalBounds(glm::vec3& mn, glm::vec3& mx) const override { mn = m_Min; mx = m_Max; }
        void GetCollisionGeometry(std::vector<glm::vec3>& positions, std::vector<uint32_t>& indices) const override;
        uint32_t TriangleCount() const override { return (uint32_t)(m_Indices.size() / 3); }
        bool GetPartVertices(int32_t part, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) const override;

        // Unit cube centered on the origin (1x1x1)
        static std::shared_ptr<Mesh> CreateCube();
        // Flat square on the XZ plane facing +Y. UVs run 0..uvRepeat.
        static std::shared_ptr<Mesh> CreatePlane(float size, float uvRepeat = 1.0f);

    private:
        std::unique_ptr<VertexArray> m_VAO;
        glm::vec3 m_Min{ -0.5f }, m_Max{ 0.5f };
        std::vector<glm::vec3> m_Positions;   // a CPU copy for physics colliders
        std::vector<uint32_t> m_Indices;
        std::vector<Vertex> m_Vertices;       // a CPU copy for static merging (empty for skinned meshes)
    };

} // namespace Engine