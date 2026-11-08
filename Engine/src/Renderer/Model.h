#pragma once
#include "Renderer/Material.h"
#include "Renderer/Mesh.h"
#include "Renderer/Renderable.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <memory>
#include <string>
#include <vector>

struct aiScene;   // assimp (global namespace)

namespace Engine {

    class Model : public Renderable {
    public:
        explicit Model(const std::string& path);

        void Draw() const override;                       // everything, ignoring materials
        void DrawInstanced(uint32_t count) const override;
        void DrawSubmeshInstanced(uint32_t i, uint32_t count) const override;
        bool GetPartVertices(int32_t part, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) const override;
        bool IsValid() const { return !m_Submeshes.empty(); }

        uint32_t SubmeshCount() const override { return (uint32_t)m_Submeshes.size(); }
        void DrawSubmesh(uint32_t i) const override;
        const Material* GetSubmeshMaterial(uint32_t i) const override;

        bool HasTexturedMaterials() const;
        void GetLocalBounds(glm::vec3& mn, glm::vec3& mx) const override { mn = m_Min; mx = m_Max; }
        void GetCollisionGeometry(std::vector<glm::vec3>& positions, std::vector<uint32_t>& indices) const override {
            for (const auto& s : m_Submeshes) s.Geometry->GetCollisionGeometry(positions, indices);
        }
        uint32_t TriangleCount() const override {
            uint32_t n = 0;
            for (const auto& s : m_Submeshes) n += s.Geometry->TriangleCount();
            return n;
        }

        // Matrix that centers the model on the origin and scales its largest side to targetSize
        glm::mat4 GetNormalizeMatrix(float targetSize) const;
        // The same scale, but standing on the origin: centred on x/z, the lowest point at y = 0 (characters, props)
        glm::mat4 GetGroundedMatrix(float targetSize) const;

        // ---- skeletal animation (models whose meshes have bones)
        // A node's local placement in a pose. A pose is one NodePose per skeleton node.
        struct NodePose {
            glm::vec3 T{ 0.0f };
            glm::quat R{ 1.0f, 0.0f, 0.0f, 0.0f };
            glm::vec3 S{ 1.0f };
        };
        bool IsSkinned() const { return !m_Bones.empty(); }
        uint32_t BoneCount() const { return (uint32_t)m_Bones.size(); }
        const std::vector<std::string>& AnimationNames() const { return m_ClipNames; }
        int FindAnimation(const std::string& name) const;   // -1 if there is none by that name
        float AnimationDuration(int clip) const;            // seconds (0 for -1)
        // The pose at `time` seconds into a clip (clip -1: the rest pose). Nodes the clip doesn't
        // animate keep their rest placement.
        void SamplePose(int clip, float time, std::vector<NodePose>& pose) const;
        // Bone matrices for the shader (u_Bones): model space, one per bone
        void PoseToPalette(const std::vector<NodePose>& pose, std::vector<glm::mat4>& palette) const;
        const std::vector<glm::mat4>& RestPalette() const { return m_RestPalette; }   // for a model nobody animates

    private:
        struct Submesh {
            std::unique_ptr<Mesh> Geometry;
            uint32_t MaterialIndex = 0;
        };
        std::vector<Submesh> m_Submeshes;
        std::vector<Material> m_Materials;
        glm::vec3 m_Min{ 0.0f }, m_Max{ 0.0f };
        void LoadSkinned(const aiScene* scene);

        struct SkeletonNode { std::string Name; int Parent = -1; NodePose Rest; };   // parents come first
        struct Bone { int Node = 0; glm::mat4 Offset{ 1.0f }; };   // offset: model space -> bone space at rest
        struct Channel {
            int Node = 0;
            std::vector<std::pair<float, glm::vec3>> T, S;
            std::vector<std::pair<float, glm::quat>> R;
        };
        struct Clip { float Duration = 0.0f; std::vector<Channel> Channels; };
        std::vector<SkeletonNode> m_Nodes;
        std::vector<Bone> m_Bones;
        std::vector<Clip> m_Clips;
        std::vector<std::string> m_ClipNames;
        std::vector<glm::mat4> m_RestPalette;
    };

} // namespace Engine