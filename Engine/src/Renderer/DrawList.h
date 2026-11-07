#pragma once
#include "Renderer/Material.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace Engine {

    class Renderable;
    class Texture2D;

    // Per-object data for one drawn part, read by lit.vert / shadow.vert from a storage buffer
    // (binding 0) as u_Instances[u_InstanceBase + gl_InstanceID]. Layout matches std430.
    struct InstanceData {
        glm::mat4  Model{ 1.0f };
        glm::mat4  Normal{ 1.0f };          // transpose(inverse(mat3(Model))), padded to a mat4
        glm::vec4  Color{ 1.0f };           // rgb: sRGB-style colour, a: alpha (opacity x material alpha)
        glm::vec4  Surface{ 0.0f };         // x: metallic, y: roughness
        glm::vec4  Emissive{ 0.0f };        // rgb: linear emissive
        glm::ivec4 Info{ 0 };               // x: entity ID (picking), y: unlit
    };
    static_assert(sizeof(InstanceData) == 192, "InstanceData must match the std430 layout in the shaders");

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
        const std::vector<glm::mat4>* Bones = nullptr;   // skinned: this entity's bone palette (never shared)

        uint32_t First = 0;                 // into DrawList::Instances
        uint32_t Count = 0;

        bool SameState(const DrawBatch& o) const {
            return Geometry == o.Geometry && Part == o.Part && Albedo == o.Albedo && Normal == o.Normal &&
                MetalRough == o.MetalRough && Emissive == o.Emissive && Mode == o.Mode && Cutoff == o.Cutoff &&
                Bones == o.Bones;
        }
        bool operator<(const DrawBatch& o) const;   // groups equal states together
    };

    // Everything one frame draws, built by Scene::BuildDrawList and uploaded once:
    //   Opaque      opaque and cutout parts inside the camera's view, grouped into instanced batches
    //   Transparent see-through parts inside the view, back to front (equal neighbours still merge)
    //   Shadow      every shadow caster (not culled: lights see more than the camera)
    class DrawList {
    public:
        DrawList();
        ~DrawList();
        DrawList(const DrawList&) = delete;
        DrawList& operator=(const DrawList&) = delete;

        // A part to draw: its state (First/Count ignored) and its instance data
        struct Item { DrawBatch State; InstanceData Instance; float Distance = 0.0f; };

        void Clear();
        // Turn the collected items into batches and instances (call once, after adding everything)
        void Finish(std::vector<Item>& opaque, std::vector<Item>& transparent, std::vector<Item>& shadow);
        void Upload();                      // to the GPU buffer
        void Bind() const;                  // storage buffer binding 0

        std::vector<InstanceData> Instances;
        std::vector<DrawBatch> Opaque, Transparent, Shadow;
        uint32_t Culled = 0;                // parts skipped by the view frustum this frame

    private:
        uint32_t m_Buffer = 0;
        size_t m_Capacity = 0;              // bytes
    };

} // namespace Engine
