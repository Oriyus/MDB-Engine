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

    class Mesh : public Renderable {
    public:
        Mesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
        static std::shared_ptr<Mesh> CreateSphere(uint32_t segments = 32);

        void Draw() const override;
        void GetLocalBounds(glm::vec3& mn, glm::vec3& mx) const override { mn = m_Min; mx = m_Max; }

        // Unit cube centered on the origin (1x1x1)
        static std::shared_ptr<Mesh> CreateCube();
        // Flat square on the XZ plane facing +Y. UVs run 0..uvRepeat.
        static std::shared_ptr<Mesh> CreatePlane(float size, float uvRepeat = 1.0f);

    private:
        std::unique_ptr<VertexArray> m_VAO;
        glm::vec3 m_Min{ -0.5f }, m_Max{ 0.5f };
    };

} // namespace Engine