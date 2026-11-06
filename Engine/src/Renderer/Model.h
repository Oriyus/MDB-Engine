#pragma once
#include "Renderer/Material.h"
#include "Renderer/Mesh.h"
#include "Renderer/Renderable.h"
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>

namespace Engine {

    class Model : public Renderable {
    public:
        explicit Model(const std::string& path);

        void Draw() const override;                       // everything, ignoring materials
        bool IsValid() const { return !m_Submeshes.empty(); }

        uint32_t SubmeshCount() const override { return (uint32_t)m_Submeshes.size(); }
        void DrawSubmesh(uint32_t i) const override;
        const Material* GetSubmeshMaterial(uint32_t i) const override;

        bool HasTexturedMaterials() const;
        void GetLocalBounds(glm::vec3& mn, glm::vec3& mx) const override { mn = m_Min; mx = m_Max; }

        // Matrix that centers the model on the origin and scales its largest side to targetSize
        glm::mat4 GetNormalizeMatrix(float targetSize) const;

    private:
        struct Submesh {
            std::unique_ptr<Mesh> Geometry;
            uint32_t MaterialIndex = 0;
        };
        std::vector<Submesh> m_Submeshes;
        std::vector<Material> m_Materials;
        glm::vec3 m_Min{ 0.0f }, m_Max{ 0.0f };
    };

} // namespace Engine