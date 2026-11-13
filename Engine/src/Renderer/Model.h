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
        // The file's collision mesh if it has one (nodes named UCX_* / *_COL or with glTF extras "collision"), else
        // the drawn geometry. Used by Convex hull / Mesh colliders and grass footprints.
        void GetCollisionGeometry(std::vector<glm::vec3>& positions, std::vector<uint32_t>& indices) const override {
            if (!m_CollisionIndices.empty()) {
                const uint32_t base = (uint32_t)positions.size();
                positions.insert(positions.end(), m_CollisionPositions.begin(), m_CollisionPositions.end());
                for (uint32_t i : m_CollisionIndices) indices.push_back(base + i);
                return;
            }
            for (const auto& s : m_Submeshes) s.Geometry->GetCollisionGeometry(positions, indices);
        }
        bool HasCollisionMesh() const { return !m_CollisionIndices.empty(); }
        // Vegetation: the vertices carry wind weights from COLOR_0 (Vertex::Wind); its levels of detail do too
        bool HasVertexWind() const override { return m_VertexWind; }
        // How the file's COLOR_0 was read: Tint (architecture), Wind (vegetation) or Ignored (none / other category)
        enum class VertexColorUse { Ignored, Tint, Wind };

        // Levels of detail inside the file (nodes named *_LOD<n>, or glTF extras "lod": n). This model is the finest
        // level (with the meshes no level claims); Lod(1) .. Lod(LodCount()) are the coarser ones, in the same space
        // and with the same materials. They are named "model:<path>#lod<k>" in scene files (ResourceLibrary).
        // LodDistance(k): the file's switch distance hint (extras "lod_distance", file units), 0 = none.
        uint32_t LodCount() const { return (uint32_t)m_Lods.size(); }
        const std::shared_ptr<Model>& Lod(uint32_t k) const { return m_Lods[k - 1]; }
        float LodDistance(uint32_t k) const { return k >= 1 && k <= m_LodDistances.size() ? m_LodDistances[k - 1] : 0.0f; }
        uint32_t TriangleCount() const override {
            uint32_t n = 0;
            for (const auto& s : m_Submeshes) n += s.Geometry->TriangleCount();
            return n;
        }

        // Matrix that centers the model on the origin and scales its largest side to targetSize
        glm::mat4 GetNormalizeMatrix(float targetSize) const;
        // The same scale, but standing on the origin: centred on x/z, the lowest point at y = 0 (characters, props)
        glm::mat4 GetGroundedMatrix(float targetSize) const;
        // Kit pieces (a model in a folder holding kit.json, set by ResourceLibrary) are already in metres with their
        // own pivot, so they snap together: placing one keeps the file's size and origin
        bool NativeSize = false;
        // How the editor places the model: identity for kit pieces, else GetGroundedMatrix(targetSize)
        glm::mat4 GetPlacementMatrix(float targetSize) const { return NativeSize ? glm::mat4(1.0f) : GetGroundedMatrix(targetSize); }

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
        // The same in two steps: each node in model space, then the bone matrices (IK edits in between)
        void PoseToGlobals(const std::vector<NodePose>& pose, std::vector<glm::mat4>& globals) const;
        void GlobalsToPalette(const std::vector<glm::mat4>& globals, std::vector<glm::mat4>& palette) const;
        int NodeParent(int node) const { return node >= 0 && node < (int)m_Nodes.size() ? m_Nodes[node].Parent : -1; }
        const std::vector<glm::mat4>& RestPalette() const { return m_RestPalette; }   // for a model nobody animates
        // Skeleton nodes, for animation layer masks: names (parents first), lookup, and a node's subtree
        std::vector<std::string> NodeNames() const;
        int FindNode(const std::string& name) const;      // -1 if missing
        void SubtreeMask(int root, std::vector<uint8_t>& mask) const;   // 1 for root and its descendants
        // Root motion: the topmost node a clip translates (-1: none), that node's translation at a time,
        // and the rest transform of its parent chain (node space -> model space)
        int RootMotionNode(int clip) const;
        glm::vec3 SampleNodeTranslation(int clip, int node, float time) const;
        glm::mat4 ParentRestGlobal(int node) const;

    private:
        Model() = default;                 // a level of detail, filled by the file's Model
        std::vector<std::shared_ptr<Model>> m_Lods;
        std::vector<float> m_LodDistances;
        std::vector<glm::vec3> m_CollisionPositions;
        std::vector<uint32_t> m_CollisionIndices;

        struct Submesh {
            std::unique_ptr<Mesh> Geometry;
            uint32_t MaterialIndex = 0;
        };
        std::vector<Submesh> m_Submeshes;
        std::vector<Material> m_Materials;
        bool m_VertexWind = false;
        glm::vec3 m_Min{ 0.0f }, m_Max{ 0.0f };
        void LoadSkinned(const aiScene* scene, VertexColorUse colors);

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