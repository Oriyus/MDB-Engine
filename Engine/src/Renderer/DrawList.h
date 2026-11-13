#pragma once
#include "Renderer/Material.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Engine {

    class Renderable;
    class Texture2D;

    // Per-object data for one drawn part, stored once per frame in a storage buffer (binding 0).
    // Every view refers to it through an index list (binding 1): lit.vert / shadow.vert read
    // u_Instances[u_Indices[u_InstanceBase + gl_InstanceID]]. Layout matches std430.
    struct InstanceData {
        glm::mat4  Model{ 1.0f };
        glm::mat4  Normal{ 1.0f };          // transpose(inverse(mat3(Model))), padded to a mat4
        glm::vec4  Color{ 1.0f };           // rgb: sRGB-style colour, a: alpha (opacity x material alpha)
        glm::vec4  Surface{ 0.0f };         // x: metallic, y: roughness
        glm::vec4  Emissive{ 0.0f };        // rgb: linear emissive
        glm::ivec4 Info{ 0 };               // x: entity ID (picking), y: unlit, z: 1000 x (1 - specular), w: 1 = vertex wind weights
        glm::mat4  PrevModel{ 1.0f };       // last frame's Model (motion vectors); Scene::BuildDrawList fills it
    };
    static_assert(sizeof(InstanceData) == 256, "InstanceData must match the std430 layout in the shaders");

    // A run of instances drawn with one instanced call: same geometry part, textures and alpha settings
    struct DrawBatch {
        const Renderable* Geometry = nullptr;
        int32_t Part = -1;                  // -1: the whole renderable (Draw), else DrawSubmesh(Part)
        const Texture2D* Albedo = nullptr;
        const Texture2D* Normal = nullptr;
        const Texture2D* MetalRough = nullptr;
        const Texture2D* Emissive = nullptr;
        AlphaMode Mode = AlphaMode::Opaque;
        float Cutoff = 0.5f;
        bool DoubleSided = false;           // back faces drawn too (culling off for this batch)
        const std::vector<glm::mat4>* Bones = nullptr;   // skinned: this entity's bone palette (never shared)
        const TerrainMaterial* Terrain = nullptr;        // a terrain chunk: lit.frag blends its layers (null in shadow lists)

        uint32_t First = 0;                 // into DrawList::Indices
        uint32_t Count = 0;

        bool SameState(const DrawBatch& o) const {
            return Geometry == o.Geometry && Part == o.Part && Albedo == o.Albedo && Normal == o.Normal &&
                MetalRough == o.MetalRough && Emissive == o.Emissive && Mode == o.Mode && Cutoff == o.Cutoff &&
                DoubleSided == o.DoubleSided &&
                Bones == o.Bones && Terrain == o.Terrain;
        }
        bool operator<(const DrawBatch& o) const;   // groups equal states together
    };

    // One shadow view of the frame: a point-light cube face, or a sun cascade (Light = -1, Face = cascade)
    struct ShadowView {
        glm::mat4 ViewProj{ 1.0f };
        int Light = -1;                     // index into the point lights, -1 = the sun
        int Face = 0;                       // cube face, or the sun cascade
        float TexelWorld = 0.0f;            // sun: size of one shadow-map texel in world units
        float DepthRange = 1.0f;            // sun: world distance covered by depth 0..1
    };

    // Everything one frame draws, built by Scene::BuildDrawList and uploaded once:
    //   Opaque       opaque and cutout parts inside the camera's view, grouped into instanced batches
    //   Transparent  see-through parts inside the view, back to front (equal neighbours still merge)
    //   ShadowViews  per shadow view (each point-light cube face, then the sun): the casters inside it
    class DrawList {
    public:
        DrawList();
        ~DrawList();
        DrawList(const DrawList&) = delete;
        DrawList& operator=(const DrawList&) = delete;

        // A part to draw in one view: its state (First/Count ignored) and which instance it is
        struct Item { DrawBatch State; uint32_t Instance = 0; float Distance = 0.0f; };
        enum class Order { ByState, BackToFront };

        void Clear();
        uint32_t AddInstance(const InstanceData& d) { Instances.push_back(d); return (uint32_t)Instances.size() - 1; }
        // Sort a view's items and append them as batches (with their indices) to out
        void AddBatches(std::vector<Item>& items, std::vector<DrawBatch>& out, Order order);
        void Upload();                      // instances and indices to the GPU
        void Bind() const;                  // storage buffer bindings 0 and 1

        std::vector<InstanceData> Instances;
        std::vector<uint32_t> Indices;
        std::vector<DrawBatch> Opaque, Transparent;
        std::vector<DrawBatch> Selected;    // the highlighted parts inside the view (editor outline), albedo only for alpha tests
        // Input, kept across Clear: entity handles (entt integral, sorted) to collect into Selected. Null or empty: none.
        const std::vector<uint32_t>* Highlight = nullptr;
        // Motion vectors: each drawn entity's world matrix (with LocalOffset) last frame and this frame, by entt handle.
        // Kept across Clear; BuildDrawList swaps them. A part not drawn last frame gets PrevModel = Model (no motion).
        std::unordered_map<uint32_t, glm::mat4> PrevWorld, CurWorld;
        std::vector<std::vector<DrawBatch>> ShadowViews;    // the casters in each of Views
        std::vector<ShadowView> Views;      // this frame's shadow views (each point-light face, then the sun cascades)
        glm::vec4 CascadeSplits{ 0.0f };    // view depth where each sun cascade ends
        int CascadeCount = 0;               // 0: no sun shadows this frame
        uint32_t Culled = 0;                // objects outside the camera view
        uint32_t ShadowSkipped = 0;         // (caster, shadow view) pairs skipped: outside that light's view
        uint32_t LodReduced = 0;            // objects drawn at a coarser level of detail (MeshRendererComponent::Lods)
        uint32_t LodCulled = 0;             // objects beyond their CullDistance

    private:
        uint32_t m_Buffers[2] = { 0, 0 };   // instances, indices
        std::vector<uint32_t> m_SortOrder;  // AddBatches scratch
        size_t m_Capacity[2] = { 0, 0 };    // bytes
    };

} // namespace Engine
